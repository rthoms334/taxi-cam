#pragma once

#include "aircraft_mounts.hpp"
#include "entry_pair.hpp"
#include "scene_recovery.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace taxi_camera::native_camera {

enum class ProbeStage : std::size_t {
  manager,
  pool,
  lifecycle,
  entries,
  first_view,
  second_view,
  handoff,
  pose,
  activation,
  publication,
  count
};
inline constexpr std::array<const char*, static_cast<std::size_t>(ProbeStage::count)> kProbeStageNames{
    "manager", "pool", "lifecycle", "entries", "view 1", "view 2", "handoff", "pose", "activation", "publication"};

struct ProbePerformance {
  // Last serviced callback only. Stages are disjoint; lifecycle includes any
  // creation, cleanup and resize work. Unclassified overhead remains in total.
  std::array<double, static_cast<std::size_t>(ProbeStage::count)> stage_ms{};
  std::uint64_t query_calls = 0;
  std::uint64_t read_calls = 0;
  std::uint64_t requested_bytes = 0;
  std::uint64_t query_cache_hits = 0;
  std::uint64_t query_cache_validation_failures = 0;
  double query_ms = 0;
  double read_ms = 0;
  std::uint32_t entry_count = 0;
  std::uint32_t bucket_count = 0;
};

struct ProbeSnapshot {
  bool hook_installed = false;
  bool accepting_requests = false;
  bool pose_captured = false;
  std::uint64_t profile_transition_token = 0;
  std::uint32_t profile_transition_id = 0;
  bool profile_transition_pending = false;
  bool profile_transition_ready = false;
  bool profile_transition_failed = false;
  MountPair mounts = default_mounts();
  std::array<MountedPose, 2> mounted_poses{};
  std::uint64_t updates = 0;
  // Serviced callbacks only: skipped/throttled callbacks are not timed. Includes
  // diagnostic reads and private calls, but not the original update or GPU work.
  std::uint64_t inspection_count = 0;
  double observer_last_ms = 0;
  double observer_max_ms = 0;
  ProbePerformance performance;
  // Cumulative nonzero IDs returned by creation, including later rollbacks.
  std::uint64_t created_total = 0;
  std::uint32_t requested_rate = 15;
  std::uint32_t requested_feeds = 2;
  // Last native activation requests, not a measured rendered-frame rate.
  std::array<bool, 2> gates{};
  std::array<std::uint64_t, 2> activation_counts{};
  std::uint32_t thread_id = 0;
  std::uint32_t free_views = 0;
  std::array<bool, 2> ready{};
  std::array<const char*, 2> inspection_status{"not_inspected", "not_inspected"};
  std::array<bool, 2> resource_present{};
  bool outputs_matched = false;
  std::array<std::array<std::array<std::int32_t, 2>, 3>, 2> dimensions{};
  std::array<std::array<std::uint64_t, 2>, 2> flags{};
  engine_camera::Snapshot pair{};
  bool pose_waiting = false;
  bool view_waiting = false;
  std::uint64_t view_wait_count = 0;
  bool recovery_pending = false;
  // A bounded retry has been accepted and is waiting for pose/creation.
  bool restart_pending = false;
  unsigned recovery_attempts = 0;
  std::uint64_t stop_sequence = 0;
  SceneStopReason stop_reason = SceneStopReason::none;
  std::string stop_detail;
  std::string message = "Native scene test has not been started.";
};

// Called by the UI, outside DllMain. Installs the observer only after verifying
// the fixed main executable and pins this add-on until process exit. Engine
// functions are never called here: requests are consumed by the update observer.
void request_scene_test(bool reuse_calibration = false) noexcept;
// Each request gets a unique completion token. Zero refuses the request.
// Existing views are closed/revalidated by the observer and are never replaced.
std::uint64_t request_scene_profile_transition(std::uint32_t id) noexcept;
// Button mode retains public telemetry so another button press can restart.
void request_scene_stop(bool keep_telemetry = false) noexcept;
void note_scene_capture_progress(std::uint64_t now_ms) noexcept;
// Atomic configuration only; consumed by the observer, never calls the engine.
// Limits activation opportunities to 15..60 per second per selected feed.
// Close activation gates while retaining owned views; no ownership changes.
void suspend_scene_rendering(bool suspended) noexcept;
void request_scene_rate(unsigned rate, unsigned feeds = 2) noexcept;
// Validated configuration mailbox only. The observer applies separate mounts
// with a fresh verified aircraft pose before their next activation.
bool request_scene_profile(std::uint32_t id) noexcept;
bool request_scene_mounts(const MountPair& mounts) noexcept;
ProbeSnapshot scene_snapshot();

}  // namespace taxi_camera::native_camera
