#include "../../src/camera/probe_inspection_gate.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace taxi_camera::native_camera;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
ProbeInspectionState idle_state() {
  ProbeInspectionState state;
  state.established_pair = state.suspended = state.gates_closed = state.pair_ready = true;
  return state;
}
void admission() {
  const auto idle = idle_state();
  ProbeInspectionGate gate;
  for (unsigned frame = 0; frame < 10000; ++frame)
    require(gate.decide(idle) == ProbeInspectionDecision::idle, "Closed suspended observers resumed periodic reads");
  // Each missing prerequisite or pending task independently defeats idle.
  for (unsigned field = 0; field < 9; ++field) {
    ProbeInspectionGate candidate;
    auto state = idle;
    const std::array<bool*, 9> fields{&state.established_pair, &state.suspended,         &state.gates_closed,
                                      &state.pair_ready,       &state.lifecycle_pending, &state.start_pending,
                                      &state.warmup_pending,   &state.recovery_pending,  &state.mount_changed};
    *fields[field] = !*fields[field];
    require(candidate.decide(state) != ProbeInspectionDecision::idle, "A missing prerequisite or pending task was ignored");
    require(candidate.decide(idle) == ProbeInspectionDecision::idle, "A completed task permanently prevented idle");
    require(candidate.decide(state) == ProbeInspectionDecision::required, "Leaving idle did not force a fresh inspection");
  }
  auto mount = idle;
  mount.mount_changed = true;
  ProbeInspectionGate active;
  require(active.decide(mount) == ProbeInspectionDecision::required, "A mount revision waited for the periodic throttle");
}
void transitions() {
  ProbeInspectionGate gate;
  auto state = idle_state();
  require(gate.decide(state) == ProbeInspectionDecision::idle, "Established closed pair did not idle");
  state.suspended = false;
  require(gate.decide(state) == ProbeInspectionDecision::required, "Resume did not bypass the periodic throttle");
  require(gate.decide(state) == ProbeInspectionDecision::periodic, "Resume permanently disabled normal scheduling");
  state.suspended = true;
  require(gate.decide(state) == ProbeInspectionDecision::idle, "Resuspended pair did not idle");
  state.mount_changed = true;
  require(gate.decide(state) == ProbeInspectionDecision::required, "An idle mount revision was throttled");
  state.mount_changed = false;
  require(gate.decide(state) == ProbeInspectionDecision::idle, "Completed mount work prevented idle");
  for (bool* pending : {&state.lifecycle_pending, &state.start_pending, &state.warmup_pending, &state.recovery_pending}) {
    *pending = true;
    require(gate.decide(state) == ProbeInspectionDecision::required, "New pending work did not wake idle immediately");
    require(gate.decide(state) == ProbeInspectionDecision::periodic, "Pending work incorrectly entered idle on the next observer");
    *pending = false;
    require(gate.decide(state) == ProbeInspectionDecision::idle, "Completed pending work did not permit idle");
  }
  state.gates_closed = false;
  require(gate.decide(state) == ProbeInspectionDecision::required, "An open gate was treated as idle");
  require(gate.decide(state) == ProbeInspectionDecision::periodic, "An open gate became idle before closure");
  state.gates_closed = true;
  require(gate.decide(state) == ProbeInspectionDecision::idle, "Closed gate did not permit idle");
  state.pair_ready = false;
  require(gate.decide(state) == ProbeInspectionDecision::required, "Readiness loss did not leave idle");
  require(gate.decide(state) == ProbeInspectionDecision::periodic, "An unavailable pair froze its inspection scheduling");
  state.pair_ready = true;
  require(gate.decide(state) == ProbeInspectionDecision::idle, "Recovered pair did not permit idle");
}
}  // namespace
int main() {
  try {
    admission();
    transitions();
    std::printf("{\"passed\":true,\"checks\":%u,\"idle_frames\":10000}\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}