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

// Capture spacing rate (SceneCaptureManager::set_source_rate). A camera may
// render on consecutive simulator frames, so the spacing only refuses a second
// capture within one frame up to this rate; the schedule sets the cadence.
inline constexpr unsigned kMaximumCaptureSourceRate = 120;

}  // namespace taxi_camera
