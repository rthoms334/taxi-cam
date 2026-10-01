#pragma once

#include <windows.h>

#include <cstdint>

namespace taxi_camera::engine_hook {

using Observer = void (*)(void* original_rcx) noexcept;

enum class Status {
  installed,
  removed,
  invalid_argument,
  invalid_slot_memory,
  invalid_code_pointer,
  original_mismatch,
  installation_consumed,
  not_installed,
  slot_changed,
  protection_change_failed,
  protection_restore_failed,
  protection_restored,
};

struct Result {
  Status status;
  DWORD windows_error = ERROR_SUCCESS;
  // True only when this operation's compare/exchange changed the slot.
  bool pointer_changed = false;
  // True when protection was unchanged or VirtualProtect accepted restoration
  // of the prior protection. Windows may report PAGE_EXECUTE_READWRITE for a
  // private copy after a PAGE_EXECUTE_WRITECOPY write. A failed restoration is
  // reported even if the pointer exchange succeeded.
  bool protection_restored = true;
};

// Optional admission for an image data page mapped PAGE_EXECUTE_WRITECOPY.
// The implementation verifies these values against bounded PE headers in the
// current main image. This never admits executable/writable PE sections, other
// modules, or anonymous executable memory. The caller still owns image lifetime.
// Only removal of our successful verified copy-on-write install can subsequently
// admit that same slot's PAGE_EXECUTE_READWRITE private-page promotion.
struct ImageDataSlotProof {
  HMODULE main_image = nullptr;
  std::uint32_t image_size = 0;
  std::uint32_t section_rva = 0;
  std::uint32_t section_size = 0;
};

// One successful installation per linked module instance, including an install
// whose final protection restoration fails. The caller supplies the exact live
// slot and original, and owns all module/slot/observer lifetime guarantees.
// A null observer is a no-op. Saved observer/original never change after a
// successful exchange; enable requests inside the observer's own state.
Result install(void** slot, void* expected_original, Observer observer = nullptr, const ImageDataSlotProof* image_data = nullptr) noexcept;

// Exchanges only our thunk for the original. Does not wait for in-flight calls,
// erase callback state, or permit another installation. A foreign replacement
// is left untouched; removal can be retried if that owner restores our thunk.
Result remove() noexcept;

// Retries a failed page-protection restoration without exchanging the pointer.
// Install/remove also retry pending restoration first and refuse to exchange
// any pointer while it remains unresolved. Keep the slot allocation alive.
Result restore_protection() noexcept;

const char* status_name(Status status) noexcept;

}  // namespace taxi_camera::engine_hook
