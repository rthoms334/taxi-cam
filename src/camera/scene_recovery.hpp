#pragma once

#include "entry_pair.hpp"

#include <cstdint>
#include <cstring>

namespace taxi_camera::native_camera {

enum class SceneStopReason {
  none,
  explicit_stop,
  inspection_unavailable,
  owned_entry_absent,
  resolution_changed,
  capture_stalled,
  identity_refused,
  pose_invalid,
  creation_failed,
  exception
};

inline const char* scene_stop_reason_name(SceneStopReason reason) noexcept {
  switch (reason) {
    case SceneStopReason::none:
      return "none";
    case SceneStopReason::explicit_stop:
      return "explicit_stop";
    case SceneStopReason::inspection_unavailable:
      return "inspection_unavailable";
    case SceneStopReason::owned_entry_absent:
      return "owned_entry_absent";
    case SceneStopReason::resolution_changed:
      return "resolution_changed";
    case SceneStopReason::capture_stalled:
      return "capture_stalled";
    case SceneStopReason::identity_refused:
      return "identity_refused";
    case SceneStopReason::pose_invalid:
      return "pose_invalid";
    case SceneStopReason::creation_failed:
      return "creation_failed";
    case SceneStopReason::exception:
      return "exception";
  }
  return "unknown";
}

inline bool retryable_scene_stop(SceneStopReason reason) noexcept {
  // Dimension drift uses the retained-pair path. It must never trigger erase
  // and recreation of camera entries that the renderer may still reference.
  return reason == SceneStopReason::inspection_unavailable || reason == SceneStopReason::owned_entry_absent;
}

inline bool temporary_pose_unavailable(const char* reason) noexcept {
  return reason && (!std::strcmp(reason, "telemetry_busy") || !std::strcmp(reason, "aircraft_telemetry_stale") ||
                    !std::strcmp(reason, "camera_telemetry_stale") || !std::strcmp(reason, "not_initialized") ||
                    !std::strcmp(reason, "aircraft_session_changed"));
}

// Observer policy, serialized by the probe mailbox mutex. No native calls.
// Retry only after the existing controller has confirmed every old ID absent,
// and a fresh pose plus fresh manager validation is available to the caller.
class SceneRecovery {
 public:
  static constexpr unsigned maximum_retries = 3;
  static constexpr std::uint64_t retry_delay_ms = 2000;
  void start() noexcept {
    requested_ = true;
    pending_ = false;
    attempts_ = 0;
    reason_ = SceneStopReason::none;
    healthy_since_ = last_progress_ = 0;
  }
  void stop() noexcept {
    requested_ = false;
    pending_ = false;
    reason_ = SceneStopReason::explicit_stop;
    ++sequence_;
  }
  void failed(SceneStopReason reason, std::uint64_t now) noexcept {
    reason_ = reason;
    stopped_at_ = now;
    healthy_since_ = last_progress_ = 0;
    pending_ = requested_ && retryable_scene_stop(reason) && attempts_ < maximum_retries;
    ++sequence_;
  }
  bool retry(std::uint64_t now, const engine_camera::Snapshot& pair, bool fresh_pose) noexcept {
    if (!requested_ || !pending_ || !fresh_pose || now < stopped_at_ || now - stopped_at_ < retry_delay_ms || pair.owned_ids[0] ||
        pair.owned_ids[1] || pair.request_pending || pair.creation_pending || pair.state != engine_camera::State::disabled ||
        pair.failure != engine_camera::Failure::none || pair.blocked != engine_camera::Blocked::none)
      return false;
    pending_ = false;
    ++attempts_;
    return true;
  }
  void resumed_retained_resolution() noexcept {
    if (requested_ && !pending_ && reason_ == SceneStopReason::resolution_changed)
      reason_ = SceneStopReason::none;
  }
  bool requested() const noexcept { return requested_; }
  void capture_progress(std::uint64_t now) noexcept {
    if (!requested_ || pending_ || !retryable_scene_stop(reason_))
      return;
    if (!last_progress_ || now < last_progress_ || now - last_progress_ > 2000)
      healthy_since_ = now;
    last_progress_ = now;
    if (now >= healthy_since_ && now - healthy_since_ >= 10000) {
      attempts_ = 0;
      reason_ = SceneStopReason::none;
    }
  }
  bool pending() const noexcept { return pending_; }
  unsigned attempts() const noexcept { return attempts_; }
  std::uint64_t sequence() const noexcept { return sequence_; }
  SceneStopReason reason() const noexcept { return reason_; }

 private:
  bool requested_ = false;
  bool pending_ = false;
  unsigned attempts_ = 0;
  std::uint64_t sequence_ = 0;
  std::uint64_t stopped_at_ = 0;
  std::uint64_t healthy_since_ = 0, last_progress_ = 0;
  SceneStopReason reason_ = SceneStopReason::none;
};

}  // namespace taxi_camera::native_camera
