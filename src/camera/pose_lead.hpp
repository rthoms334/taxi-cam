#pragma once
#include <cmath>
#include <cstdint>
#include "aircraft_mounts.hpp"

namespace taxi_camera::native_camera {

// The aircraft model's scene transform read during the camera-manager update
// lags the frame the camera views render: live 2026-10-01 in flight
// (iniBuilds A350, about 240 kt) the cameras sat metres behind their mounts,
// the tail view showed the fin it is mounted on, and the viewpoints wandered
// with frame time, also back and forth while taxiing. The user saw the same to
// a degree on every aircraft, so every profile leads by one frame unless its
// AircraftProfile::pose_lead_frames says otherwise.
//
// One frame of lag is the aircraft's movement during the frame being drawn:
// velocity times that frame's duration, which swings with frame time (a fixed
// average step left the cameras jumping a couple of metres back and forth in
// a 0.9.60 flight). The velocity is measured from the transform reads against
// the update clock and smoothed, since an aircraft's speed changes slowly; the
// frame duration is the interval between the last two camera-manager updates.
// Orientation is not led. Observer thread only.
class PoseLead {
 public:
  // Reads further apart than this are not used to measure velocity.
  static constexpr double kMaximumReadGapSeconds = 0.5;
  // Faster than this (m/s) is a slew, teleport or new model: no lead.
  static constexpr double kMaximumSpeed = 400;
  // Longer frames are capped: a hitch is not flown through by the camera.
  static constexpr double kMaximumFrameSeconds = 0.1;
  static constexpr double kMaximumLeadFrames = 4;
  // Velocity smoothing per read (1 = unsmoothed).
  static constexpr double kSmoothing = 0.35;

  // now_s: this update's clock in seconds; frame_s: the duration of the frame
  // being drawn (interval between the last two updates); epoch/resets:
  // aircraft session and reset generation; frames: the aircraft's lead (0
  // none). Returns the pose to mount the cameras on. Velocity is measured
  // either way so the log shows every aircraft's movement.
  BodyPose lead(const BodyPose& scene, double now_s, double frame_s, std::uint64_t epoch, std::uint64_t resets, double frames) noexcept {
    const bool same_session = valid_ && epoch && epoch == epoch_ && resets == resets_;
    if (!same_session)
      velocity_valid_ = false;
    if (same_session && std::isfinite(now_s) && now_s > read_s_) {
      const double gap = now_s - read_s_;
      Vector3 velocity{};
      for (unsigned i = 0; i < 3; ++i)
        velocity[i] = (scene.origin[i] - origin_[i]) / gap;
      const double speed = length(velocity);
      if (gap <= kMaximumReadGapSeconds && std::isfinite(speed) && speed <= kMaximumSpeed) {
        for (unsigned i = 0; i < 3; ++i)
          velocity_[i] = velocity_valid_ ? velocity_[i] + kSmoothing * (velocity[i] - velocity_[i]) : velocity[i];
        velocity_valid_ = true;
      } else {
        velocity_valid_ = false;
      }
    }
    if (!same_session || (std::isfinite(now_s) && now_s > read_s_)) {
      origin_ = scene.origin;
      read_s_ = std::isfinite(now_s) ? now_s : 0;
      epoch_ = epoch;
      resets_ = resets;
      valid_ = true;
    }
    if (!velocity_valid_)
      velocity_ = {};
    const double frame = std::isfinite(frame_s) && frame_s > 0 ? std::fmin(frame_s, kMaximumFrameSeconds) : 0;
    const double applied = std::isfinite(frames) && frames > 0 && frames <= kMaximumLeadFrames ? frames : 0;
    speed_ = length(velocity_);
    step_metres_ = speed_ * frame;
    lead_metres_ = step_metres_ * applied;
    BodyPose led = scene;
    for (unsigned i = 0; i < 3; ++i)
      led.origin[i] += velocity_[i] * frame * applied;
    return led;
  }
  void reset() noexcept { *this = {}; }
  // Smoothed speed (m/s), the movement during the frame being drawn and the
  // lead applied to the cameras (metres) at the last read.
  double speed() const noexcept { return speed_; }
  double step_metres() const noexcept { return step_metres_; }
  double lead_metres() const noexcept { return lead_metres_; }

 private:
  static double length(const Vector3& v) noexcept { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
  Vector3 origin_{}, velocity_{};
  double read_s_ = 0, speed_ = 0, step_metres_ = 0, lead_metres_ = 0;
  std::uint64_t epoch_ = 0, resets_ = 0;
  bool valid_ = false, velocity_valid_ = false;
};

}  // namespace taxi_camera::native_camera
