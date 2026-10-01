#pragma once
#include <cmath>
#include <cstdint>
#include "aircraft_mounts.hpp"

namespace taxi_camera::native_camera {

// On some aircraft the model's scene transform read during the camera-manager
// update lags the frame the camera views render: live 2026-10-01 in flight
// (iniBuilds A350, about 240 kt) the cameras sat metres behind their mounts,
// the tail view showed the fin it is mounted on, and the viewpoints wandered
// with frame time, also back and forth while taxiing. The PMDG 777 did not
// show it, so the lead is per aircraft (AircraftProfile::pose_lead_frames).
// The pose is led by the model's own movement per simulator frame: the
// displacement between two reads divided by the camera-manager updates
// between them, so no clock jitter enters. Orientation is not led.
// Observer thread only.
class PoseLead {
 public:
  // Reads further apart than this many updates do not estimate a frame step.
  static constexpr std::uint64_t kMaximumUpdates = 30;
  // Above this per-frame step (about 400 m/s at 10 fps) the read is a slew,
  // teleport or new model, not flight: no lead.
  static constexpr double kMaximumStepMetres = 40;
  static constexpr double kMaximumLeadFrames = 4;

  // update: camera-manager update count of this read; epoch/resets: aircraft
  // session and reset generation; frames: the aircraft's lead (0 none).
  // Returns the pose to mount the cameras on. The step is measured either way
  // so the log shows every aircraft's per-frame movement. A second read in the
  // same update reuses the step measured for that update.
  BodyPose lead(const BodyPose& scene, std::uint64_t update, std::uint64_t epoch, std::uint64_t resets, double frames) noexcept {
    if (!valid_ || update != update_) {
      step_ = {};
      step_metres_ = 0;
      if (valid_ && epoch && epoch == epoch_ && resets == resets_ && update > update_ && update - update_ <= kMaximumUpdates) {
        const double updates = static_cast<double>(update - update_);
        Vector3 step{};
        for (unsigned i = 0; i < 3; ++i)
          step[i] = (scene.origin[i] - origin_[i]) / updates;
        const double length = std::sqrt(step[0] * step[0] + step[1] * step[1] + step[2] * step[2]);
        if (std::isfinite(length) && length <= kMaximumStepMetres) {
          step_ = step;
          step_metres_ = length;
        }
      }
      origin_ = scene.origin;
      update_ = update;
      epoch_ = epoch;
      resets_ = resets;
      valid_ = true;
    }
    const double applied = std::isfinite(frames) && frames > 0 && frames <= kMaximumLeadFrames ? frames : 0;
    lead_metres_ = step_metres_ * applied;
    BodyPose led = scene;
    for (unsigned i = 0; i < 3; ++i)
      led.origin[i] += step_[i] * applied;
    return led;
  }
  void reset() noexcept { *this = {}; }
  // Metres the model moved per frame at the last read (zero when unknown), and
  // the lead applied to the cameras.
  double step_metres() const noexcept { return step_metres_; }
  double lead_metres() const noexcept { return lead_metres_; }

 private:
  Vector3 origin_{}, step_{};
  std::uint64_t update_ = 0, epoch_ = 0, resets_ = 0;
  double step_metres_ = 0, lead_metres_ = 0;
  bool valid_ = false;
};

}  // namespace taxi_camera::native_camera
