#include "../../src/camera/scene_recovery.hpp"
#include "view_retirement_test.hpp"

#include <cstdio>
#include <cstdlib>

namespace {
namespace ec = taxi_camera::engine_camera;
using namespace taxi_camera::native_camera;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}
struct Engine {
  unsigned creates = 0, erases = 0;
  bool allow_erase = false;
  static bool initialize(void*, ec::DescriptorStorage& descriptor) noexcept {
    descriptor.bytes.fill(0);
    return true;
  }
  static ec::EntryId create(void* context, ec::ManagerToken, const ec::DescriptorStorage&) noexcept {
    return 100 + ++static_cast<Engine*>(context)->creates;
  }
  static bool erase(void* context, ec::ManagerToken, ec::EntryId) noexcept {
    auto& engine = *static_cast<Engine*>(context);
    ++engine.erases;
    return engine.allow_erase;
  }
  ec::EngineCallbacks callbacks() { return {this, initialize, create, erase}; }
};
}  // namespace

int main() {
  test_view_retirement();
  SceneRecovery recovery;
  ec::Snapshot clean;
  require(!recovery.retry(9999, clean, true), "No initial invented request");
  for (const auto* error : {"telemetry_busy", "aircraft_telemetry_stale", "camera_telemetry_stale", "not_initialized"})
    require(temporary_pose_unavailable(error), "Temporary telemetry errors retain a closed pair");
  for (const auto* error : {"", "invalid_basis", "outside_calibration_radius", "identity_mismatch"})
    require(!temporary_pose_unavailable(error), "Identity and numeric failures remain fatal");
  require(!temporary_pose_unavailable(nullptr), "Null error never classified temporary");
  for (const auto reason :
       {SceneStopReason::none, SceneStopReason::explicit_stop, SceneStopReason::identity_refused, SceneStopReason::pose_invalid,
        SceneStopReason::creation_failed, SceneStopReason::exception, SceneStopReason::capture_stalled}) {
    recovery.start();
    recovery.failed(reason, 10);
    require(!recovery.pending() && !recovery.retry(99999, clean, true), "Fatal failure cannot retry");
  }
  for (const auto reason : {SceneStopReason::inspection_unavailable, SceneStopReason::owned_entry_absent}) {
    recovery.start();
    recovery.failed(reason, 100);
    require(recovery.pending() && recovery.reason() == reason, "Recoverable reason is recorded");
    require(!recovery.retry(99, clean, true), "Clock regression refuses retry");
    require(!recovery.retry(2099, clean, true), "Retry delay boundary");
    require(!recovery.retry(2100, clean, false), "No stale pose retry");
    for (unsigned field = 0; field < 7; ++field) {
      auto incomplete = clean;
      if (field == 0)
        incomplete.owned_ids[0] = 1;
      if (field == 1)
        incomplete.owned_ids[1] = 2;
      if (field == 2)
        incomplete.request_pending = true;
      if (field == 3)
        incomplete.creation_pending = true;
      if (field == 4)
        incomplete.state = ec::State::cleanup_pending;
      if (field == 5)
        incomplete.failure = ec::Failure::manager_destroyed;
      if (field == 6)
        incomplete.blocked = ec::Blocked::manager_mismatch;
      require(!recovery.retry(2100, incomplete, true), "Unconfirmed cleanup or invalid owner blocks retry");
    }
    require(recovery.retry(2100, clean, true), "Confirmed clean state retries after the delay");
    require(!recovery.retry(2100, clean, true), "Same failure cannot queue duplicate creation");
  }

  // Exercise the actual ownership controller through a failed erase and retry.
  Engine engine;
  ec::PairController pair;
  constexpr ec::ManagerToken manager{7, 1};
  recovery.start();
  pair.request_independent_pose();
  require(pair.process_update(manager, engine.callbacks()), "Initial controller update");
  const auto original = pair.snapshot().owned_ids;
  require(original[0] && original[1] && engine.creates == 2, "Initial owned pair exists");
  // A missing pose closes gates in the caller, without issuing a disable.
  require(temporary_pose_unavailable("aircraft_telemetry_stale"), "Stale pose suspends");
  require(pair.process_update(manager, engine.callbacks()), "Suspended controller update");
  require(pair.snapshot().owned_ids == original && engine.erases == 0 && engine.creates == 2,
          "Temporary pose gap retains IDs without repeated creation or erase");
  recovery.failed(SceneStopReason::resolution_changed, 90);
  require(!recovery.pending() && !recovery.retry(5000, pair.snapshot(), true), "Resolution change cannot recreate retained views");
  recovery.resumed_retained_resolution();
  require(recovery.reason() == SceneStopReason::none && recovery.requested(), "Retained resolution recovery preserves demand");
  recovery.failed(SceneStopReason::owned_entry_absent, 100);
  pair.request_disable();
  require(pair.process_update(manager, engine.callbacks()), "First cleanup update");
  require(pair.snapshot().state == ec::State::cleanup_pending && pair.snapshot().owned_ids == original,
          "Unconfirmed erase preserves both IDs");
  require(!recovery.retry(5000, pair.snapshot(), true), "No retry while either ID remains");
  engine.allow_erase = true;
  require(pair.process_update(manager, engine.callbacks()), "Confirmed cleanup update");
  require(recovery.retry(5000, pair.snapshot(), true), "Retry after actual controller confirms absence");
  pair.request_independent_pose();
  require(pair.process_update(manager, engine.callbacks()), "Retry creation update");
  require(engine.creates == 4 && pair.snapshot().owned_ids[0] != original[0], "Exactly one new pair created");

  recovery.failed(SceneStopReason::inspection_unavailable, 6000);
  recovery.stop();
  pair.request_disable();
  require(pair.process_update(manager, engine.callbacks()), "Fresh OFF cleanup");
  require(!recovery.requested() && !recovery.pending() && !recovery.retry(99999, pair.snapshot(), true),
          "Explicit OFF cancels pending recovery immediately");
  require(engine.creates == 4, "Fresh OFF never recreates");
  recovery.start();
  for (unsigned attempt = 1; attempt <= SceneRecovery::maximum_retries; ++attempt) {
    recovery.failed(SceneStopReason::owned_entry_absent, attempt * 3000);
    require(recovery.retry(attempt * 3000 + 2000, clean, true) && recovery.attempts() == attempt, "Bounded retry accepted");
  }
  recovery.failed(SceneStopReason::owned_entry_absent, 20000);
  require(!recovery.pending() && !recovery.retry(99999, clean, true), "Retry storm is bounded");
  recovery.start();
  require(recovery.attempts() == 0 && recovery.reason() == SceneStopReason::none, "Explicit new Start resets retry budget");
  recovery.failed(SceneStopReason::inspection_unavailable, 1000);
  require(recovery.retry(3000, clean, true), "Capture stall uses confirmed cleanup policy");
  recovery.capture_progress(4000);
  recovery.capture_progress(20000);
  require(recovery.attempts() == 1, "Long capture gaps cannot refill the retry budget");
  for (std::uint64_t time = 21000; time < 30000; time += 1000)
    recovery.capture_progress(time);
  require(recovery.attempts() == 1, "Healthy interval must complete before refill");
  recovery.capture_progress(30000);
  require(recovery.attempts() == 0, "Ten seconds of advancing captures refill recovery budget");
  recovery.failed(SceneStopReason::identity_refused, 31000);
  for (std::uint64_t time = 32000; time < 50000; time += 1000)
    recovery.capture_progress(time);
  require(recovery.reason() == SceneStopReason::identity_refused && !recovery.pending(), "Capture progress cannot clear identity failure");
  std::printf("Scene recovery: PASS %u checks\n", checks);
}
