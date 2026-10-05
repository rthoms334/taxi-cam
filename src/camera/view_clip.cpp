#include "view_clip.hpp"
#include "local_memory.hpp"

#include <windows.h>

#include <array>
#include <cmath>
#include <cstring>

namespace taxi_camera::native_camera {

bool plausible_camera_clip(const CameraClip& clip) noexcept {
  return std::isfinite(clip.near_plane) && std::isfinite(clip.far_plane) && std::isfinite(clip.default_far) && clip.near_plane > 0 &&
         clip.default_far > clip.near_plane && (clip.far_plane < 0 || clip.far_plane > clip.near_plane);
}

bool read_camera_clip(std::uint64_t camera_address, CameraClip& clip) noexcept {
  if (!camera_address || (camera_address & 7) || camera_address > UINTPTR_MAX - kCameraNearOffset - 16)
    return false;
  // Sixteen bytes from +0x5F0: near, far, default far and the next camera word.
  std::array<std::uint64_t, 2> words{};
  if (!read_local_flag_words(camera_address + kCameraNearOffset, words))
    return false;
  std::array<float, 4> values{};
  std::memcpy(values.data(), words.data(), sizeof(values));
  clip = {values[0], values[1], values[2]};
  return true;
}

bool camera_far_target(const CameraClip& own, const CameraClip& main, CameraClip& target) noexcept {
  if (!plausible_camera_clip(own))
    return false;
  target = own;
  if (plausible_camera_clip(main)) {
    target.far_plane = main.far_plane;
    target.default_far = main.default_far;
    // The camera keeps its own near plane; the main far must still lie beyond it.
    if (!plausible_camera_clip(target))
      return false;
  }
  return true;
}

CameraClipResult apply_camera_far(std::uint64_t camera_address, const CameraClip& target) noexcept {
  CameraClipResult result;
  const auto fail = [&](const char* error) {
    result.error = error;
    return result;
  };
  // One exact read decides; a camera already at its target costs nothing more.
  if (!read_camera_clip(camera_address, result.before))
    return fail("clip_read_failed");
  if (!plausible_camera_clip(result.before))
    return fail("clip_implausible_camera");
  if (result.before.far_plane == target.far_plane && result.before.default_far == target.default_far) {
    result.after = result.before;
    result.complete = true;
    result.error = "";
    return result;
  }
  if (!plausible_camera_clip({result.before.near_plane, target.far_plane, target.default_far}))
    return fail("clip_implausible_target");
  const auto field = camera_address + kCameraNearOffset;
  if (!writable_private_span(field, 12))
    return fail("clip_not_writable");
  {
    // The store takes its own fresh proof of these 8 bytes immediately before it.
    const std::array<float, 2> values{target.far_plane, target.default_far};
    result.write_attempted = true;
    if (!write_local_private(camera_address + kCameraFarOffset, values.data(), sizeof(values)))
      return fail("clip_write_failed");
  }
  if (!read_camera_clip(camera_address, result.after))
    return fail("clip_recheck_failed");
  if (result.after.far_plane != target.far_plane || result.after.default_far != target.default_far ||
      result.after.near_plane != result.before.near_plane)
    return fail("clip_changed_during_update");
  result.complete = true;
  result.error = "";
  return result;
}

}  // namespace taxi_camera::native_camera
