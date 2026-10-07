#pragma once

namespace taxi_camera {

inline constexpr unsigned kMinimumCameraRate = 5;
inline constexpr unsigned kMaximumCameraRate = 60;
inline constexpr unsigned kDefaultCameraRate = 10;
// Camera mode (settings camera_mode). The presets and custom are targets in
// images per camera per second (custom: the saved camera_rate, the
// companion's slider). automatic instead renders a whole number of cameras on
// every frame (1, 2, then all of them), the only way to keep every frame
// carrying the same camera work, and steps between those to keep the
// simulator's frame rate close to its rate without camera work
// (AutoCameraPolicy).
enum class CameraMode : unsigned { performance = 0, balanced = 1, smooth = 2, custom = 3, automatic = 4 };
inline constexpr unsigned kCameraModeCount = 5;
inline constexpr CameraMode kDefaultCameraMode = CameraMode::automatic;
inline constexpr unsigned kPerformanceCameraRate = 10, kBalancedCameraRate = 15, kSmoothCameraRate = 30;
inline constexpr unsigned kAutoCameraLevels = 3;
// Cameras Auto renders on every frame at a level, at most the feeds there are.
constexpr unsigned auto_cameras_per_frame(unsigned level, unsigned feeds) noexcept {
  const unsigned wanted = (level < kAutoCameraLevels ? level : kAutoCameraLevels - 1) + 1;
  const unsigned available = feeds ? feeds : 1u;
  return wanted < available ? wanted : available;
}

// Capture spacing rate (SceneCaptureManager::set_source_rate): its spacing
// refuses a second capture of one camera within one simulator frame. A camera
// may render on consecutive frames, so the rate follows the measured update
// rate: spacing about two thirds of a frame, never more than 1/120 s (the rate
// while the update rate is unknown) and never under 1/kMaximumCaptureSourceRate.
inline constexpr unsigned kMinimumCaptureSourceRate = 120;
inline constexpr unsigned kMaximumCaptureSourceRate = 500;
inline unsigned capture_source_rate(double update_hz) noexcept {
  if (!(update_hz > 0))
    return kMinimumCaptureSourceRate;
  const double wanted = update_hz * 1.5;
  if (wanted <= kMinimumCaptureSourceRate)
    return kMinimumCaptureSourceRate;
  if (wanted >= kMaximumCaptureSourceRate)
    return kMaximumCaptureSourceRate;
  // Steps of 10 so a jittering update rate does not reset the spacing often.
  return (static_cast<unsigned>(wanted) + 9) / 10 * 10;
}

}  // namespace taxi_camera
