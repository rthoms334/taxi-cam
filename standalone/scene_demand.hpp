#pragma once

namespace taxi_camera::standalone {
// Display demand is independent of native ownership. OFF parks the existing
// pair; aircraft/profile changes use a separate retained-pair transition.
struct SceneDemand {
  bool start;
  bool suspend;
  unsigned stamp_mask;
};
// accepted_mask has already passed heartbeat, identity, enabled and cutoff
// checks. Target discovery gates writes, never preparation of requested views.
inline SceneDemand scene_demand(unsigned accepted_mask,
                                bool test_scene,
                                unsigned assigned_mask,
                                bool scene_requested,
                                bool failed) noexcept {
  accepted_mask &= 3;
  const bool wanted = accepted_mask || test_scene;
  return {wanted && !scene_requested && !failed, !wanted || failed, failed ? 0 : accepted_mask & assigned_mask & 3};
}
}  // namespace taxi_camera::standalone
