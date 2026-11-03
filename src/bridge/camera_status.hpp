#pragma once

#include "../camera/probe.hpp"

namespace taxi_camera {

inline const char* camera_transition_message(const native_camera::ProbeSnapshot& scene) noexcept {
  if (scene.profile_transition_failed)
    return "Aircraft change paused: camera lifecycle validation failed. Restart MSFS to retry.";
  if (!scene.pair.owned_ids[0] && !scene.pair.owned_ids[1])
    return "Waiting for pending camera startup to finish before changing aircraft profile.";
  return "Closing and validating retained camera views for the aircraft profile.";
}

// The camera manager began a transition of its own after the bridge applied
// one (its token moved on) while the flight stayed loaded: it retired a kept
// pair it could not resize or revalidate, or latched a session reset. The
// bridge has already requested its cameras, so it waits for that transition
// like its own and requests them again. A flight change (loading, or a new
// session epoch) keeps its own transition path.
inline bool camera_manager_transition(std::uint64_t applied_token,
                                      const native_camera::ProbeSnapshot& scene,
                                      bool session_unchanged) noexcept {
  return applied_token && session_unchanged && scene.profile_transition_token != applied_token;
}

// A retained stop reason can outlive successful recovery. Only temporary
// failures on a stopped pair (or pending cleanup) override normal capture status.
inline const char* camera_stop_message(const native_camera::ProbeSnapshot& scene) noexcept {
  using native_camera::SceneStopReason;
  if (scene.stop_reason == SceneStopReason::none || scene.stop_reason == SceneStopReason::explicit_stop ||
      scene.stop_reason == SceneStopReason::resolution_changed)
    return nullptr;
  if (native_camera::retryable_scene_stop(scene.stop_reason)) {
    if (scene.recovery_pending)
      return "Camera startup interrupted; waiting for safe cleanup before retrying.";
    if (scene.restart_pending)
      return "Retrying camera startup; waiting for a validated aircraft position and camera state.";
    if (scene.pair.state == engine_camera::State::active)
      return nullptr;
    return "Camera startup stopped after validation failed. See Diagnostics for details.";
  }
  switch (scene.stop_reason) {
    case SceneStopReason::identity_refused:
      return "Camera safety check failed: simulator camera identity could not be verified. See Diagnostics for details.";
    case SceneStopReason::pose_invalid:
      return "Camera startup stopped: aircraft position could not be validated. See Diagnostics for details.";
    case SceneStopReason::creation_failed:
      return "Camera startup failed: the simulator did not create both camera views. See Diagnostics for details.";
    case SceneStopReason::capture_stalled:
      return "Camera capture stopped. See Diagnostics for details.";
    case SceneStopReason::exception:
      return "Camera startup stopped after an internal error. See Diagnostics for details.";
    default:
      return nullptr;
  }
}

}  // namespace taxi_camera
