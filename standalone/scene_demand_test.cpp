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
      const auto demand = taxi_camera::standalone::scene_demand(wanted, requested, false);
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
  pair.request_disable();  // Explicit aircraft/profile teardown remains separate.
  pair.process_update(owner, callbacks);
  require(engine.erases == 2 && pair.snapshot().state == ec::State::disabled, "Explicit lifecycle cleanup was lost");
}
}  // namespace
int main() {
  try {
    button_sequence(15);
    button_sequence(60);
    const auto failed = taxi_camera::standalone::scene_demand(true, false, true);
    require(!failed.start && failed.suspend, "A failed start retried without permission");
    std::puts("PASS scene demand: left/OFF/right, repeated toggles, closed idle gates and retained pair at 15/60 fps. Mock engine only.");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL scene demand: %s\n", e.what());
    return 1;
  }
}
