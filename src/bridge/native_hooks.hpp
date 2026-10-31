#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace taxi_camera::standalone {
inline thread_local unsigned owned_depth = 0;
struct OwnedWork {
  OwnedWork() noexcept { ++owned_depth; }
  ~OwnedWork() { --owned_depth; }
};
inline bool image_region(const void* p, bool executable) noexcept {
  MEMORY_BASIC_INFORMATION m{};
  if (!p || !VirtualQuery(p, &m, sizeof(m)) || m.State != MEM_COMMIT || m.Type != MEM_IMAGE || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
    return false;
  const DWORD access = m.Protect & 255;
  return executable ? access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY
                    : access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY;
}
inline bool pin_image(const void* p, bool executable) noexcept {
  HMODULE module{};
  return image_region(p, executable) &&
         GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(p), &module);
}
// One verified native COM vtable per method. No executable memory allocation,
// cloned tables or guessed expanded interface slots. Forward pointers are
// published before CAS and remain process-lifetime, even after partial failure.
struct NativeSlot {
  std::atomic<void*> original{};
  void** address{};
  void** pending_protection{};
  DWORD pending_access{};
  bool repair_protection() noexcept {
    if (!pending_protection)
      return true;
    DWORD discarded{};
    if (!VirtualProtect(pending_protection, sizeof(void*), pending_access, &discarded))
      return false;
    pending_protection = nullptr;
    return true;
  }
  bool install(void* object, unsigned index, void* wrapper) noexcept {
    if (!object || !repair_protection())
      return false;
    auto** table = *static_cast<void***>(object);
    auto** slot = table + index;
    if (address)
      return slot == address && *slot == wrapper;
    void* prior = *slot;
    // A slot that already holds this wrapper would make the wrapper forward to
    // itself: unbounded recursion on its first call. Refuse before publishing
    // any forward pointer.
    if (prior == wrapper)
      return false;
    if (!pin_image(slot, false) || !pin_image(prior, true) || !pin_image(wrapper, true))
      return false;
    original.store(prior, std::memory_order_release);
    DWORD old{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
      return false;
    void* observed = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), wrapper, prior);
    DWORD discarded{};
    const bool restored = VirtualProtect(slot, sizeof(void*), old, &discarded) != FALSE;
    if (!restored) {
      pending_protection = slot;
      pending_access = old;
    }
    const bool repaired = restored || repair_protection();
    // Retain address/original even on restore failure; no code may be unloaded.
    if (observed == prior)
      address = slot;
    return observed == prior && repaired;
  }
  template <class F>
  F forward() const noexcept {
    return reinterpret_cast<F>(original.load(std::memory_order_acquire));
  }
};
}  // namespace taxi_camera::standalone
