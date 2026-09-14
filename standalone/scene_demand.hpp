#pragma once

namespace taxi_camera::standalone {
// Display demand is independent of native ownership. OFF parks the existing
// pair; only an aircraft/profile lifecycle change may request its retirement.
struct SceneDemand {
  bool start;
  bool suspend;
};
inline SceneDemand scene_demand(bool display_requested, bool scene_requested, bool failed) noexcept {
  return {display_requested && !scene_requested && !failed, !display_requested || failed};
}
}  // namespace taxi_camera::standalone
