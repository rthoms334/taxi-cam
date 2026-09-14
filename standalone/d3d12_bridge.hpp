#pragma once
#include <vector>
#include "../src/pfd_target_detector.hpp"
#include "../src/scene_runtime.hpp"
namespace taxi_camera::standalone {
struct GraphicsStatus {
  bool ready{};
  std::uint64_t device{}, resources{}, lists{}, draws{}, hook_failures{}, clear_states{};
  const char* error = "not_started";
  std::uint64_t selected_draws{}, selected_rt_metadata{}, selected_rt_callbacks{}, selected_pending_matches{};
  std::uint64_t selected_view_resolved{}, selected_view_rejected{}, copy_attempts{}, copy_rejected{};
  const char* copy_error = "not_attempted";
};
bool initialize_graphics() noexcept;
// Isolated validation supplies a real hardware/WARP device. This never calls
// the simulator or telemetry. Own objects created by core code must be bypassed.
bool initialize_graphics(ID3D12Device*) noexcept;
GraphicsStatus graphics_status() noexcept;
std::vector<PfdTargetObservation> pfd_inventory();
bool assign_targets(std::uint64_t left, std::uint64_t right) noexcept;
void set_target_mask(unsigned mask) noexcept;
void set_calibration(unsigned mask, unsigned budget) noexcept;
std::array<std::uint64_t, 2> target_ids() noexcept;
void set_aircraft_profile(std::uint32_t id) noexcept;
void discover_pfds(std::uint64_t now) noexcept;
}  // namespace taxi_camera::standalone
