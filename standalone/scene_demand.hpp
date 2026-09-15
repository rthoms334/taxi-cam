#pragma once
#include <cmath>
#include <cstdint>

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
                                bool failed,
                                bool prewarm = false) noexcept {
  accepted_mask &= 3;
  const bool wanted = accepted_mask || test_scene || prewarm;
  return {wanted && !scene_requested && !failed, !wanted || failed, failed ? 0 : accepted_mask & assigned_mask & 3};
}
// Preconditions are independent of display assignment. Unsupported, airborne,
// moving, loading or stale sessions cannot spend the one background attempt.
struct ScenePrewarmReadiness {
  bool connected{}, session_settings{}, enabled{}, matching_identity{}, graphics_ready{};
  bool cutoff{}, buttons_valid{}, pose_ready{}, on_ground_valid{}, on_ground{};
  bool speed_valid{}, diagnostics_active{};
  double speed_knots{};
  bool eligible() const noexcept {
    return connected && session_settings && enabled && matching_identity && graphics_ready && !cutoff && buttons_valid && pose_ready &&
           on_ground_valid && on_ground && speed_valid && std::isfinite(speed_knots) && speed_knots >= 0 && speed_knots <= 0.5 &&
           !diagnostics_active;
  }
};
class ScenePrewarm {
 public:
  enum class Phase { waiting, warming, ready, cancelled, timed_out, failed, foreground };
  // An upper bound on background work, not a guessed aircraft-load delay.
  static constexpr std::uint64_t MaximumWarmupMs = 5000;
  bool observe(std::uint64_t now, bool eligible, bool foreground, bool pair_output, bool failed) noexcept {
    if (foreground) {
      if (phase_ == Phase::waiting || phase_ == Phase::warming)
        phase_ = Phase::foreground;
      return false;
    }
    if (phase_ == Phase::warming) {
      if (!eligible)
        phase_ = Phase::cancelled;
      else if (failed)
        phase_ = Phase::failed;
      else if (pair_output)
        phase_ = Phase::ready;
      else if (now < started_ms_ || now - started_ms_ >= MaximumWarmupMs)
        phase_ = Phase::timed_out;
    } else if (phase_ == Phase::waiting && eligible && !failed) {
      started_ms_ = now;
      phase_ = pair_output ? Phase::ready : Phase::warming;
    }
    return phase_ == Phase::warming;
  }
  void finish_start(bool accepted) noexcept {
    if (!accepted && phase_ == Phase::warming)
      phase_ = Phase::failed;
  }
  Phase phase() const noexcept { return phase_; }
  std::uint64_t started_ms() const noexcept { return started_ms_; }
  const char* name() const noexcept {
    switch (phase_) {
      case Phase::waiting:
        return "waiting";
      case Phase::warming:
        return "warming";
      case Phase::ready:
        return "ready_parked";
      case Phase::cancelled:
        return "cancelled_parked";
      case Phase::timed_out:
        return "budget_parked";
      case Phase::failed:
        return "failed_parked";
      case Phase::foreground:
        return "foreground";
    }
    return "unknown";
  }

 private:
  Phase phase_ = Phase::waiting;
  std::uint64_t started_ms_{};
};
// Only an explicit foreground start can latch the foreground failure state.
// Background preparation/refusal consumes its one attempt without requiring
// the user to turn a subsequent TAXI request off and back on.
inline bool finish_scene_start(ScenePrewarm& prewarm, bool background, bool accepted) noexcept {
  if (background)
    prewarm.finish_start(accepted);
  return !background && !accepted;
}
}  // namespace taxi_camera::standalone
