#include "view_cascades.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstring>

#include "view_cascades_contract.hpp"

namespace taxi_camera::native_camera::view_cascades {
namespace {

using Setup = std::uint64_t (*)(void*, void*, void*, void*);
std::atomic<Setup> original{nullptr};
std::array<std::atomic<std::uint64_t>, kMaxCameraFeeds> owned_views{};
std::atomic<std::uint64_t> hits{0}, writes{0};
std::atomic<bool> installed{false};
std::atomic<const char*> install_error{""};
SRWLOCK install_lock = SRWLOCK_INIT;
bool attempted = false;

// The decision the hook makes for one setup call: lowers *slice_count to
// kCameraShadowSlices when the view is owned and its current count is higher.
// Returns whether the view was owned.
bool apply(std::uint64_t view, float* slice_count, const std::array<std::uint64_t, kMaxCameraFeeds>& owned, bool& lowered) noexcept {
  lowered = false;
  if (!view || !slice_count)
    return false;
  bool mine = false;
  for (const auto candidate : owned)
    mine = mine || (candidate && candidate == view);
  if (!mine)
    return false;
  const auto target = static_cast<float>(kCameraShadowSlices);
  if (*slice_count > target) {
    *slice_count = target;
    lowered = true;
  }
  return true;
}

// Main thread, inside View::PrepareScene. RCX/RDX are the scene and the render
// context; the setup takes no other arguments (it zeroes R8 before its first
// call), and R8/R9 are passed through unchanged. The context is the
// simulator's live argument; context+0xC10 is the view being prepared, so the
// only write lands in that live view's slice-count field.
std::uint64_t cascade_setup(void* scene, void* context, void* r8, void* r9) noexcept {
  if (context) {
    std::uint64_t view = 0;
    std::memcpy(&view, static_cast<const unsigned char*>(context) + view_cascades_contract::ContextViewOffset, sizeof(view));
    if (view) {
      std::array<std::uint64_t, kMaxCameraFeeds> owned{};
      for (unsigned i = 0; i < owned.size(); ++i)
        owned[i] = owned_views[i].load(std::memory_order_relaxed);
      bool lowered = false;
      if (apply(view, reinterpret_cast<float*>(view + view_cascades_contract::SliceCountOffset), owned, lowered)) {
        hits.fetch_add(1, std::memory_order_relaxed);
        if (lowered)
          writes.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  return original.load(std::memory_order_acquire)(scene, context, r8, r9);
}

// The slot's page and the protection to write it with. Read-only data pages
// are written PAGE_READWRITE. The Xbox loader maps this build's read-only data
// executable/write-copy (the camera-manager slot hook handles the same page
// kind): that is accepted only inside the caller's verified read-only,
// non-executable image section, and keeps its execute permission and CFG
// targets through Windows' copy-on-write promotion.
bool slot_page(void** slot, std::uintptr_t section_begin, std::uintptr_t section_end, DWORD& writable) noexcept {
  MEMORY_BASIC_INFORMATION region{};
  if (VirtualQuery(slot, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT || region.Type != MEM_IMAGE ||
      region.AllocationBase != GetModuleHandleW(nullptr) || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
    return false;
  const auto access = region.Protect & 0xff;
  if (access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY) {
    writable = PAGE_READWRITE;
    return true;
  }
  const auto address = reinterpret_cast<std::uintptr_t>(slot);
  // Execute/read-write: the same page after a copy-on-write promotion.
  if ((access == PAGE_EXECUTE_WRITECOPY || access == PAGE_EXECUTE_READWRITE) && address >= section_begin && address < section_end &&
      sizeof(void*) <= section_end - address) {
    writable = region.Protect | PAGE_TARGETS_NO_UPDATE;
    return true;
  }
  return false;
}

DWORD preserve_cfg_targets(DWORD protection) noexcept {
  const auto access = protection & 0xff;
  return access == PAGE_EXECUTE_WRITECOPY || access == PAGE_EXECUTE_READWRITE ? protection | PAGE_TARGETS_NO_UPDATE : protection;
}

bool executable(const void* code) noexcept {
  MEMORY_BASIC_INFORMATION region{};
  if (!code || VirtualQuery(code, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT || region.Type != MEM_IMAGE ||
      region.AllocationBase != GetModuleHandleW(nullptr))
    return false;
  const auto access = region.Protect & 0xff;
  return access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
}

}  // namespace

void publish_views(const std::array<std::uint64_t, kMaxCameraFeeds>& views) noexcept {
  for (unsigned i = 0; i < views.size(); ++i)
    owned_views[i].store(views[i], std::memory_order_relaxed);
}

bool install(std::uintptr_t image_base,
             std::uint32_t setup_rva,
             std::uint32_t slot_rva,
             std::uint32_t section_rva,
             std::uint32_t section_size) noexcept {
  AcquireSRWLockExclusive(&install_lock);
  struct Release {
    ~Release() { ReleaseSRWLockExclusive(&install_lock); }
  } release;
  if (attempted)
    return installed.load(std::memory_order_acquire);
  attempted = true;
  const auto fail = [](const char* error) {
    install_error.store(error, std::memory_order_release);
    return false;
  };
  if (!image_base || !setup_rva || !slot_rva || slot_rva % alignof(void*) || !section_size || slot_rva < section_rva ||
      slot_rva - section_rva > section_size - sizeof(void*))
    return fail("unavailable");
  auto** slot = reinterpret_cast<void**>(image_base + slot_rva);
  auto* expected = reinterpret_cast<void*>(image_base + setup_rva);
  DWORD writable = 0;
  if (!slot_page(slot, image_base + section_rva, image_base + section_rva + section_size, writable))
    return fail("slot_memory");
  if (!executable(expected))
    return fail("setup_not_code");
  if (std::atomic_ref<void*>(*slot).load(std::memory_order_acquire) != expected)
    return fail("slot_changed");
  original.store(reinterpret_cast<Setup>(expected), std::memory_order_release);
  DWORD prior = 0;
  if (!VirtualProtect(slot, sizeof(void*), writable, &prior))
    return fail("protection_change_failed");
  const auto observed =
      InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(&cascade_setup), expected);
  DWORD discarded = 0;
  const bool restored = VirtualProtect(slot, sizeof(void*), preserve_cfg_targets(prior), &discarded) != FALSE;
  if (observed != expected)
    return fail("slot_changed");
  installed.store(true, std::memory_order_release);
  if (!restored)
    install_error.store("protection_restore_failed", std::memory_order_release);
  return true;
}

Statistics statistics() noexcept {
  Statistics out;
  out.installed = installed.load(std::memory_order_acquire);
  out.requested = kCameraShadowSlices;
  out.hits = hits.load(std::memory_order_relaxed);
  out.writes = writes.load(std::memory_order_relaxed);
  out.error = install_error.load(std::memory_order_acquire);
  return out;
}

}  // namespace taxi_camera::native_camera::view_cascades
