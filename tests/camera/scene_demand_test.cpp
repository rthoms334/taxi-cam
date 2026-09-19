#include "../../src/shared/scene_demand.hpp"
#include <cstdio>
#include <limits>
#include <stdexcept>
#include "../../src/camera/entry_pair.hpp"
#include "../../src/camera/probe_inspection_gate.hpp"
#include "../../src/camera/render_schedule.hpp"

namespace {
namespace ec = taxi_camera::engine_camera;
void require(bool ok, const char* text) {
  if (!ok)
    throw std::runtime_error(text);
}
struct Engine {
  unsigned creates = 0, erases = 0;
  static bool initialize(void*, ec::DescriptorStorage& d) noexcept {
    d.bytes.fill(0);
    d.bytes[44] = 1;
    return true;
  }
  static ec::EntryId create(void* p, ec::ManagerToken, const ec::DescriptorStorage&) noexcept {
    return 1000 + ++static_cast<Engine*>(p)->creates;
  }
  static bool erase(void* p, ec::ManagerToken, ec::EntryId) noexcept {
    ++static_cast<Engine*>(p)->erases;
    return true;
  }
};
void button_sequence(unsigned rate) {
  Engine engine;
  ec::PairController pair;
  const ec::ManagerToken owner{11, 1};
  const ec::EngineCallbacks callbacks{&engine, Engine::initialize, Engine::create, Engine::erase};
  taxi_camera::native_camera::RenderSchedule schedule;
  schedule.configure(rate);
  bool requested = false;
  std::uint64_t time = 0;
  unsigned starts = 0;
  std::array<ec::EntryId, 2> retained{};
  const auto phase = [&](bool wanted, unsigned duration) {
    std::array<unsigned, 2> pulses{};
    const auto end = time + duration;
    for (; time < end; time += 10) {
      const auto demand = taxi_camera::standalone::scene_demand(wanted ? 3u : 0u, false, 3, requested, false);
      if (demand.start) {
        pair.request_independent_pose();
        requested = true;
        ++starts;
      }
      pair.process_update(owner, callbacks);
      const auto state = pair.snapshot();
      if (requested) {
        require(state.state == ec::State::active, "Button change retired the owned pair");
        if (!retained[0])
          retained = state.owned_ids;
        require(state.owned_ids == retained, "Button change replaced camera IDs");
      }
      const auto gates = schedule.tick(time, demand.suspend);
      require(wanted || (!gates[0] && !gates[1]), "OFF left a render gate open");
      require(!(gates[0] && gates[1]), "Resume opened both cameras in one update");
      for (unsigned i = 0; i < 2; ++i)
        pulses[i] += gates[i];
    }
    if (wanted)
      require(pulses[0] && pulses[1], "A resumed camera was starved");
  };
  phase(false, 1000);
  require(engine.creates == 0, "Idle before first TAXI created cameras");
  phase(true, 103000);  // Observed left TAXI run.
  phase(false, 8000);   // Observed OFF interval before right TAXI.
  phase(true, 30000);   // Right TAXI must reuse both scene views.
  for (unsigned i = 0; i < 100; ++i) {
    phase(false, 1000);  // Also used for cutoff, service pause and lost heartbeat.
    phase(true, 1000);   // Either or both PFDs use the same nose/tail pair.
  }
  require(starts == 1 && engine.creates == 2 && engine.erases == 0, "Display demand performed native reallocation");
  pair.request_disable();  // Explicit terminal cleanup remains separate from display demand.
  pair.process_update(owner, callbacks);
  require(engine.erases == 2 && pair.snapshot().state == ec::State::disabled, "Explicit lifecycle cleanup was lost");
}
void discovery_sequence() {
  Engine engine;
  ec::PairController pair;
  const ec::ManagerToken owner{11, 1};
  const ec::EngineCallbacks callbacks{&engine, Engine::initialize, Engine::create, Engine::erase};
  bool requested = false;
  unsigned starts = 0;
  const auto update = [&](unsigned accepted, bool test_scene, unsigned assigned, unsigned expected_stamp, bool expected_suspend) {
    const auto demand = taxi_camera::standalone::scene_demand(accepted, test_scene, assigned, requested, false);
    require(demand.stamp_mask == expected_stamp, "An unrequested or undiscovered display was stamped");
    require(demand.suspend == expected_suspend, "Scene suspension depended on display discovery");
    if (demand.start) {
      pair.request_independent_pose();
      requested = true;
      ++starts;
    }
    pair.process_update(owner, callbacks);
  };
  update(0, false, 3, 0, true);
  require(engine.creates == 0, "Discovered targets created cameras without accepted demand");
  update(1, false, 0, 0, false);
  require(starts == 1 && engine.creates == 2, "TAXI before target discovery did not prepare the pair");
  const auto ids = pair.snapshot().owned_ids;
  for (unsigned i = 0; i < 100; ++i)
    update(1, false, 0, 0, false);
  update(1, false, 2, 0, false);  // Opposite display becoming ready cannot receive this button's output.
  update(1, false, 3, 1, false);
  update(3, false, 1, 1, false);  // A missing second target does not block the first target.
  update(0, false, 3, 0, true);
  update(2, false, 0, 0, false);
  update(2, false, 3, 2, false);
  update(0, true, 3, 0, false);  // Scene-only testing never writes either PFD.
  update(0, false, 3, 0, true);
  require(starts == 1 && engine.creates == 2 && engine.erases == 0 && pair.snapshot().owned_ids == ids,
          "Discovery, OFF or the other button recreated/erased the pair");
}
void prewarm_readiness() {
  using taxi_camera::standalone::ScenePrewarmReadiness;
  const ScenePrewarmReadiness ready{true, true, true, true, true, false, true, true, true, true, true, false, 0, true};
  require(ready.eligible(), "A matched grounded ready session was refused");
  for (auto member : {&ScenePrewarmReadiness::connected, &ScenePrewarmReadiness::session_settings, &ScenePrewarmReadiness::enabled,
                      &ScenePrewarmReadiness::matching_identity, &ScenePrewarmReadiness::graphics_ready,
                      &ScenePrewarmReadiness::buttons_valid, &ScenePrewarmReadiness::pose_ready, &ScenePrewarmReadiness::on_ground_valid,
                      &ScenePrewarmReadiness::on_ground, &ScenePrewarmReadiness::speed_valid, &ScenePrewarmReadiness::session_ready}) {
    auto unavailable = ready;
    unavailable.*member = false;
    require(!unavailable.eligible(), "Missing background safety evidence admitted prewarm");
  }
  for (auto member : {&ScenePrewarmReadiness::cutoff, &ScenePrewarmReadiness::diagnostics_active}) {
    auto prohibited = ready;
    prohibited.*member = true;
    require(!prohibited.eligible(), "Cutoff/calibration diagnostic admitted prewarm");
  }
  for (double speed : {-1.0, 0.50001, 60.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    auto moving = ready;
    moving.speed_knots = speed;
    require(!moving.eligible(), "Moving or invalid groundspeed admitted prewarm");
  }
}
void prewarm_sequence(unsigned rate) {
  namespace win = taxi_camera::standalone;
  Engine engine;
  ec::PairController pair;
  const ec::ManagerToken owner{11, 1};
  const ec::EngineCallbacks callbacks{&engine, Engine::initialize, Engine::create, Engine::erase};
  taxi_camera::native_camera::RenderSchedule schedule;
  schedule.configure(rate);
  win::ScenePrewarm warmup;
  bool requested = false;
  unsigned starts = 0;
  for (std::uint64_t now = 0; now < 20000; now += 10) {
    const bool eligible = now >= 1000;
    const bool foreground = now >= 10000 && (now / 1000) % 2;
    // The first image is available before the GPU has completed all three pairs.
    const std::uint64_t completed = now < 2000 ? 0 : now < 2200 ? 1 : now < 2400 ? 2 : 3;
    const win::ScenePrewarmProgress progress{now >= 1500, now >= 1800, now >= 1800 ? 3u : 0u, completed};
    const bool warm = warmup.observe(now, eligible, foreground, progress, false);
    const auto demand = win::scene_demand(foreground ? 3u : 0u, false, 3, requested, false, warm);
    require(demand.stamp_mask == (foreground ? 3u : 0u), "Prewarm wrote an unrequested PFD");
    if (demand.start) {
      ++starts;
      requested = true;
      pair.request_independent_pose();
    }
    const auto before = pair.snapshot();
    if (!taxi_camera::native_camera::park_initial_scene(demand.suspend, before.owned_ids[0] || before.owned_ids[1]))
      pair.process_update(owner, callbacks);
    const auto gates = schedule.tick(now, demand.suspend);
    if (!warm && !foreground)
      require(!gates[0] && !gates[1], "Completed/idle warmup left native rendering active");
    if (now < 1000)
      require(engine.creates == 0, "Missing startup telemetry created cameras");
    if (now >= 1800 && now < 2400)
      require(warm, "Submitted or first-pair output ended warmup before three GPU completions");
    if (now >= 2400 && now < 10000)
      require(warmup.phase() == win::ScenePrewarm::Phase::ready, "Three completed pairs were not retained parked");
  }
  require(starts == 1 && engine.creates == 2 && engine.erases == 0, "Prewarm and button toggles allocated extra native cameras");
}
void prewarm_loading_and_completion() {
  namespace win = taxi_camera::standalone;
  using Phase = win::ScenePrewarm::Phase;
  win::ScenePrewarm warm;
  require(warm.observe(1000, true, false, {}, false), "Ready session did not begin preparation");
  require(warm.observe(11000, true, false, {}, false) && warm.phase() == Phase::preparing,
          "Ten-second shader preparation consumed the rendering budget");
  require(!warm.observe(11100, false, false, {}, false) && warm.phase() == Phase::paused,
          "Lost heartbeat/telemetry did not park the warmup");
  require(warm.observe(70000, true, false, {}, false) && warm.phase() == Phase::preparing,
          "Stable loaded aircraft could not resume the original preparation");
  require(warm.started_ms() == 1000, "Resuming preparation reset the overall work budget");
  require(warm.observe(71000, true, false, {true, true, 3, 0}, false), "Native readiness did not start rendering");
  for (unsigned completed = 0; completed < 3; ++completed)
    require(warm.observe(71100 + completed * 100, true, false, {true, true, 3, completed}, false),
            "CPU submissions or fewer than three GPU completions ended warmup");
  require(!warm.observe(71500, true, false, {true, true, 3, 3}, false) && warm.phase() == Phase::ready,
          "GPU completion did not park the prepared cameras");
  require(!warm.observe(72000, true, false, {true, true, 3, 3}, false), "Ready cameras rendered indefinitely while TAXI was off");

  win::ScenePrewarm retained;
  require(retained.observe(1000, true, false, {true, true, 50, 49}, false), "Retained views skipped fresh warmup");
  require(retained.observe(1100, true, false, {true, true, 50, 50}, false), "Old in-flight pair counted as new-session warmup");
  require(retained.observe(1200, true, false, {true, true, 53, 52}, false), "Only two new pairs completed");
  require(retained.observe(1250, true, false, {false, false, 53, 53}, false), "Obsolete scene output marked warmup ready");
  require(!retained.observe(1300, true, false, {true, true, 53, 53}, false) && retained.phase() == Phase::ready,
          "New-session pairs did not complete the retained warmup");
}
void prewarm_limits_and_takeover() {
  namespace win = taxi_camera::standalone;
  using Phase = win::ScenePrewarm::Phase;
  for (unsigned cause = 0; cause < 4; ++cause) {
    win::ScenePrewarm warmup;
    require(warmup.observe(1000, true, false, {}, false), "Ready session did not begin warmup");
    const bool render_timeout = cause == 1;
    if (render_timeout)
      require(warmup.observe(1100, true, false, {true, false, 0, 0}, false), "Ready views did not begin render budget");
    const auto now = cause == 0 ? 1000 + win::ScenePrewarm::MaximumStartupMs : render_timeout ? 6100 : cause == 3 ? 999 : 1200;
    require(!warmup.observe(now, true, false, {}, cause == 2), "Failure/deadline left warmup rendering");
    require(warmup.phase() == (cause == 2 ? Phase::failed : Phase::timed_out), "Incorrect terminal warmup state");
    for (unsigned retry = 0; retry < 100; ++retry)
      require(!warmup.observe(now + 1 + retry, true, false, {}, false), "Terminal warmup restarted in the background");
    warmup = {};
    require(warmup.observe(200000, true, false, {}, false), "A new aircraft session could not warm the retained pair");
  }
  win::ScenePrewarm paused;
  require(paused.observe(1000, true, false, {}, false), "Pause fixture start");
  require(!paused.observe(2000, false, false, {}, false), "Pause did not close gates");
  require(!paused.observe(121000, false, false, {}, false) && paused.phase() == Phase::timed_out,
          "Indefinite ineligibility escaped the overall deadline");
  for (unsigned phase = 0; phase < 4; ++phase) {
    win::ScenePrewarm foreground;
    if (phase) {
      require(foreground.observe(1000, true, false, {}, false), "Takeover fixture start");
      foreground.observe(1100, phase != 3, false, {phase == 2, false, 0, 0}, false);
    }
    require(!foreground.observe(1200, true, true, {}, false) && foreground.phase() == Phase::foreground,
            "Foreground request waited for background preparation/completion");
    require(!foreground.observe(2000, true, false, {}, false), "Foreground OFF restarted background rendering");
  }
  Engine engine;
  ec::PairController pair;
  const ec::EngineCallbacks callbacks{&engine, Engine::initialize, Engine::create, Engine::erase};
  pair.request_independent_pose();
  for (unsigned i = 0; i < 100; ++i) {
    const auto before = pair.snapshot();
    if (!taxi_camera::native_camera::park_initial_scene(true, before.owned_ids[0] || before.owned_ids[1]))
      pair.process_update({11, 1}, callbacks);
  }
  require(engine.creates == 0 && pair.snapshot().request_pending, "Suspended pending creation was consumed");
  pair.process_update({11, 1}, callbacks);
  require(engine.creates == 2 && engine.erases == 0, "Resume did not use the one queued pair");
  require(!taxi_camera::native_camera::park_initial_scene(true, true), "Owned pair gate closure/cleanup was skipped");
}
void prewarm_unqueued_failure_then_taxi() {
  namespace win = taxi_camera::standalone;
  for (unsigned cause = 0; cause < 4; ++cause) {
    win::ScenePrewarm warmup;
    Engine engine;
    ec::PairController pair;
    const ec::EngineCallbacks callbacks{&engine, Engine::initialize, Engine::create, Engine::erase};
    bool requested = false, failed = false;
    const bool background = warmup.observe(1000, true, false, {}, false);
    require(win::scene_demand(0, false, 3, requested, failed, background).start, "Background preparation not admitted");
    const auto after_prepare = cause == 1 ? 121000u : 1100u;
    const bool allowed = warmup.observe(after_prepare, cause != 0, false, {}, cause == 2);
    require(allowed == (cause == 3), "The wrong pre-native stage admitted the start");
    failed = win::finish_scene_start(warmup, background, requested);
    require(!failed && !requested && engine.creates == 0 && !pair.snapshot().request_pending,
            "Unqueued background failure latched foreground failure or queued native creation");
    const bool foreground_background = warmup.observe(after_prepare + 1, true, true, {}, failed);
    const auto taxi = win::scene_demand(1, false, 1, requested, failed, foreground_background);
    require(taxi.start && !taxi.suspend && taxi.stamp_mask == 1, "First TAXI needed a retoggle after interrupted preparation");
    pair.request_independent_pose();
    pair.process_update({11, 1}, callbacks);
    requested = pair.snapshot().state == ec::State::active;
    failed = win::finish_scene_start(warmup, false, requested);
    require(requested && !failed && engine.creates == 2 && engine.erases == 0, "Foreground did not create exactly one pair");
    for (unsigned retry = 0; retry < 100; ++retry)
      require(!warmup.observe(after_prepare + 2 + retry, true, false, {}, false), "Warmup restarted after foreground takeover");
  }
  win::ScenePrewarm explicit_failure;
  const bool failed = win::finish_scene_start(explicit_failure, false, false);
  const auto latched = win::scene_demand(1, false, 1, false, failed);
  require(failed && !latched.start && latched.suspend && !latched.stamp_mask, "Foreground failure lost its no-retry latch");
  require(win::retain_background_prewarm(true, false, true),
          "Readiness lost during contract resolution parked the only background start");
  require(!win::retain_background_prewarm(true, false, false), "A hard background refusal was retried");
  require(!win::retain_background_prewarm(true, true, true), "An accepted start stayed deferred");
  require(!win::retain_background_prewarm(false, false, true), "Foreground readiness loss bypassed its failure latch");
  win::ScenePrewarm retained_wait;
  require(retained_wait.observe(1000, true, false, {}, false), "Deferred background start did not begin");
  require(retained_wait.phase() == win::ScenePrewarm::Phase::preparing, "Deferred background start left preparing");
  require(retained_wait.observe(1500, true, false, {}, false) && retained_wait.phase() == win::ScenePrewarm::Phase::preparing,
          "The same background attempt was not kept for a later readiness sample");
}
}  // namespace
int main() {
  try {
    button_sequence(15);
    button_sequence(60);
    discovery_sequence();
    prewarm_readiness();
    prewarm_sequence(15);
    prewarm_sequence(60);
    prewarm_loading_and_completion();
    prewarm_limits_and_takeover();
    prewarm_unqueued_failure_then_taxi();
    const auto failed = taxi_camera::standalone::scene_demand(3, false, 3, false, true);
    require(!failed.start && failed.suspend && !failed.stamp_mask, "A failed start retried or stamped without permission");
    const auto test = taxi_camera::standalone::scene_demand(0, true, 0, false, false);
    require(test.start && !test.suspend && !test.stamp_mask, "Scene test incorrectly required a display target");
    const auto unknown = taxi_camera::standalone::scene_demand(4, false, 7, false, false);
    require(!unknown.start && unknown.suspend && !unknown.stamp_mask, "Unknown demand bits created or stamped views");
    std::puts(
        "PASS scene demand: prepare once before discovery, matched-target-only writes, scene-only test, left/OFF/right, retained "
        "pair, three completed background pairs, loading pause/resume, setup/render budgets and closed idle gates at 15/60 fps. Mock "
        "engine only.");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL scene demand: %s\n", e.what());
    return 1;
  }
}
