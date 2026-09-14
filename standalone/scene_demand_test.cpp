#include "scene_demand.hpp"
#include "../engine-camera/entry_pair.hpp"
#include "../native-camera/render_schedule.hpp"
#include <cstdio>
#include <stdexcept>

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
}  // namespace
int main() {
  try {
    button_sequence(15);
    button_sequence(60);
    discovery_sequence();
    const auto failed = taxi_camera::standalone::scene_demand(3, false, 3, false, true);
    require(!failed.start && failed.suspend && !failed.stamp_mask, "A failed start retried or stamped without permission");
    const auto test = taxi_camera::standalone::scene_demand(0, true, 0, false, false);
    require(test.start && !test.suspend && !test.stamp_mask, "Scene test incorrectly required a display target");
    const auto unknown = taxi_camera::standalone::scene_demand(4, false, 7, false, false);
    require(!unknown.start && unknown.suspend && !unknown.stamp_mask, "Unknown demand bits created or stamped views");
    std::puts(
        "PASS scene demand: prepare once before discovery, matched-target-only writes, scene-only test, left/OFF/right, retained "
        "pair and closed idle gates at 15/60 fps. Mock engine only.");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL scene demand: %s\n", e.what());
    return 1;
  }
}
