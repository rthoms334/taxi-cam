#include "view_aa.hpp"
#include "local_memory.hpp"

#include <windows.h>

namespace taxi_camera::native_camera {
namespace {
bool writable_flags(std::uint64_t address) noexcept {
  return writable_private_span(address, 16);
}
bool read_flags(std::uint64_t address, std::array<std::uint64_t, 2>& flags) noexcept {
  return read_local_flag_words(address, flags);
}
bool read_overrides(discovery::ImageReader& image, std::array<std::uint64_t, 2>& value, const CameraImageLayout& layout) noexcept {
  return image.read(layout.view_flag_clear_override, &value[0], 8) && image.read(layout.view_flag_set_override, &value[1], 8);
}
}  // namespace

ViewAaResult set_owned_view_aa(const engine_camera::OwnedViewSnapshot& view,
                               discovery::ImageReader& image,
                               const CameraImageLayout& layout,
                               ViewAaRequest request) noexcept {
  ViewAaResult result;
  const auto fail = [&](const char* error) {
    result.error = error;
    return result;
  };
  const bool enabled = request.aa;
  if (!camera_layout_detail::image_rva(layout.view_flag_clear_override, 8, 8) ||
      !camera_layout_detail::image_rva(layout.view_flag_set_override, 8, 8) ||
      layout.view_flag_clear_override == layout.view_flag_set_override)
    return fail("aa_invalid_image_layout");
  if (!view.complete || !view.ready || view.mode != 2 || view.status != engine_camera::OwnedViewStatus::ready || view.read_failures ||
      (view.error && *view.error) || !view.view_address || (view.view_address & 7) || view.view_address > UINTPTR_MAX - 64)
    return fail("aa_invalid_owned_view");
  // bit36 is cleared only together with the AA bit (development values 4 and
  // 5), and only on the image its render-driver test was decoded on.
  if (request.bit36 == ViewBit36::clear && !enabled)
    return fail("bit36_without_aa");
  if (request.bit36 == ViewBit36::clear && layout != observed_store_layout())
    return fail("bit36_unverified_image");
  // An open gate only refuses a write. A view kept rendering every update
  // (continuous development schedule) already carrying the requested bits is
  // confirmed by the same rereads below, without writing.
  if (!(view.flags[0] & 1u) && requested_view_flags(view.flags[0], request) != view.flags[0])
    return fail("aa_gate_open");
  const auto field = view.view_address + 48;
  if (!writable_flags(field))
    return fail("aa_flags_not_writable");
  std::array<std::uint64_t, 2> current{}, overrides{}, again{};
  if (!read_flags(field, current) || !read_overrides(image, overrides, layout))
    return fail("aa_read_failed");
  if (current != view.flags)
    return fail("aa_snapshot_changed");
  if (enabled && (overrides[0] & kViewAaFlag) != 0)
    return fail("aa_cleared_by_global_override");
  if (!enabled && (overrides[1] & ~overrides[0] & kViewAaFlag) != 0)
    return fail("aa_forced_by_global_override");
  if (request.bit36 == ViewBit36::clear && (overrides[1] & ~overrides[0] & kViewDirectOutputFlag) != 0)
    return fail("bit36_forced_by_global_override");
  // Without bit36 the view takes the global frame-generation mode. The caller
  // withholds the clear while it is on; this refuses a change that raced it.
  if (request.bit36 == ViewBit36::clear && read_global_frame_generation(image, layout) != 0)
    return fail("bit36_frame_generation_on");
  auto desired = current;
  desired[0] = requested_view_flags(current[0], request);
  if (desired != current) {
    SIZE_T written = 0;
    result.write_attempted = true;
    result.changed = desired[0] ^ current[0];
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(field), &desired[0], 8, &written) || written != 8)
      return fail("aa_write_failed");
  }
  if (!read_flags(field, current) || !read_overrides(image, again, layout))
    return fail("aa_recheck_failed");
  if (current != desired || again != overrides)
    return fail("aa_changed_during_update");
  result.complete = true;
  result.error = "";
  return result;
}

ViewAaResult set_owned_view_aa(const engine_camera::OwnedViewSnapshot& view,
                               discovery::ImageReader& image,
                               const CameraImageLayout& layout,
                               bool enabled) noexcept {
  return set_owned_view_aa(view, image, layout, ViewAaRequest{enabled, ViewBit36::keep});
}

ViewAaResult disable_owned_view_aa(const engine_camera::OwnedViewSnapshot& view,
                                   discovery::ImageReader& image,
                                   const CameraImageLayout& layout) noexcept {
  return set_owned_view_aa(view, image, layout, false);
}

std::int32_t read_global_aa_mode(discovery::ImageReader& image, const CameraImageLayout& layout) noexcept {
  std::int32_t mode = -1;
  if (layout != observed_store_layout() || !image.read(kObservedAaModeRva, &mode, sizeof(mode)))
    return -1;
  return mode;
}

std::int32_t read_global_frame_generation(discovery::ImageReader& image, const CameraImageLayout& layout) noexcept {
  std::uint8_t mode = 0;
  if (layout != observed_store_layout() || !image.read(kObservedFrameGenerationRva, &mode, sizeof(mode)))
    return -1;
  return mode;
}

ViewAaResult restore_view_aa_flag(std::uint64_t view_address) noexcept {
  return restore_view_flags(view_address, kViewAaFlag);
}

ViewAaResult restore_view_flags(std::uint64_t view_address, std::uint64_t bits) noexcept {
  ViewAaResult result;
  const auto fail = [&](const char* error) {
    result.error = error;
    return result;
  };
  if (!bits || (bits & ~(kViewAaFlag | kViewDirectOutputFlag)))
    return fail("aa_invalid_restore_bits");
  if (!view_address || (view_address & 7) || view_address > UINTPTR_MAX - 64)
    return fail("aa_invalid_view_address");
  const auto field = view_address + 48;
  if (!writable_flags(field))
    return fail("aa_flags_not_writable");
  std::array<std::uint64_t, 2> current{};
  if (!read_flags(field, current))
    return fail("aa_read_failed");
  if (!(current[0] & 1u))
    return fail("aa_gate_open");
  auto desired = current;
  desired[0] |= bits;
  if (desired != current) {
    SIZE_T written = 0;
    result.write_attempted = true;
    result.changed = desired[0] ^ current[0];
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(field), &desired[0], 8, &written) || written != 8)
      return fail("aa_write_failed");
  }
  if (!read_flags(field, current))
    return fail("aa_recheck_failed");
  if (current != desired)
    return fail("aa_changed_during_update");
  result.complete = true;
  result.error = "";
  return result;
}

}  // namespace taxi_camera::native_camera
