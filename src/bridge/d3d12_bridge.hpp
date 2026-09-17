#pragma once
#include <cstddef>
#include <vector>
#include "../graphics/pfd_target_detector.hpp"
#include "../graphics/scene_runtime.hpp"
namespace taxi_camera::standalone {
struct GraphicsStatus {
  bool ready{};
  // draws counts fully observed recordings; idle retains only per-resource
  // activity required by the complete PFD detector inventory.
  std::uint64_t device{}, resources{}, lists{}, draws{}, hook_failures{}, clear_states{};
  const char* error = "not_started";
  std::uint64_t selected_draws{}, selected_rt_metadata{}, selected_rt_callbacks{}, selected_pending_matches{};
  std::uint64_t selected_view_resolved{}, selected_view_rejected{}, copy_attempts{}, copy_rejected{};
  const char* copy_error = "not_attempted";
  std::array<std::uint64_t, 32> selected_exit_scopes{};
  std::uint64_t calibration_clears{};
  std::uint64_t selected_exit_base{}, selected_exit_nonbase{}, selected_exit_split{};
  std::uint64_t fallback_attempts{}, fallback_stamps{}, fallback_query_refused{}, fallback_state_refused{};
  std::uint64_t preferred_copy_attempts{}, preferred_copy_stamps{}, preferred_copy_no_proof{};
  const char* preferred_copy_reason = "not_attempted";
  std::uint64_t dynamic_depth_bias_calls{}, dynamic_strip_cut_calls{}, sample_position_calls{};
  std::uint64_t recording_end_draws{}, shader_deferred{}, close_forward_refused{};
  const char* target_detection = "warming_up";
  bool observing{};
  std::uint64_t observation_epoch{}, observation_invalidations{};
  // Opt-in hot-path diagnostics: misses equal actual registry acquisitions.
  std::uint64_t list_lookup_calls{}, list_cache_hits{}, list_registry_lookups{};
  std::uint64_t idle_state_bypasses{}, idle_callback_bypasses{};
};
bool initialize_graphics() noexcept;
// Resolve a reported device's optional COM proxy chain before native hooks or
// owned GPU work. Isolated validation supplies a hardware/WARP device; this
// never calls the simulator or telemetry.
bool initialize_graphics(IUnknown*) noexcept;
bool graphics_ready() noexcept;
// Include bounded warmup, rendering and calibration demand. Idle keeps native
// resource/descriptor lifetime and display activity discovery, plus retirement
// of previously recorded/submitted work. Source models stay continuously
// observed. PFD state omitted while idle needs real successful Reset to resume.
void set_graphics_observation_demand(bool enabled) noexcept;
void set_graphics_diagnostics_enabled(bool enabled) noexcept;
GraphicsStatus graphics_status() noexcept;
std::vector<PfdTargetObservation> pfd_inventory();
// Control-thread only. Turns off late-attach barrier/copy/OM extras after a
// healthy inventory or a short timeout. Not called from recording hooks.
void service_live_backfill(std::uint64_t now, std::size_t inventory_count) noexcept;
bool assign_targets(std::uint64_t left, std::uint64_t right) noexcept;
void set_target_mask(unsigned mask) noexcept;
void set_calibration(unsigned mask, unsigned budget) noexcept;
std::array<std::uint64_t, 2> target_ids() noexcept;
void set_aircraft_profile(std::uint32_t id) noexcept;
void discover_pfds(std::uint64_t now) noexcept;
}  // namespace taxi_camera::standalone
