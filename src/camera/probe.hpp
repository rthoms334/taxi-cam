#pragma once

#include "../shared/camera_rate.hpp"
#include "aircraft_mounts.hpp"
#include "entry_pair.hpp"
#include "node_link.hpp"
#include "pose_trace.hpp"
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
  aa,
  count
};
inline constexpr std::array<const char*, static_cast<std::size_t>(ProbeStage::count)> kProbeStageNames{
    "manager", "pool", "lifecycle", "entries", "view 1", "view 2", "handoff", "pose", "activation", "publication", "AA"};

struct ProbePerformance {
  // Last serviced callback only. Stages are disjoint; lifecycle includes any
  // creation, cleanup and resize work. AA measures recurring pulse preparation;
  // initial AA setup remains in lifecycle. Unclassified overhead remains in total.
  std::array<double, static_cast<std::size_t>(ProbeStage::count)> stage_ms{};
  // Kernel memory calls inside each stage: ReadProcessMemory reads, and region
  // or working-set queries (query_calls below counts both kinds too).
  std::array<std::uint32_t, static_cast<std::size_t>(ProbeStage::count)> stage_reads{};
  std::array<std::uint32_t, static_cast<std::size_t>(ProbeStage::count)> stage_queries{};
  std::uint64_t query_calls = 0;
  std::uint64_t query_allocation_calls = 0;
  std::uint64_t query_page_calls = 0;
  std::uint64_t query_fallback_calls = 0;
  std::uint64_t read_calls = 0;
  std::uint64_t requested_bytes = 0;
  std::uint64_t query_cache_hits = 0;
  std::uint64_t query_cache_validation_failures = 0;
  double query_ms = 0;
  double read_ms = 0;
  std::uint32_t entry_count = 0;
  std::uint32_t bucket_count = 0;
};

// Slowest camera-manager update (simulator main thread) since the previous
// take_observer_peak(). pre_ms runs from entry to the serviced start (or to
// the return when throttled or idle); stages cover only that serviced update.
struct ObserverPeak {
  double total_ms = 0;
  double pre_ms = 0;
  bool serviced = false;
  ProbePerformance performance;
};

struct ProbeSnapshot {
  bool hook_installed = false;
  bool accepting_requests = false;
  // The start was not queued because public flight readiness moved. The caller
  // may retry. A contract, hook, or other hard refusal leaves this false.
  bool readiness_deferred = false;
  bool pose_captured = false;
  std::uint64_t profile_transition_token = 0;
  std::uint32_t profile_transition_id = 0;
  bool profile_transition_pending = false;
  bool profile_transition_ready = false;
  bool profile_transition_failed = false;
  MountPair mounts = default_mounts();
  std::array<MountedPose, kMaxCameraFeeds> mounted_poses{};
  std::uint64_t updates = 0;
  // Serviced callbacks only: skipped/throttled callbacks are not timed. Includes
  // diagnostic reads and private calls, but not the original update or GPU work.
  std::uint64_t inspection_count = 0;
  double observer_last_ms = 0;
  double observer_max_ms = 0;
  ProbePerformance performance;
  // Cumulative nonzero IDs returned by creation, including later rollbacks.
  std::uint64_t created_total = 0;
  std::uint32_t requested_rate = kDefaultCameraRate;
  std::uint32_t requested_feeds = 2;
  bool requested_nose_priority = false;
  // Last native activation requests, not a measured rendered-frame rate.
  std::array<bool, kMaxCameraFeeds> gates{};
  std::array<std::uint64_t, kMaxCameraFeeds> activation_counts{};
  // Camera far distance (view_clip): near plane, culling far and default far
  // in metres, per feed as last applied and for the main view as last matched
  // (zeros until known). writes counts changed cameras; error is the last
  // refusal ("" when none).
  std::array<std::array<float, 3>, kMaxCameraFeeds> draw_clip{};
  std::array<float, 3> main_clip{};
  bool follow_main_far = false;
  // PoseLead at the last pose read: smoothed model speed (m/s), its movement
  // during the frame being drawn and the lead applied to the mounts (metres).
  double pose_speed = 0, pose_step_m = 0, pose_lead_m = 0;
  // After-update placement on the synced transform: feeds placed, refusals and
  // the last refusal reason ("" when none).
  std::uint64_t post_applied = 0, post_refused = 0;
  const char* post_error = "";
  // Camera mount on the aircraft Node (node_mount.hpp): whether this image's
  // parent contract resolved (and why not), per-feed state (0 world placement,
  // 1 attached, 2 lost), attach/restore/refusal counts and the last refusal.
  bool mount_available = false;
  std::string mount_contract_error;
  std::array<std::uint8_t, kMaxCameraFeeds> mount_state{};
  std::uint64_t mount_attaches = 0, mount_restores = 0, mount_refused = 0;
  const char* mount_error = "";
  // Diagnostics: main view, aircraft object camera and own nose camera Node
  // parent links (node_link.hpp), from the last calibration latch.
  std::array<NodeLinkReport, 3> node_links{};
  // This flight session's aircraft passed its public pose match; its scene
  // transform places the cameras without the local calibration from then on.
  bool pose_session_proven = false;
  std::uint64_t draw_clip_writes = 0;
  const char* draw_clip_error = "";
  std::uint32_t thread_id = 0;
  std::uint32_t free_views = 0;
  std::uint64_t retirement_deferrals = 0;
  bool retirement_waiting = false;
  const char* retirement_status = "not_inspected";
  std::array<std::uint32_t, 2> retirement_queue_counts{};
  // Pooled views whose bit31 this bridge cleared and later restored (writes
  // performed), refused restores, and whether any such view is still pending.
  std::uint64_t aa_restores = 0;
  std::uint64_t aa_restore_failures = 0;
  bool aa_cleared_pending = false;
  // Consecutive pulses and cumulative holds where the diffuse texture had no
  // render-target record (inspection=rt_record_absent).
  unsigned rt_record_refusals = 0;
  std::uint64_t rt_record_holds = 0;
  std::array<bool, kMaxCameraFeeds> ready{};
  std::array<const char*, kMaxCameraFeeds> inspection_status{"not_inspected", "not_inspected", "not_inspected"};
  std::array<bool, kMaxCameraFeeds> resource_present{};
  // Output admission per view: mode2, resource present, all size pairs and the
  // Bitmap at the requested pane. Gates open only for views with this true.
  std::array<bool, kMaxCameraFeeds> output_ready{};
  // Observer updates spent waiting for the initial pane output after resizing.
  unsigned output_waits = 0;
  // Per view, three hex digits (diffuse, add-diffuse, depth-stencil): 1 Bitmap,
  // 2 texture, 4 render-target record, as the captured replay resolves them.
  std::array<std::uint32_t, kMaxCameraFeeds> output_slots{};
  bool outputs_matched = false;
  std::array<std::array<std::array<std::int32_t, 2>, 3>, kMaxCameraFeeds> dimensions{};
  std::array<std::array<std::uint64_t, 2>, kMaxCameraFeeds> flags{};
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
// Full flight-session reset. Invalidates starts immediately; the observer
// closes and retires exact owned IDs using existing lifetime guards. Reports
// through profile_transition_*; ready requires confirmed absence and public
// load readiness. Saved mounts/calibration settings are not changed.
std::uint64_t request_scene_session_reset(std::uint32_t id) noexcept;
// Button mode retains public telemetry so another button press can restart.
void request_scene_stop(bool keep_telemetry = false) noexcept;
void note_scene_capture_progress(std::uint64_t now_ms) noexcept;
// Atomic configuration only; consumed by the observer, never calls the engine.
// Limits activation opportunities to 1..60 per second per selected feed (the
// moving minimum of 5 applies to the saved rate; parked floors go lower).
// nose_priority skips every other turn of the non-nose feeds.
// Close activation gates while retaining owned views; no ownership changes.
void suspend_scene_rendering(bool suspended) noexcept;
void request_scene_rate(unsigned rate, unsigned feeds = 2, bool nose_priority = false) noexcept;
// Atomic only, consumed by the observer before each pose refresh. True gives
// Taxi Cam's views the main view's far distances (above 60 kt, so they keep
// drawing the ground after take-off); false restores each camera's own far.
void request_scene_main_far(bool follow) noexcept;
// Diagnostics: hands each completed PoseTrace window to sink once (bridge
// worker; no engine access). False when none is ready.
using PoseTraceSink = void (*)(unsigned window, const PoseTraceEntry* entries, std::size_t count, void* context);
bool take_pose_trace(PoseTraceSink sink, void* context) noexcept;
// Validated configuration mailbox only. The observer applies separate mounts
// with a fresh verified aircraft pose before their next activation.
bool request_scene_profile(std::uint32_t id) noexcept;
bool request_scene_mounts(const MountPair& mounts) noexcept;
ProbeSnapshot scene_snapshot();
// Diagnostics only; the worker logs one peak per status interval.
ObserverPeak take_observer_peak() noexcept;

}  // namespace taxi_camera::native_camera
