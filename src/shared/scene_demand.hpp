#pragma once
#include <cmath>
#include <cstdint>

namespace taxi_camera::standalone {
// Display demand is independent of native ownership. OFF parks the existing
// pair; aircraft/airport changes use a separate full-session reset.
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
// moving, loading or stale sessions cannot perform background rendering.
struct ScenePrewarmReadiness {
  bool connected{}, session_settings{}, enabled{}, matching_identity{}, graphics_ready{};
  bool cutoff{}, buttons_valid{}, pose_ready{}, on_ground_valid{}, on_ground{};
  bool speed_valid{}, diagnostics_active{};
  double speed_knots{};
  bool session_ready{};
  bool eligible() const noexcept {
    return session_ready && connected && session_settings && enabled && matching_identity && graphics_ready && !cutoff && buttons_valid &&
           pose_ready && on_ground_valid && on_ground && speed_valid && std::isfinite(speed_knots) && speed_knots >= 0 &&
           speed_knots <= 0.5 && !diagnostics_active;
  }
};
struct ScenePrewarmProgress {
  bool views_ready{}, output{};
  std::uint64_t submitted_pairs{}, completed_pairs{};
};
class ScenePrewarm {
 public:
  enum class Phase { waiting, preparing, warming, paused, ready, timed_out, failed, foreground };
  // Preparation/calibration can outlast aircraft loading. Give rendering its
  // own small budget once both native views exist, with a separate overall cap.
  // Neither limit delays a foreground request or extends stale-data validity.
  static constexpr std::uint64_t MaximumStartupMs = 120000;
  static constexpr std::uint64_t MaximumWarmupMs = 5000;
  static constexpr std::uint64_t RequiredPairs = 3;
  bool observe(std::uint64_t now, bool eligible, bool foreground, ScenePrewarmProgress progress, bool failed) noexcept {
    if (foreground) {
      if (pending())
        phase_ = Phase::foreground;
      return false;
    }
    if (!pending())
      return false;
    if (phase_ == Phase::waiting) {
      if (!eligible || failed)
        return false;
      started_ms_ = now;
      baseline_pairs_ = progress.submitted_pairs;
      phase_ = Phase::preparing;
    }
    if (failed)
      phase_ = Phase::failed;
    else if (now < started_ms_ || now - started_ms_ >= MaximumStartupMs)
      phase_ = Phase::timed_out;
    else if (!eligible)
      phase_ = Phase::paused;
    else {
      // A transient loading/telemetry gap closes the gates, then resumes the
      // same request and retained pair. It never starts a new allocation cycle.
      phase_ = render_started_ ? Phase::warming : Phase::preparing;
      if (progress.views_ready && !render_started_) {
        render_started_ = true;
        render_started_ms_ = now;
        phase_ = Phase::warming;
      }
      if (progress.views_ready && progress.output && progress.completed_pairs >= baseline_pairs_ &&
          progress.completed_pairs - baseline_pairs_ >= RequiredPairs)
        phase_ = Phase::ready;
      else if (render_started_ && (now < render_started_ms_ || now - render_started_ms_ >= MaximumWarmupMs))
        phase_ = Phase::timed_out;
    }
    return active();
  }
  void finish_start(bool accepted) noexcept {
    if (!accepted && active())
      phase_ = Phase::failed;
  }
  bool active() const noexcept { return phase_ == Phase::preparing || phase_ == Phase::warming; }
  bool pending() const noexcept { return phase_ == Phase::waiting || phase_ == Phase::paused || active(); }
  Phase phase() const noexcept { return phase_; }
  std::uint64_t started_ms() const noexcept { return started_ms_; }
  std::uint64_t baseline_pairs() const noexcept { return baseline_pairs_; }
  const char* name() const noexcept {
    switch (phase_) {
      case Phase::waiting:
        return "waiting";
      case Phase::preparing:
        return "preparing";
      case Phase::warming:
        return "warming";
      case Phase::paused:
        return "paused_parked";
      case Phase::ready:
        return "ready_parked";
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
  std::uint64_t started_ms_{}, render_started_ms_{}, baseline_pairs_{};
  bool render_started_ = false;
};
// Only an explicit foreground start can latch the foreground failure state.
// Background preparation/refusal consumes its one attempt without requiring
// the user to turn a subsequent TAXI request off and back on.
inline bool finish_scene_start(ScenePrewarm& prewarm, bool background, bool accepted) noexcept {
  if (background)
    prewarm.finish_start(accepted);
  return !background && !accepted;
}
// Contract resolution can outlast the 500 ms telemetry window. That refusal is
// not a failed camera. Keep the same background attempt for every profile,
// including FlyByWire A380, both iniBuilds A350s, and the manual-only iniBuilds
// A380. A hard refusal still consumes the one attempt.
inline bool retain_background_prewarm(bool background, bool accepted, bool readiness_deferred) noexcept {
  return background && !accepted && readiness_deferred;
}
}  // namespace taxi_camera::standalone
