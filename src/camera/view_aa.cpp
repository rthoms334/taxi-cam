#include "view_aa.hpp"

#include <windows.h>
#include <algorithm>
#include <limits>

namespace taxi_camera::native_camera {
namespace {
bool writable_flags(std::uint64_t address) noexcept {
  if (!address || address > std::numeric_limits<std::uintptr_t>::max() - 16)
    return false;
  const auto end = address + 16;
  void* allocation = nullptr;
  while (address < end) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
        region.Type != MEM_PRIVATE || region.Protect != PAGE_READWRITE || !region.AllocationBase ||
        (allocation && allocation != region.AllocationBase))
      return false;
    allocation = region.AllocationBase;
    const auto begin = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (begin > address || region.RegionSize > UINTPTR_MAX - begin || begin + region.RegionSize <= address)
      return false;
    address = std::min<std::uint64_t>(end, begin + region.RegionSize);
  }
  return true;
}
bool read_flags(std::uint64_t address, std::array<std::uint64_t, 2>& flags) noexcept {
  SIZE_T bytes = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), flags.data(), sizeof(flags), &bytes) &&
         bytes == sizeof(flags);
}
bool read_overrides(discovery::ImageReader& image, std::array<std::uint64_t, 2>& value) noexcept {
  return image.read(kViewFlagClearOverride, &value[0], 8) && image.read(kViewFlagSetOverride, &value[1], 8);
}
}  // namespace

ViewAaResult disable_owned_view_aa(const engine_camera::OwnedViewSnapshot& view, discovery::ImageReader& image) noexcept {
  ViewAaResult result;
  const auto fail = [&](const char* error) {
    result.error = error;
    return result;
  };
  if (!view.complete || !view.ready || view.mode != 2 || view.status != engine_camera::OwnedViewStatus::ready || view.read_failures ||
      (view.error && *view.error) || !view.view_address || (view.view_address & 7) || view.view_address > UINTPTR_MAX - 64)
    return fail("aa_invalid_owned_view");
  if (!(view.flags[0] & 1u))
    return fail("aa_gate_open");
  const auto field = view.view_address + 48;
  if (!writable_flags(field))
    return fail("aa_flags_not_writable");
  std::array<std::uint64_t, 2> current{}, overrides{}, again{};
  if (!read_flags(field, current) || !read_overrides(image, overrides))
    return fail("aa_read_failed");
  if (current != view.flags)
    return fail("aa_snapshot_changed");
  if ((overrides[1] & ~overrides[0] & kViewAaFlag) != 0)
    return fail("aa_forced_by_global_override");
  auto desired = current;
  desired[0] &= ~kViewAaFlag;
  if (desired != current) {
    SIZE_T written = 0;
    result.write_attempted = true;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(field), &desired[0], 8, &written) || written != 8)
      return fail("aa_write_failed");
  }
  if (!read_flags(field, current) || !read_overrides(image, again))
    return fail("aa_recheck_failed");
  if (current != desired || again != overrides)
    return fail("aa_changed_during_update");
  result.complete = true;
  result.error = "";
  return result;
}

}  // namespace taxi_camera::native_camera
