#include "observer_hook.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>

extern "C" {
alignas(8) std::uintptr_t taxi_engine_hook_original = 0;
void taxi_engine_hook_thunk();
#ifdef TAXI_ENGINE_HOOK_VALIDATION
BOOL taxi_hook_test_virtual_protect(void*, SIZE_T, DWORD, PDWORD) noexcept;
#endif
}

namespace {
using namespace taxi_camera::engine_hook;

SRWLOCK control_lock = SRWLOCK_INIT;
std::atomic<Observer> saved_observer{nullptr};
void** saved_slot = nullptr;
void** pending_protection_slot = nullptr;
DWORD pending_protection = 0;
bool consumed = false;
bool removed = false;
ImageDataSlotProof saved_image_data{};
bool has_image_data = false;
bool installed_copy_on_write = false;
thread_local bool observing = false;

struct Lock {
  Lock() noexcept { AcquireSRWLockExclusive(&control_lock); }
  ~Lock() { ReleaseSRWLockExclusive(&control_lock); }
};

bool readable_protection(DWORD protection) noexcept {
  if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
    return false;
  switch (protection & 0xff) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
      return true;
    default:
      return false;
  }
}

// Header-only read: no raw dereference, no protection changes, and no section
// content reads. All metadata must fit within the first 64 KiB of this image.
bool image_header(HMODULE image, std::uint32_t rva, void* output, std::size_t size) noexcept {
  constexpr std::uint32_t header_limit = 65536;
  const auto base = reinterpret_cast<std::uintptr_t>(image);
  if (rva > header_limit || size > header_limit - rva || base > std::numeric_limits<std::uintptr_t>::max() - rva)
    return false;
  const auto address = base + rva;
  MEMORY_BASIC_INFORMATION region{};
  if (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
      region.Type != MEM_IMAGE || region.AllocationBase != image || !readable_protection(region.Protect))
    return false;
  const auto first = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
  if (region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - first || address < first || address > first + region.RegionSize ||
      size > first + region.RegionSize - address)
    return false;
  SIZE_T copied = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), output, size, &copied) != FALSE && copied == size;
}

bool verified_image_data(void** slot, const MEMORY_BASIC_INFORMATION& region, const ImageDataSlotProof* proof) noexcept {
  if (proof == nullptr || proof->main_image == nullptr || proof->main_image != GetModuleHandleW(nullptr) || region.Type != MEM_IMAGE ||
      region.AllocationBase != proof->main_image || proof->image_size == 0 || proof->image_size > 0x80000000u || proof->section_size == 0 ||
      proof->section_rva > proof->image_size || proof->section_size > proof->image_size - proof->section_rva)
    return false;
  const auto base = reinterpret_cast<std::uintptr_t>(proof->main_image);
  const auto address = reinterpret_cast<std::uintptr_t>(slot);
  if (base > std::numeric_limits<std::uintptr_t>::max() - proof->image_size || address < base || address - base < proof->section_rva ||
      address - base - proof->section_rva > proof->section_size ||
      sizeof(void*) > proof->section_size - (address - base - proof->section_rva))
    return false;
  IMAGE_DOS_HEADER dos{};
  if (!image_header(proof->main_image, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 ||
      dos.e_lfanew > 65536 - 24)
    return false;
  struct NtPrefix {
    DWORD signature;
    IMAGE_FILE_HEADER file;
  } nt{};
  static_assert(sizeof(nt) == 24);
  const auto nt_rva = static_cast<std::uint32_t>(dos.e_lfanew);
  if (!image_header(proof->main_image, nt_rva, &nt, sizeof(nt)) || nt.signature != IMAGE_NT_SIGNATURE ||
      nt.file.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.file.NumberOfSections == 0 || nt.file.NumberOfSections > 96 ||
      nt.file.SizeOfOptionalHeader < offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) || nt.file.SizeOfOptionalHeader > 4096)
    return false;
  IMAGE_OPTIONAL_HEADER64 optional{};
  if (!image_header(proof->main_image, nt_rva + sizeof(nt), &optional, offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory)) ||
      optional.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || optional.SizeOfImage != proof->image_size || optional.SizeOfHeaders > 65536 ||
      optional.SizeOfHeaders > optional.SizeOfImage)
    return false;
  const auto sections_rva = nt_rva + static_cast<std::uint32_t>(sizeof(nt)) + nt.file.SizeOfOptionalHeader;
  const auto sections_size = nt.file.NumberOfSections * static_cast<std::uint32_t>(sizeof(IMAGE_SECTION_HEADER));
  if (sections_rva > optional.SizeOfHeaders || sections_size > optional.SizeOfHeaders - sections_rva)
    return false;
  IMAGE_SECTION_HEADER sections[96]{};
  if (!image_header(proof->main_image, sections_rva, sections, sections_size))
    return false;
  bool matched = false;
  for (unsigned i = 0; i < nt.file.NumberOfSections; ++i) {
    const auto& section = sections[i];
    const auto extent = section.Misc.VirtualSize != 0 ? section.Misc.VirtualSize : section.SizeOfRawData;
    if (extent == 0)
      continue;
    if (section.VirtualAddress < optional.SizeOfHeaders || section.VirtualAddress > optional.SizeOfImage ||
        extent > optional.SizeOfImage - section.VirtualAddress)
      return false;
    for (unsigned j = 0; j < i; ++j) {
      const auto previous_extent = sections[j].Misc.VirtualSize != 0 ? sections[j].Misc.VirtualSize : sections[j].SizeOfRawData;
      if (previous_extent != 0 && section.VirtualAddress < sections[j].VirtualAddress + previous_extent &&
          sections[j].VirtualAddress < section.VirtualAddress + extent)
        return false;
    }
    if (section.VirtualAddress == proof->section_rva && extent == proof->section_size) {
      constexpr DWORD refused = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_DISCARDABLE;
      if ((section.Characteristics & IMAGE_SCN_MEM_READ) == 0 || (section.Characteristics & refused) != 0)
        return false;
      matched = true;
    }
  }
  return matched;
}

bool slot_memory(void** slot, const ImageDataSlotProof* proof, DWORD& writable_protection, bool owned_copy_on_write = false) noexcept {
  MEMORY_BASIC_INFORMATION region{};
  if (VirtualQuery(slot, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
      (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
    return false;
  const auto protection = region.Protect & 0xff;
  // Executable pages remain refused by default. The one explicit exception is
  // a main-image read-only data section already mapped executable copy-on-write.
  if (protection == PAGE_EXECUTE_WRITECOPY || (owned_copy_on_write && protection == PAGE_EXECUTE_READWRITE)) {
    if (!verified_image_data(slot, region, proof))
      return false;
    writable_protection = region.Protect;  // Preserve executable permission.
  } else if (protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY) {
    writable_protection = PAGE_READWRITE;
  } else {
    return false;
  }
  const auto first = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
  const auto address = reinterpret_cast<std::uintptr_t>(slot);
  return region.RegionSize <= std::numeric_limits<std::uintptr_t>::max() - first && address >= first &&
         address <= first + region.RegionSize && sizeof(void*) <= first + region.RegionSize - address;
}

bool code_pointer(const void* pointer) noexcept {
  MEMORY_BASIC_INFORMATION region{};
  if (pointer == nullptr || VirtualQuery(pointer, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
      (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
    return false;
  const auto protection = region.Protect & 0xff;
  return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
         protection == PAGE_EXECUTE_WRITECOPY;
}

void* slot_value(void** slot) noexcept {
  return std::atomic_ref<void*>(*slot).load(std::memory_order_acquire);
}

BOOL change_protection(void* address, SIZE_T size, DWORD protection, PDWORD prior) noexcept {
#ifdef TAXI_ENGINE_HOOK_VALIDATION
  return taxi_hook_test_virtual_protect(address, size, protection, prior);
#else
  return VirtualProtect(address, size, protection, prior);
#endif
}

DWORD preserve_cfg_targets(DWORD protection) noexcept {
  const auto access = protection & 0xff;
  return access == PAGE_EXECUTE_WRITECOPY || access == PAGE_EXECUTE_READWRITE ? protection | PAGE_TARGETS_NO_UPDATE : protection;
}

bool restore_pending_protection(DWORD& error) noexcept {
  if (pending_protection_slot == nullptr)
    return true;
  DWORD discarded = 0;
  if (!change_protection(pending_protection_slot, sizeof(void*), preserve_cfg_targets(pending_protection), &discarded)) {
    error = GetLastError();
    return false;
  }
  pending_protection_slot = nullptr;
  pending_protection = 0;
  return true;
}

Result exchange_slot(void** slot, void* expected, void* replacement, DWORD writable_protection, Status applied, Status mismatch) noexcept {
  DWORD prior = 0;
  if (!change_protection(slot, sizeof(void*), preserve_cfg_targets(writable_protection), &prior))
    return {Status::protection_change_failed, GetLastError()};
  // Retain the original protection even if restoration fails after a CAS
  // mismatch. No later exchange may replace this recovery obligation.
  pending_protection_slot = slot;
  pending_protection = prior;
  const auto observed = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), replacement, expected);
  const bool changed = observed == expected;
  if (changed && applied == Status::installed) {
    consumed = true;  // Our thunk may already be running, even if restoration fails.
    installed_copy_on_write = (writable_protection & 0xff) == PAGE_EXECUTE_WRITECOPY;
  }
  if (changed && applied == Status::removed)
    removed = true;
  DWORD error = ERROR_SUCCESS;
  if (!restore_pending_protection(error))
    return {Status::protection_restore_failed, error, changed, false};
  return {changed ? applied : mismatch, ERROR_SUCCESS, changed};
}
}  // namespace

extern "C" void taxi_engine_hook_observe(void* original_rcx) noexcept {
  const auto observer = saved_observer.load(std::memory_order_acquire);
  if (observer == nullptr || observing)
    return;
  observing = true;
  observer(original_rcx);
  observing = false;
}

namespace taxi_camera::engine_hook {

Result install(void** slot, void* expected_original, Observer observer, const ImageDataSlotProof* image_data) noexcept {
  const Lock lock;
  DWORD error = ERROR_SUCCESS;
  if (!restore_pending_protection(error))
    return {Status::protection_restore_failed, error, false, false};
  if (consumed)
    return {Status::installation_consumed};
  if (slot == nullptr || reinterpret_cast<std::uintptr_t>(slot) % alignof(void*) != 0 || expected_original == nullptr ||
      expected_original == reinterpret_cast<void*>(&taxi_engine_hook_thunk))
    return {Status::invalid_argument};
  DWORD writable_protection = 0;
  if (!slot_memory(slot, image_data, writable_protection))
    return {Status::invalid_slot_memory};
  if (!code_pointer(expected_original) || (observer != nullptr && !code_pointer(reinterpret_cast<const void*>(observer))))
    return {Status::invalid_code_pointer};
  if (slot_value(slot) != expected_original)
    return {Status::original_mismatch};

  // Published before the interlocked slot exchange, and immutable from the
  // first successful exchange until the caller establishes unload quiescence.
  taxi_engine_hook_original = reinterpret_cast<std::uintptr_t>(expected_original);
  saved_observer.store(observer, std::memory_order_release);
  saved_slot = slot;
  saved_image_data = image_data != nullptr ? *image_data : ImageDataSlotProof{};
  has_image_data = image_data != nullptr;
  return exchange_slot(slot, expected_original, reinterpret_cast<void*>(&taxi_engine_hook_thunk), writable_protection, Status::installed,
                       Status::original_mismatch);
}

Result remove() noexcept {
  const Lock lock;
  DWORD error = ERROR_SUCCESS;
  if (!restore_pending_protection(error))
    return {Status::protection_restore_failed, error, false, false};
  if (!consumed || removed)
    return {Status::not_installed};
  DWORD writable_protection = 0;
  if (!slot_memory(saved_slot, has_image_data ? &saved_image_data : nullptr, writable_protection, installed_copy_on_write))
    return {Status::invalid_slot_memory};
  if (slot_value(saved_slot) != reinterpret_cast<void*>(&taxi_engine_hook_thunk))
    return {Status::slot_changed};
  return exchange_slot(saved_slot, reinterpret_cast<void*>(&taxi_engine_hook_thunk), reinterpret_cast<void*>(taxi_engine_hook_original),
                       writable_protection, Status::removed, Status::slot_changed);
}

Result restore_protection() noexcept {
  const Lock lock;
  DWORD error = ERROR_SUCCESS;
  if (!restore_pending_protection(error))
    return {Status::protection_restore_failed, error, false, false};
  return {Status::protection_restored};
}

const char* status_name(Status status) noexcept {
  switch (status) {
    case Status::installed:
      return "installed";
    case Status::removed:
      return "removed";
    case Status::invalid_argument:
      return "invalid_argument";
    case Status::invalid_slot_memory:
      return "invalid_slot_memory";
    case Status::invalid_code_pointer:
      return "invalid_code_pointer";
    case Status::original_mismatch:
      return "original_mismatch";
    case Status::installation_consumed:
      return "installation_consumed";
    case Status::not_installed:
      return "not_installed";
    case Status::slot_changed:
      return "slot_changed";
    case Status::protection_change_failed:
      return "protection_change_failed";
    case Status::protection_restore_failed:
      return "protection_restore_failed";
    case Status::protection_restored:
      return "protection_restored";
  }
  return "unknown";
}
}  // namespace taxi_camera::engine_hook
