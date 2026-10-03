#pragma once
#include <cstdint>

namespace taxi_camera::native_camera {

// Camera far distance for Taxi Cam's own views. The view refresh
// (update_view, 1.8.16.0 RVA 66825216) passes camera+0x5F0 as the near plane,
// camera+0x5F4 as the far distance of the view's culling frustum (a negative
// value falls back to +0x5F8) and camera+0x5F8 as the default far, which the
// rendering depth range, render-context scaling and the shared object-LOD
// viewer list read. The engine's own SetFar (RVA 66859263) writes both far
// values. Taxi Cam's views are created with a 1000 m far, so after take-off
// they stop drawing the ground and the cloud raymarch ends each sky ray below
// the cloud base; the main view's camera carries the simulator's own
// distance. See docs/architecture.md (camera draw distance, camera weather).
inline constexpr std::uint64_t kCameraNearOffset = 0x5F0, kCameraFarOffset = 0x5F4, kCameraDefaultFarOffset = 0x5F8;

struct CameraClip {
  float near_plane = 0, far_plane = 0, default_far = 0;
  bool operator==(const CameraClip&) const = default;
};

// Finite, positive near, default far beyond near, and a culling far that is
// either negative (falls back to the default) or beyond near.
bool plausible_camera_clip(const CameraClip& clip) noexcept;

// One exact read of +0x5F0..+0x5FB. Any current-process camera address; the
// caller must hold it inside a validated engine update. False on misalignment,
// overflow or a failed read; the values are not judged.
bool read_camera_clip(std::uint64_t camera_address, CameraClip& clip) noexcept;

// The far pair (+0x5F4, +0x5F8) a Taxi Cam camera should carry. With a
// plausible main-view clip: the main view's two far values, so the camera
// draws as far as the main view. Otherwise (main view not yet known) the
// camera's own values as first observed. The near plane is never part of the
// target. False when the needed input is implausible; the caller then writes
// nothing.
bool camera_far_target(const CameraClip& own, const CameraClip& main, CameraClip& target) noexcept;

struct CameraClipResult {
  bool complete = false;
  bool write_attempted = false;
  CameraClip before, after;
  const char* error = "not_attempted";
};

// Only the camera of a freshly verified owned view, inside the validated
// engine observer and before that view's update_view. One exact read of the
// three values; when both far values already equal the target nothing else
// happens. Otherwise +0x5F0..+0x5FB must lie in one private PAGE_READWRITE
// allocation, only the 8 bytes +0x5F4..+0x5FB are written (the near plane is
// never touched), and all three values are reread. No engine call,
// allocation or protection change; a failure leaves the camera as it was.
CameraClipResult apply_camera_far(std::uint64_t camera_address, const CameraClip& target) noexcept;

}  // namespace taxi_camera::native_camera
