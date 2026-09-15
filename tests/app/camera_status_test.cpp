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
  std::printf("Camera status: PASS %u checks\n", checks);
}
