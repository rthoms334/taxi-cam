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

inline bool latched_scene_stop(SceneStopReason reason) noexcept {
  // A failed native creation/initial resize may have partially allocated output.
  // Later inspection failures must neither replace that cause nor rearm retry.
  return reason == SceneStopReason::identity_refused || reason == SceneStopReason::creation_failed;
}

inline bool temporary_pose_unavailable(const char* reason) noexcept {
  return reason && (!std::strcmp(reason, "telemetry_busy") || !std::strcmp(reason, "aircraft_telemetry_stale") ||
                    !std::strcmp(reason, "camera_telemetry_stale") || !std::strcmp(reason, "not_initialized") ||
                    !std::strcmp(reason, "aircraft_session_changed"));
}

// Body-pose sample detail when the aircraft is more than 10 km from the local
// calibration origin (typical after a long sector with a parked pair).
inline bool outside_local_calibration_radius(const char* reason) noexcept {
  return reason && !std::strcmp(reason, "outside_local_calibration_radius");
}

// Active-pair policy when a pose capture fails. Issue 54: a stale local lock
// must not latch pose_invalid (that needs a manual deactivate/activate). Issue
// 84: nor may it retire the pair, since the replacement pair it forces is the
// RenderThreadProc crash path. Retain the closed pair from the stale report
// until the arrival latch completes. Calibration pending on its own, outside a
// relocation, keeps its existing handling.
inline bool retain_pair_for_recalibration(bool stale_local_calibration, bool retained_recalibration, bool calibration_pending) noexcept {
  return stale_local_calibration || (retained_recalibration && calibration_pending);
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
    // Only explicit Start/Stop transitions can clear a latched refusal. Preserve
    // its original reason even when a later failure is independently terminal.
    if (latched_scene_stop(reason_) && reason != reason_)
      return;
    // A pending failure is one recovery episode. Repeated unavailable samples
    // may report a different field/region, but must not keep postponing its
    // original retry deadline. A fresh guarded update still owns every retry.
    if (pending_ && reason_ == reason)
      return;
    reason_ = reason;
    stopped_at_ = now;
    healthy_since_ = last_progress_ = 0;
    pending_ = requested_ && retryable_scene_stop(reason) && attempts_ < maximum_retries;
    ++sequence_;
  }
  bool retry(std::uint64_t now, const engine_camera::Snapshot& pair, bool fresh_pose) noexcept {
    if (!requested_ || !pending_ || !fresh_pose || now < stopped_at_ || now - stopped_at_ < retry_delay_ms ||
        engine_camera::any_owned(pair.owned_ids) || pair.request_pending || pair.creation_pending ||
        pair.state != engine_camera::State::disabled || pair.failure != engine_camera::Failure::none ||
        pair.blocked != engine_camera::Blocked::none)
      return false;
    pending_ = false;
    ++attempts_;
    return true;
  }
  void resumed_retained_resolution() noexcept {
    if (requested_ && !pending_ && reason_ == SceneStopReason::resolution_changed) {
      reason_ = SceneStopReason::none;
      // Bump the control-loop snapshot so "Camera dimensions restored" is logged.
      ++sequence_;
    }
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

// A first manager failure can precede public/private pose calibration. Permit
// only that read-only prerequisite while recovery waits; retry itself still
// requires a usable pose and owns the delay/attempt budget. The caller already
// validated the current manager and serializes request revisions with Stop.
inline bool initial_retry_calibration_allowed(const SceneRecovery& recovery,
                                              const engine_camera::Snapshot& pair,
                                              bool calibration_required,
                                              bool suspended,
                                              std::uint64_t inspected_revision,
                                              std::uint64_t current_revision) noexcept {
  return calibration_required && !suspended && inspected_revision == current_revision && recovery.requested() && recovery.pending() &&
         retryable_scene_stop(recovery.reason()) && recovery.attempts() < SceneRecovery::maximum_retries &&
         pair.state == engine_camera::State::disabled && pair.failure == engine_camera::Failure::none &&
         pair.blocked == engine_camera::Blocked::none && pair.owner == engine_camera::ManagerToken{} &&
         !engine_camera::any_owned(pair.owned_ids) && !pair.request_pending && !pair.creation_pending;
}

// Called under the probe request mutex, after the first initializer refused a
// temporary pose inspection. The enable was consumed, but no native descriptor
// initializer/create ran. Clear only this empty controller failure; supplying
// no callbacks makes native cleanup/creation impossible here. The next retry
// still needs the ordinary fresh manager, pose, pool and request guards.
inline bool defer_initial_pose_failure(engine_camera::PairController& controller,
                                       engine_camera::ManagerToken manager,
                                       SceneRecovery& recovery,
                                       bool temporary_pose,
                                       unsigned created,
                                       std::uint64_t inspected_revision,
                                       std::uint64_t current_revision,
                                       std::uint64_t now) noexcept {
  const auto pair = controller.snapshot();
  if (!temporary_pose || created != 0 || inspected_revision != current_revision || !recovery.requested() ||
      recovery.attempts() >= SceneRecovery::maximum_retries ||
      (recovery.reason() != SceneStopReason::none && !retryable_scene_stop(recovery.reason())) ||
      pair.state != engine_camera::State::failed || pair.failure != engine_camera::Failure::initializer_failed ||
      pair.blocked != engine_camera::Blocked::none || pair.owner != engine_camera::ManagerToken{} ||
      engine_camera::any_owned(pair.owned_ids) || pair.request_pending || pair.creation_pending)
    return false;
  controller.request_disable();
  if (!controller.process_update(manager, {}))
    return false;
  const auto clean = controller.snapshot();
  if (clean.state != engine_camera::State::disabled || clean.failure != engine_camera::Failure::none ||
      clean.blocked != engine_camera::Blocked::none || engine_camera::any_owned(clean.owned_ids) || clean.request_pending ||
      clean.creation_pending)
    return false;
  recovery.failed(SceneStopReason::inspection_unavailable, now);
  return recovery.pending();
}

}  // namespace taxi_camera::native_camera
