#pragma once
#include <cmath>
#include <cstdint>
#include "aircraft_mounts.hpp"

namespace taxi_camera::native_camera {

// The aircraft model's scene transform read during the camera-manager update
// lags the frame the camera views render: live 2026-10-01 in flight
// (iniBuilds A350, about 240 kt) the cameras sat metres behind their mounts,
// the tail view showed the fin it is mounted on, and the viewpoints wandered,
// also back and forth while taxiing. The user saw the same to a degree on
// every aircraft, so every profile leads by one frame unless its
// AircraftProfile::pose_lead_frames says otherwise.
//
// A per-update transform trace (0.9.62, A350 taxi and take-off roll to
// 115 m/s) showed the simulation advancing the model in its own steps, one or
// sometimes two per drawn frame, uncorrelated with frame time (r = -0.06), so
// speed x frame duration missed by up to ~3 m. Against "the next read is what
// is drawn", the best of the rules tried is the smoothed average movement per
// update: RMS 0.93 m (speed x frame 1.18 m), median 0.24 m (0.49 m). The
// remaining error is the unpredictable double step. The step is the
// displacement between reads divided by the camera-manager updates between
// them, smoothed by kStepSmoothing per update. Orientation is not led.
// Observer thread only.
class PoseLead {
 public:
  // Reads further apart than this are not used.
  static constexpr std::uint64_t kMaximumUpdates = 30;
  static constexpr double kMaximumReadGapSeconds = 0.5;
  // Faster than this (m/s), or a per-update step over kMaximumStepMetres, is a
  // slew, teleport or new model: no lead, and the estimate starts again.
  static constexpr double kMaximumSpeed = 400;
  static constexpr double kMaximumStepMetres = 40;
  static constexpr double kMaximumLeadFrames = 4;
  // Per-update weight of a new step in the average (the trace's best value).
  static constexpr double kStepSmoothing = 0.1;
  // Per-read weight of a new velocity for the reported speed.
  static constexpr double kSpeedSmoothing = 0.35;

  // update: camera-manager update count of this read; now_s: its clock in
  // seconds; epoch/resets: aircraft session and reset generation; frames: the
  // aircraft's lead (0 none). Returns the pose to mount the cameras on. The
  // step and speed are measured either way so the log shows every aircraft.
  BodyPose lead(const BodyPose& scene,
                std::uint64_t update,
                double now_s,
                std::uint64_t epoch,
                std::uint64_t resets,
                double frames) noexcept {
    const bool same_session = valid_ && epoch && epoch == epoch_ && resets == resets_;
    if (!same_session)
      estimate_valid_ = false;
    if (same_session && update > update_) {
      const auto updates = update - update_;
      const double gap = std::isfinite(now_s) ? now_s - read_s_ : -1;
      Vector3 step{}, velocity{};
      for (unsigned i = 0; i < 3; ++i) {
        const double moved = scene.origin[i] - origin_[i];
        step[i] = moved / static_cast<double>(updates);
        velocity[i] = gap > 0 ? moved / gap : 0;
      }
      const double step_length = length(step), speed = length(velocity);
      if (updates <= kMaximumUpdates && gap > 0 && gap <= kMaximumReadGapSeconds && std::isfinite(step_length) &&
          step_length <= kMaximumStepMetres && std::isfinite(speed) && speed <= kMaximumSpeed) {
        // The weight of `updates` per-update steps of kStepSmoothing each.
        const double weight = estimate_valid_ ? 1 - std::pow(1 - kStepSmoothing, static_cast<double>(updates)) : 1;
        const double speed_weight = estimate_valid_ ? kSpeedSmoothing : 1;
        for (unsigned i = 0; i < 3; ++i) {
          step_[i] += weight * (step[i] - step_[i]);
          velocity_[i] += speed_weight * (velocity[i] - velocity_[i]);
        }
        estimate_valid_ = true;
      } else {
        estimate_valid_ = false;
      }
    }
    if (!same_session || update > update_) {
      origin_ = scene.origin;
      update_ = update;
      read_s_ = std::isfinite(now_s) ? now_s : 0;
      epoch_ = epoch;
      resets_ = resets;
      valid_ = true;
    }
    if (!estimate_valid_) {
      step_ = {};
      velocity_ = {};
    }
    const double applied = std::isfinite(frames) && frames > 0 && frames <= kMaximumLeadFrames ? frames : 0;
    speed_ = length(velocity_);
    step_metres_ = length(step_);
    lead_metres_ = step_metres_ * applied;
    BodyPose led = scene;
    for (unsigned i = 0; i < 3; ++i)
      led.origin[i] += step_[i] * applied;
    return led;
  }
  void reset() noexcept { *this = {}; }
  // Smoothed speed (m/s), smoothed movement per update and the lead applied
  // to the cameras (metres) at the last read.
  double speed() const noexcept { return speed_; }
  double step_metres() const noexcept { return step_metres_; }
  double lead_metres() const noexcept { return lead_metres_; }

 private:
  static double length(const Vector3& v) noexcept { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
  Vector3 origin_{}, step_{}, velocity_{};
  double read_s_ = 0, speed_ = 0, step_metres_ = 0, lead_metres_ = 0;
  std::uint64_t update_ = 0, epoch_ = 0, resets_ = 0;
  bool valid_ = false, estimate_valid_ = false;
};

}  // namespace taxi_camera::native_camera
