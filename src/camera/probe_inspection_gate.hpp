#pragma once

namespace taxi_camera::native_camera {

// OFF must also pause queued creation/recovery when no entries exist. Existing
// entries still need the ordinary validated path to close gates or clean up.
inline bool park_initial_scene(bool suspended, bool has_owned_entries) noexcept {
  return suspended && !has_owned_entries;
}

struct ProbeInspectionState {
  bool established_pair = false;
  bool suspended = false;
  bool gates_closed = false;
  bool pair_ready = false;
  bool lifecycle_pending = false;
  bool start_pending = false;
  bool warmup_pending = false;
  bool recovery_pending = false;
  bool mount_changed = false;
};

enum class ProbeInspectionDecision { periodic, idle, required };

// This gate stores no native address or memory proof. An idle observer performs
// no native operation. Leaving idle requires a fresh complete inspection before
// the existing caller may use any manager, view or camera pointer again.
class ProbeInspectionGate {
 public:
  ProbeInspectionDecision decide(const ProbeInspectionState& state) noexcept {
    const bool idle = state.established_pair && state.suspended && state.gates_closed && state.pair_ready && !state.lifecycle_pending &&
                      !state.start_pending && !state.warmup_pending && !state.recovery_pending && !state.mount_changed;
    if (idle) {
      idled_ = true;
      return ProbeInspectionDecision::idle;
    }
    const bool required = idled_ || state.mount_changed;
    idled_ = false;
    return required ? ProbeInspectionDecision::required : ProbeInspectionDecision::periodic;
  }

 private:
  bool idled_ = false;
};

}  // namespace taxi_camera::native_camera