#include "../../src/app/simulator_exit.hpp"
#include <cstdio>
#include <map>
#include <stdexcept>

namespace {
unsigned checks{};
void require(bool ok, const char* message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}
}  // namespace

int main() {
  using namespace taxi_camera::standalone;
  try {
    std::map<std::uint32_t, ProcessRecord> table{
        {1, {1, 0, 10, true}},        // launcher
        {100, {100, 1, 20, true}},    // simulator
        {200, {200, 100, 30, true}},  // companion started by exe.xml
        {300, {300, 200, 40, true}},  // grandchild
        {400, {400, 1, 50, true}},    // companion started by the user
        {500, {500, 100, 5, true}},   // parent PID reused: "child" older than parent
        {600, {600, 999, 60, true}},  // parent gone
        {700, {700, 100, 0, true}},   // unknown creation stamp
    };
    const auto lookup = [&](std::uint32_t pid) {
      const auto found = table.find(pid);
      return found == table.end() ? ProcessRecord{} : found->second;
    };
    require(descends_from(200, 100, lookup), "Direct child of the simulator was not recognised");
    require(descends_from(300, 100, lookup), "Grandchild of the simulator was not recognised");
    require(!descends_from(400, 100, lookup), "A Start menu launch was treated as a simulator child");
    require(!descends_from(500, 100, lookup), "A reused parent PID was accepted");
    require(!descends_from(600, 100, lookup), "An exited parent was followed");
    require(!descends_from(700, 100, lookup), "An unknown creation stamp was accepted");
    require(!descends_from(100, 100, lookup) && !descends_from(0, 100, lookup) && !descends_from(200, 0, lookup),
            "Degenerate pids matched");
    table[1].parent_pid = 1;  // A cycle must terminate.
    table[100].parent_pid = 1;
    table[1].created = 25;
    require(!descends_from(400, 7777, lookup), "A cycle or missing ancestor matched");

    require(exit_after_simulator(true, true), "Preference on and simulator child must exit");
    require(!exit_after_simulator(false, true), "Preference off must keep the companion");
    require(!exit_after_simulator(true, false), "A manually started companion must keep running");
    require(!may_exit_now(true, true), "Unsaved edits in a visible window were discarded");
    require(may_exit_now(true, false) && may_exit_now(false, true), "Clean or hidden window blocked exit");

    SimulatorExitGate gate;
    require(!gate.pending() && !gate.due(100000), "Idle gate fired");
    gate.simulator_closed(1000);
    require(gate.pending() && !gate.due(1000 + SimulatorExitGate::GraceMs - 1), "Gate fired before the grace elapsed");
    require(gate.due(1000 + SimulatorExitGate::GraceMs) && !gate.pending(), "Gate did not fire after the grace");
    require(!gate.due(999999), "Gate fired twice");
    gate.simulator_closed(5000);
    gate.simulator_attached();
    require(!gate.due(50000), "A new simulator did not cancel the exit");
    gate.simulator_closed(0);
    require(gate.pending() && gate.due(SimulatorExitGate::GraceMs + 1), "Zero tick lost the pending exit");
    gate.simulator_closed(9000);
    require(!gate.due(8000), "A backward clock fired the gate");
    std::printf("simulator exit checks passed: %u\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAILED: %s\n", error.what());
    return 1;
  }
}
