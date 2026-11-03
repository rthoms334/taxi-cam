#include "../../src/bridge/camera_status.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}
}  // namespace

int main() {
  using namespace taxi_camera;
  using native_camera::SceneStopReason;
  native_camera::ProbeSnapshot scene;
  for (const auto reason : {SceneStopReason::none, SceneStopReason::explicit_stop, SceneStopReason::resolution_changed}) {
    scene.stop_reason = reason;
    require(!camera_stop_message(scene), "Idle and retained resize keep their existing status");
  }
  for (const auto reason : {SceneStopReason::inspection_unavailable, SceneStopReason::owned_entry_absent}) {
    scene.stop_reason = reason;
    scene.recovery_pending = true;
    require(std::strstr(camera_stop_message(scene), "retrying"), "Pending cleanup reports a recovery wait");
    scene.recovery_pending = false;
    scene.restart_pending = true;
    require(std::strstr(camera_stop_message(scene), "Retrying"), "Accepted retry waiting for a pose is still in progress");
    scene.recovery_attempts = native_camera::SceneRecovery::maximum_retries;
    require(std::strstr(camera_stop_message(scene), "Retrying"), "Final accepted attempt is not reported exhausted before creation");
    scene.restart_pending = false;
    require(std::strstr(camera_stop_message(scene), "stopped"), "Exhausted recovery cannot say waiting for camera frames");
    scene.pair.state = engine_camera::State::active;
    require(!camera_stop_message(scene), "Successfully restarted pair does not show historical temporary error");
    scene.pair.state = engine_camera::State::disabled;
  }
  for (const auto reason : {SceneStopReason::identity_refused, SceneStopReason::pose_invalid, SceneStopReason::creation_failed,
                            SceneStopReason::capture_stalled, SceneStopReason::exception}) {
    scene.stop_reason = reason;
    require(camera_stop_message(scene), "Fatal stop overrides generic frame wait even after cleanup");
    scene.pair.state = engine_camera::State::active;
    require(camera_stop_message(scene), "Retained IDs do not hide a fatal stop");
    scene.pair.state = engine_camera::State::disabled;
  }
  scene.message = "Initial camera creation parked until render demand resumes.";
  scene.profile_transition_failed = true;
  require(std::strstr(camera_transition_message(scene), "validation failed") && !std::strstr(camera_transition_message(scene), "parked"),
          "A failed aircraft transition reused an unrelated prior camera message");
  scene.profile_transition_failed = false;
  scene.pair.request_pending = true;
  require(std::strstr(camera_transition_message(scene), "pending camera startup"),
          "Empty startup transition claimed to be validating existing cameras");
  scene.pair.owned_ids = {1001, 1002};
  require(std::strstr(camera_transition_message(scene), "retained camera views"),
          "Existing-camera transition lost its retained lifecycle status");
  // The camera manager started a transition of its own after the bridge
  // applied one, such as the full reset it takes for a kept pair it could not
  // resize: its new token sends the bridge back to the transition so the
  // cameras are requested again. (2026-11-03, PMDG 777: after that reset the
  // cameras stayed off until the next display-area change.)
  native_camera::ProbeSnapshot manager;
  manager.profile_transition_token = 14;
  require(!camera_manager_transition(14, manager, true), "The bridge's own applied transition was taken for a new one");
  require(!camera_manager_transition(0, manager, true), "No applied transition yet (or a stopped connection) was taken for one");
  manager.profile_transition_token = 16;
  require(camera_manager_transition(14, manager, true), "The camera manager's own reset after a resume was not followed");
  require(!camera_manager_transition(14, manager, false), "A flight change left its own transition path");
  std::printf("Camera status: PASS %u checks\n", checks);
}
