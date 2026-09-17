#include "probe.hpp"

#include "../graphics/scene_handoff.hpp"
#include "../hooks/observer_hook.hpp"
#include "../shared/camera_rate.hpp"
#include "activation_mask.hpp"
#include "aircraft_inventory.hpp"
#include "aircraft_scene_pose.hpp"
#include "body_pose_provider.hpp"
#include "camera_contract.hpp"
#include "local_memory.hpp"
#include "manager_inspection.hpp"
#include "owned_entry_inventory.hpp"
#include "owned_view.hpp"
#include "probe_inspection_gate.hpp"
#include "render_schedule.hpp"
#include "retained_profile.hpp"
#include "source_view.hpp"
#include "view_aa.hpp"
#include "view_readiness_wait.hpp"
#include "view_resize.hpp"
#include "view_resize_recovery.hpp"
#include "view_retirement.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace taxi_camera::native_camera {
namespace {
namespace ec = engine_camera;

struct Runtime {
  std::mutex mutex;
  std::mutex start_mutex;
  ProbeSnapshot published;
  ec::PairController pair;
  std::atomic<bool> hooked{false};
  std::atomic<bool> enabled{false};
  std::atomic<bool> suspended{false};
  std::atomic<unsigned> requested_settings{kDefaultCameraRate | (2u << 8)};
  // Protected by mutex; never used directly by a native engine call.
  MountPair requested_mounts = default_mounts();
  const profiles::AircraftProfile* requested_profile = &profiles::A380;
  const profiles::AircraftProfile* aircraft_profile = &profiles::A380;
  RetainedProfileTransition profile_transition;
  RetainedProfileTransition::AllocationEvidence published_allocation{};
  std::uint64_t profile_transition_token = 0;
  std::uint64_t scene_session_epoch = 0;
  bool retained_restart_requested = false;
  profiles::CameraPanes allocation_panes = profiles::A380.camera_panes;
  std::uint64_t requested_mount_revision = 0;
  bool requested_start = false;
  std::uint64_t requested_start_revision = 0;
  SceneRecovery recovery;
  std::string stop_detail;
  bool protection_ready = false;
  std::atomic_flag observing = ATOMIC_FLAG_INIT;
  std::uintptr_t base = 0;
  discovery::Inventory image;
  CameraContract contract;
  // Observer-thread fields. No borrowed camera/output pointers cross into UI.
  std::uint64_t updates = 0;
  std::uint64_t inspection_count = 0;
  std::uint64_t created_total = 0;
  LARGE_INTEGER counter_frequency{};
  double observer_last_ms = 0;
  double observer_max_ms = 0;
  ProbePerformance performance;
  RenderSchedule schedule;
  ProbeInspectionGate inspection_gate;
  std::array<ec::EntryId, 2> scheduled_ids{};
  std::array<bool, 2> gates{};
  std::array<std::uint64_t, 2> activation_counts{};
  std::array<ec::EntryId, 2> resized_ids{};
  std::array<ViewDimensions, 2> resized_dimensions{};
  ViewResizeWarmup resize_warmup;
  ViewResizeRecovery resize_recovery;
  ViewReadinessWait view_wait;
  ViewRetirement retirement;
  ULONGLONG last_inspection = 0;
  std::uint64_t manager = 0;
  std::uint64_t control = 0;
  std::uint64_t owned_control = 0;
  std::uint64_t renderer = 0;
  ec::ManagerToken token{};
  unsigned creations = 0;
  bool lifecycle_touched = false;
  bool creation_valid = false;
  bool creation_pose_unavailable = false;
  bool pose_captured = false;
  bool pose_busy = false;
  bool inspection_changed = false;
  SceneStopReason inspection_stop = SceneStopReason::identity_refused;
  MountPair mounts = default_mounts();
  std::uint64_t mount_revision = 0;
  std::array<MountedPose, 2> mounted_poses{};
  const char* stage_error = "";
  std::string manager_memory_error;
  std::string inspection_error;
  std::string creation_error;
  std::string message;
};

// Intentionally process-lifetime storage: the installed observer pins this DLL.
// Neither callbacks nor their mutexes may be destroyed during runtime unloading.
Runtime& state() {
  static Runtime* const runtime = new Runtime;
  return *runtime;
}

class StageTimer {
 public:
  StageTimer(Runtime& runtime, ProbeStage stage) noexcept : runtime_(runtime), stage_(stage) { QueryPerformanceCounter(&started_); }
  ~StageTimer() {
    LARGE_INTEGER finished{};
    if (QueryPerformanceCounter(&finished) && started_.QuadPart > 0 && finished.QuadPart >= started_.QuadPart &&
        runtime_.counter_frequency.QuadPart > 0)
      runtime_.performance.stage_ms[static_cast<std::size_t>(stage_)] +=
          static_cast<double>(finished.QuadPart - started_.QuadPart) * 1000.0 / runtime_.counter_frequency.QuadPart;
  }

 private:
  Runtime& runtime_;
  ProbeStage stage_;
  LARGE_INTEGER started_{};
};

template <typename Operation>
decltype(auto) timed(Runtime& runtime, ProbeStage stage, Operation&& operation) {
  StageTimer timer(runtime, stage);
  return operation();
}

// Cache metadata only while a pure inspection builds an unpublished result.
// No private call occurs until the cache is detached and all endpoint queries
// agree. Each helper retains its original bounded reads and full trace reread.
template <typename Operation>
auto inspected(Runtime& runtime, Operation&& operation, std::string* failure_detail = nullptr) {
  ScopedLocalMemoryQueryCache queries(LocalMemoryQueryMode::private_pages);
  auto result = operation();
  if (!queries.finish()) {
    runtime.inspection_changed = true;
    runtime.inspection_error = "Memory-region metadata changed during inspection; no result was accepted: " +
                               describe_local_memory_query_failure(queries.failure());
    runtime.stage_error = runtime.inspection_error.c_str();
    if (failure_detail)
      *failure_detail = runtime.inspection_error;
    return decltype(result){};
  }
  return result;
}

template <typename Function>
Function function(Runtime& runtime, std::uint32_t rva) noexcept {
  return reinterpret_cast<Function>(runtime.base + rva);
}

template <typename T>
bool word(LocalMemoryReader& reader, std::uint64_t address, T& output) noexcept {
  return reader.read(address, &output, sizeof(output));
}

// Called only from this manager's update thunk, immediately before its original.
// Cached pointer, weak handle and vptr must all agree, then be reread.
// An enclosing transaction may borrow this pure stage. Its caller must finish
// the shared cache before using the provisional Runtime identity for any call.
bool manager_context(Runtime& runtime,
                     void* current_manager,
                     ScopedLocalMemoryQueryCache* shared = nullptr,
                     ManagerInspection* outcome = nullptr) {
  const auto refused = [&](ManagerInspection result) {
    if (outcome)
      *outcome = result;
    return false;
  };
  std::optional<ScopedLocalMemoryQueryCache> queries;
  if (shared) {
    if (!shared->is_current())
      return refused({ManagerInspectionStatus::unavailable, "Manager inspection transaction is no longer current."});
  } else {
    queries.emplace(LocalMemoryQueryMode::private_pages);
  }
  LocalMemoryReader reader(512);
  LocalImageReader image(reinterpret_cast<HMODULE>(runtime.base), runtime.image.image_size, LocalImageQueryMode::pages);
  const auto result = inspect_manager_identity(image, reader, runtime.base, reinterpret_cast<std::uintptr_t>(current_manager),
                                               runtime.owned_control, runtime.contract.layout);
  if (!result)
    return refused(result);
  if (queries && !queries->finish()) {
    runtime.manager_memory_error =
        "Manager memory-region metadata changed during inspection: " + describe_local_memory_query_failure(queries->failure());
    return refused({ManagerInspectionStatus::unavailable, runtime.manager_memory_error.c_str()});
  }
  runtime.manager = result.manager;
  runtime.renderer = result.renderer;
  runtime.control = result.control;
  runtime.token = {result.manager, result.generation};
  if (outcome)
    *outcome = result;
  return true;
}

bool capture_pose(Runtime& runtime) {
  runtime.pose_captured = false;
  runtime.pose_busy = false;
  runtime.mounted_poses = {};
  std::string memory_detail;
  auto body = sample_body_pose(GetTickCount64());
  if (!body.valid && temporary_pose_unavailable(body.error)) {
    runtime.pose_busy = true;
    runtime.message = std::string("Aircraft telemetry temporarily unavailable; render gates remain closed: ") + body.error;
    return false;
  }
  if (!body.valid && !body.calibration_required) {
    runtime.message = std::string("Aircraft body pose unavailable: ") + body.error;
    return false;
  }
  if (!body.valid) {
    // Only calibration reads the current view. Its ECEF position/FOV must match
    // a fresh public CameraGet WORLD sample. Aircraft position and orientation
    // subsequently come from independent public aircraft telemetry.
    LocalMemoryReader objects;
    LocalImageReader image(reinterpret_cast<HMODULE>(runtime.base), runtime.image.image_size, LocalImageQueryMode::pages);
    std::uint64_t source = 0;
    const auto aircraft = inspected(
        runtime,
        [&] {
          return discovery::inspect_aircraft_metadata(image, objects, runtime.image, runtime.base,
                                                      runtime.contract.layout.aircraft_facade_vtable, true, true, false, &source, nullptr,
                                                      runtime.contract.layout);
        },
        &memory_detail);
    if (!aircraft.valid || !aircraft.available || !source) {
      runtime.message = "Aircraft body calibration is waiting for a stable loaded source: " + aircraft.stage + ". " + aircraft.error;
      if (!memory_detail.empty())
        runtime.message += " " + memory_detail;
      return false;
    }
    objects.reset_budget();
    Vector3 position{};
    const auto camera = inspected(runtime, [&] { return inspect_source_pose(objects, source, &position); }, &memory_detail);
    if (!camera.complete || !calibrate_body_pose(position, camera.fov, GetTickCount64())) {
      runtime.message = "Aircraft body calibration is waiting for matching fresh public and private camera coordinates.";
      if (!memory_detail.empty())
        runtime.message += " " + memory_detail;
      return false;
    }
    body = sample_body_pose(GetTickCount64());
  }
  if (!body.valid) {
    runtime.pose_busy = temporary_pose_unavailable(body.error);
    runtime.message = std::string("Aircraft body pose unavailable: ") + body.error;
    return false;
  }
  // The visible aircraft and its camera offsets must use the same scene pose.
  // SimConnect is asynchronous; a fresh packet is still not the rendered model
  // transform during a taxi turn. Retain its session/freshness/plausibility
  // guards, but never use it as the mount transform or interpolate toward it.
  LocalMemoryReader objects;
  LocalImageReader image(reinterpret_cast<HMODULE>(runtime.base), runtime.image.image_size, LocalImageQueryMode::pages);
  std::uint64_t user = 0;
  const auto aircraft = inspected(
      runtime,
      [&] {
        return discovery::inspect_aircraft_metadata(image, objects, runtime.image, runtime.base,
                                                    runtime.contract.layout.aircraft_facade_vtable, true, true, false, nullptr, &user,
                                                    runtime.contract.layout);
      },
      &memory_detail);
  if (!aircraft.valid || !aircraft.available || !user) {
    runtime.pose_busy = true;
    runtime.message = "Aircraft scene mount is waiting for a stable active aircraft: " + aircraft.stage + ". " + aircraft.error;
    if (!memory_detail.empty())
      runtime.message += " " + memory_detail;
    return false;
  }
  objects.reset_budget();
  const auto scene =
      inspected(runtime, [&] { return inspect_aircraft_scene_pose(objects, user, runtime.base, runtime.contract.layout); }, &memory_detail);
  if (!scene.complete || !scene_body_matches_public(scene.pose, body.pose) ||
      !make_mounted_pair(scene.pose, runtime.mounts, runtime.mounted_poses)) {
    runtime.pose_busy = true;
    runtime.message = std::string("Aircraft scene mount is waiting for a consistent model pose: ") +
                      (scene.complete ? "public_pose_mismatch" : scene.error);
    if (!memory_detail.empty())
      runtime.message += " " + memory_detail;
    return false;
  }
  runtime.pose_captured = true;
  return true;
}
ec::OwnedViewSnapshot inspect_entry(Runtime& runtime, std::uint64_t id) {
  LocalMemoryReader reader;
  const auto entries = inspected(runtime, [&] { return ec::inspect_owned_entries(reader, runtime.manager, {id, 0}); });
  if (!entries.complete || !entries.entries[0].found) {
    ec::OwnedViewSnapshot failed;
    failed.error = entries.complete ? "The created entry ID is absent from the manager table." : entries.error;
    return failed;
  }
  reader.reset_budget();
  const auto pool = inspected(runtime, [&] { return ec::inspect_view_pool(reader, runtime.renderer); });
  reader.reset_budget();
  return inspected(runtime, [&] { return ec::inspect_owned_view(reader, entries.entries[0].address, id, pool); });
}

// Pool, table and both views share one read-only region-query scope. Every
// field/trace is still reread, and every queried region is revalidated before
// publishing pointers or calling the engine. Nothing survives this observer.
void inspect_pair(Runtime& runtime,
                  const std::array<ec::EntryId, 2>& ids,
                  ProbeSnapshot& report,
                  std::array<ec::OwnedViewSnapshot, 2>& views,
                  bool publish = true,
                  ScopedLocalMemoryQueryCache* shared = nullptr) {
  views = {};
  // Borrowed results remain provisional and may never publish a handoff here.
  if (shared && (publish || !shared->is_current()))
    return;
  runtime.inspection_stop = SceneStopReason::none;
  runtime.stage_error = "Owned view validation failed; removal requested.";
  const auto refuse = [&](SceneStopReason reason, const char* detail) {
    // One malformed identity must not be downgraded by the other view's
    // transient failure. Every new retry still repeats the full native guards.
    if (runtime.inspection_stop == SceneStopReason::none || reason == SceneStopReason::identity_refused) {
      runtime.inspection_stop = reason;
      runtime.stage_error = detail;
    }
  };
  const auto ticket = publish ? timed(runtime, ProbeStage::handoff, [] { return scene_handoff().begin_capture(); }) : SceneCaptureTicket{};
  std::array<std::uint64_t, 2> resources{};
  std::optional<ScopedLocalMemoryQueryCache> queries;
  if (!shared)
    queries.emplace(LocalMemoryQueryMode::private_pages);
  LocalMemoryReader reader;
  const auto pool = timed(runtime, ProbeStage::pool, [&] { return ec::inspect_view_pool(reader, runtime.renderer); });
  report.free_views = pool.valid ? pool.free_count : 0;
  if (!pool.valid) {
    refuse((runtime.inspection_changed && pool.status == ec::ViewPoolStatus::not_inspected) ||
                   pool.status == ec::ViewPoolStatus::read_failed || pool.status == ec::ViewPoolStatus::changed
               ? SceneStopReason::inspection_unavailable
               : SceneStopReason::identity_refused,
           "The current view pool could not be validated.");
    return;
  }
  reader.reset_budget();
  const auto entries = timed(runtime, ProbeStage::entries, [&] { return ec::inspect_owned_entries(reader, runtime.manager, ids); });
  runtime.performance.entry_count = entries.entry_count;
  runtime.performance.bucket_count = entries.bucket_count;
  if (!entries.complete) {
    refuse((runtime.inspection_changed && !entries.read_bytes) || entries.read_failures || std::strstr(entries.error, "changed")
               ? SceneStopReason::inspection_unavailable
               : SceneStopReason::identity_refused,
           entries.error);
    return;
  }
  for (unsigned i = 0; i < ids.size(); ++i) {
    if (!entries.entries[i].found) {
      refuse(SceneStopReason::owned_entry_absent, "An owned camera entry is absent from the verified manager table.");
      continue;
    }
    reader.reset_budget();
    const auto view = timed(runtime, i == 0 ? ProbeStage::first_view : ProbeStage::second_view,
                            [&] { return ec::inspect_owned_view(reader, entries.entries[i].address, ids[i], pool); });
    views[i] = view;
    report.inspection_status[i] = ec::owned_view_status_name(view.status);
    report.ready[i] = view.complete && view.ready;
    if (!report.ready[i]) {
      refuse((runtime.inspection_changed && view.status == ec::OwnedViewStatus::not_inspected) ||
                     view.status == ec::OwnedViewStatus::read_failed || view.status == ec::OwnedViewStatus::changed ||
                     view.status == ec::OwnedViewStatus::pool_changed || view.status == ec::OwnedViewStatus::pending
                 ? SceneStopReason::inspection_unavailable
                 : SceneStopReason::identity_refused,
             *view.error ? view.error : "The owned view is not ready.");
    }
    report.resource_present[i] = report.ready[i] && view.resource_present;
    if (report.ready[i]) {
      report.dimensions[i] = view.dimensions;
      report.flags[i] = view.flags;
      resources[i] = view.resource_address;
      // Primary-view resizing can overwrite mode2 size fields while its Bitmap
      // remains pane-sized. Retain the pair; recovery may restore fields only
      // when fresh output dimensions prove that no reallocation is needed.
      if (runtime.resized_ids[i] != ids[i] || runtime.resized_dimensions[i] != view.dimensions) {
        report.ready[i] = report.resource_present[i] = false;
        refuse(SceneStopReason::resolution_changed,
               "Owned-view dimensions changed; retaining the pair for closed-gate dimension recovery.");
      }
    }
  }
  if (queries && !queries->finish()) {
    runtime.inspection_changed = true;
    runtime.inspection_stop = SceneStopReason::inspection_unavailable;
    runtime.stage_error = "Memory-region metadata changed during pair inspection; no result was accepted.";
    views = {};
    report.ready = report.resource_present = {};
    report.dimensions = {};
    report.flags = {};
    report.inspection_status = {"not_inspected", "not_inspected"};
    return;
  }
  if (publish && report.ready[0] && report.ready[1])
    report.outputs_matched = timed(runtime, ProbeStage::handoff, [&] {
      return scene_handoff().publish(ticket, {runtime.token.identity, runtime.token.generation}, ids, resources);
    });
}

// Only a closing pulse reaches this path. It cannot supply an OwnedViewSnapshot
// to pose, resize or handoff code. Keep the complete table/pool and association
// guards, but do not touch Camera/material/output data that false activation
// neither reads nor writes. Each cache ends before the first private call.
bool close_owned_pair(Runtime& runtime, void* manager, const ec::Snapshot& pair, ProbeSnapshot& report) {
  std::array<ec::OwnedViewCloseSnapshot, 2> views{};
  {
    ScopedLocalMemoryQueryCache queries(LocalMemoryQueryMode::private_pages);
    if (!timed(runtime, ProbeStage::manager, [&] { return manager_context(runtime, manager, &queries); }) || runtime.token != pair.owner)
      return false;
    LocalMemoryReader reader;
    const auto pool = timed(runtime, ProbeStage::pool, [&] { return ec::inspect_view_pool(reader, runtime.renderer); });
    if (!pool.valid)
      return false;
    reader.reset_budget();
    const auto entries =
        timed(runtime, ProbeStage::entries, [&] { return ec::inspect_owned_entries(reader, runtime.manager, pair.owned_ids); });
    runtime.performance.entry_count = entries.entry_count;
    runtime.performance.bucket_count = entries.bucket_count;
    if (!entries.complete)
      return false;
    for (unsigned i = 0; i < views.size(); ++i) {
      if (!entries.entries[i].found)
        return false;
      reader.reset_budget();
      views[i] = timed(runtime, i == 0 ? ProbeStage::first_view : ProbeStage::second_view,
                       [&] { return ec::inspect_owned_view_for_close(reader, entries.entries[i].address, pair.owned_ids[i], pool); });
      if (!views[i].complete || views[i].dimensions != runtime.resized_dimensions[i])
        return false;
    }
    if (views[0].view_address == views[1].view_address || views[0].view_index == views[1].view_index || !queries.finish())
      return false;
  }
  const auto current = runtime.pair.snapshot();
  if (current.state != ec::State::active || current.owner != pair.owner || current.owned_ids != pair.owned_ids || current.request_pending ||
      current.creation_pending || current.failure != ec::Failure::none || current.blocked != ec::Blocked::none)
    return false;
  {
    const std::lock_guard lock(runtime.mutex);
    if (runtime.requested_start || runtime.recovery.pending() || runtime.published.view_waiting || runtime.published.pose_waiting ||
        runtime.mount_revision != runtime.requested_mount_revision)
      return false;
  }
  const bool closed = timed(runtime, ProbeStage::activation, [&] {
    // Captured return is AL: false means native lookup refused. No open call is
    // available here. Any refusal, including after a partial close, falls back
    // to the normal full inspection without trusting the cached gate booleans.
    const auto activate = function<bool (*)(void*, std::uint64_t, bool)>(runtime, runtime.contract.functions.activate_entry);
    for (unsigned i = 0; i < views.size(); ++i)
      if ((views[i].flags[0] & 1u) == 0 && !activate(reinterpret_cast<void*>(runtime.manager), pair.owned_ids[i], false))
        return false;
    ScopedLocalMemoryQueryCache queries(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader(64);
    for (const auto& view : views) {
      auto expected = view.flags;
      expected[0] |= 1u;
      std::array<std::uint64_t, 2> first{}, second{};
      if (!word(reader, view.view_address + 48, first) || first != expected || !word(reader, view.view_address + 48, second) ||
          second != first)
        return false;
    }
    return queries.finish();
  });
  if (!closed)
    return false;
  runtime.gates = {};
  // Retain the last FULL camera/output diagnostics; do not publish a new
  // resource ticket or imply that this close-only inspection refreshed them.
  {
    const std::lock_guard lock(runtime.mutex);
    report = runtime.published;
  }
  report.updates = runtime.updates;
  report.thread_id = GetCurrentThreadId();
  report.pair = pair;
  report.gates = {};
  for (unsigned i = 0; i < views.size(); ++i) {
    report.flags[i] = views[i].flags;
    report.flags[i][0] |= 1u;
    report.inspection_status[i] = "close_only; last full output diagnostics retained";
  }
  return true;
}
bool prepare_owned_view_aa(Runtime& runtime, ec::EntryId id, ec::OwnedViewSnapshot& view) {
  LocalImageReader image(reinterpret_cast<HMODULE>(runtime.base), runtime.image.image_size, LocalImageQueryMode::pages);
  const auto result = disable_owned_view_aa(view, image, runtime.contract.layout);
  if (!result.complete) {
    runtime.stage_error = result.error;
    return false;
  }
  if (result.write_attempted) {
    const auto confirmed = inspect_entry(runtime, id);
    auto expected_flags = view.flags;
    expected_flags[0] &= ~kViewAaFlag;
    if (!confirmed.complete || !confirmed.ready || confirmed.mode != 2 || confirmed.view_address != view.view_address ||
        confirmed.node_address != view.node_address || confirmed.camera_address != view.camera_address ||
        confirmed.resource_address != view.resource_address || confirmed.dimensions != view.dimensions ||
        confirmed.output_dimensions != view.output_dimensions || confirmed.flags != expected_flags) {
      runtime.stage_error = "Owned camera identity changed while disabling its AA; render gate stays closed.";
      return false;
    }
    view = confirmed;
  }
  return true;
}

void apply_pose(Runtime& runtime, const ec::OwnedViewSnapshot& view, const MountedPose& pose) noexcept {
  using SetVector = void (*)(void*, const double*);
  function<SetVector>(runtime, runtime.contract.functions.set_position)(reinterpret_cast<void*>(view.node_address), pose.position.data());
  function<SetVector>(runtime, runtime.contract.functions.set_up)(reinterpret_cast<void*>(view.camera_address), pose.up.data());
  function<SetVector>(runtime, runtime.contract.functions.set_target)(reinterpret_cast<void*>(view.camera_address), pose.target.data());
  function<void (*)(void*, float)>(runtime, runtime.contract.functions.set_fov)(reinterpret_cast<void*>(view.camera_address), pose.fov);
  function<void (*)(void*)>(runtime, runtime.contract.functions.update_view)(reinterpret_cast<void*>(view.view_address));
}

void apply_gates(Runtime& runtime,
                 const std::array<ec::EntryId, 2>& ids,
                 const std::array<bool, 2>& desired,
                 const ProbeSnapshot& inspected,
                 bool initialize_gates) {
  using Activate = void (*)(void*, std::uint64_t, bool);
  const auto activate = function<Activate>(runtime, runtime.contract.functions.activate_entry);
  // The captured false path ORs the separately verified {1,0} mask into P48/56.
  // Close gates before opening one. On a new pair, explicitly close both even
  // when setup already left bit0 set; no original update runs between calls.
  for (unsigned i = 0; i < ids.size(); ++i) {
    const bool observed_active = (inspected.flags[i][0] & 1u) == 0;
    if (initialize_gates || (!desired[i] && (runtime.gates[i] || observed_active)))
      activate(reinterpret_cast<void*>(runtime.manager), ids[i], false);
  }
  for (unsigned i = 0; i < ids.size(); ++i) {
    const bool observed_active = (inspected.flags[i][0] & 1u) == 0;
    if (desired[i] && (initialize_gates || !runtime.gates[i] || !observed_active)) {
      activate(reinterpret_cast<void*>(runtime.manager), ids[i], true);
      ++runtime.activation_counts[i];
    }
  }
  runtime.gates = desired;
}

// Runs only in the verified observer phase. Reuses the existing gate operation;
// profile changes never call the native erase, create, resize or pose setters.
void service_profile_transition(Runtime& runtime, void* manager, ProbeSnapshot& report) {
  RetainedProfileTransition transition;
  std::uint64_t token, start_revision;
  bool resume_requested;
  {
    const std::lock_guard lock(runtime.mutex);
    if (!runtime.profile_transition.holding()) {
      report.pair = runtime.pair.snapshot();
      return;
    }
    if (runtime.profile_transition.awaiting_pair()) {
      // Serialize with Start/profile requests while the controller separately
      // excludes in-flight creation. This only cancels unmaterialized mailbox
      // work; it cannot erase, create or forget an owned native camera.
      const auto cancelled = runtime.pair.cancel_uncreated_request();
      report.pair = runtime.pair.snapshot();
      if (cancelled == ec::EmptyPairCancel::busy) {
        report.message = "Waiting for the current camera operation before changing aircraft profile.";
        return;
      }
      runtime.profile_transition.begin_published(runtime.profile_transition.id(), report.pair, runtime.published_allocation);
      if (runtime.profile_transition.awaiting_pair()) {
        report.message = "Waiting for the current camera allocation evidence before changing aircraft profile.";
        return;
      }
      if (cancelled == ec::EmptyPairCancel::cancelled) {
        report.message = "Pending camera startup cleared; ready for the aircraft profile.";
        return;
      }
    }
    transition = runtime.profile_transition;
    token = runtime.profile_transition_token;
    resume_requested = runtime.retained_restart_requested;
    start_revision = runtime.requested_start_revision;
  }
  const auto pair = runtime.pair.snapshot();
  report.pair = pair;
  bool validated = false;
  if (!transition.failed() && timed(runtime, ProbeStage::manager, [&] { return manager_context(runtime, manager); })) {
    RetainedProfileTransition::Views views{};
    inspect_pair(runtime, pair.owned_ids, report, views, false);
    const auto action = transition.inspect(runtime.token, pair, views);
    if (action == RetainedProfileTransition::Decision::close) {
      const auto before_views = views;
      if (!manager_context(runtime, manager) || runtime.token != pair.owner)
        transition.refuse();
      else {
        apply_gates(runtime, pair.owned_ids, {}, report, true);
        inspect_pair(runtime, pair.owned_ids, report, views, false);
        bool unchanged = true;
        for (unsigned i = 0; i < 2; ++i)
          unchanged = unchanged && views[i].complete && views[i].ready && views[i].view_address == before_views[i].view_address &&
                      views[i].node_address == before_views[i].node_address && views[i].camera_address == before_views[i].camera_address &&
                      views[i].resource_address == before_views[i].resource_address && (views[i].flags[0] & 1u);
        if (unchanged)
          transition.inspect(runtime.token, runtime.pair.snapshot(), views);
        else
          transition.refuse();
      }
    }
    validated = transition.ready();
    if (validated)
      runtime.gates = {};
  }
  // A prior ready acknowledgement never authorizes resume after a failed fresh
  // manager inspection. Missing telemetry/calibration keeps this hold active.
  const auto resume_epoch = get_aircraft_session_epoch();
  const bool pose_ready = validated && resume_requested && aircraft_matches_profile() &&
                          timed(runtime, ProbeStage::pose, [&] { return capture_pose(runtime); });
  report.outputs_matched = false;
  report.pose_waiting = resume_requested && validated && !pose_ready;
  report.message = transition.failed() ? "Aircraft change paused: retained camera identity could not be validated. Restart MSFS to resume."
                   : !validated        ? "Waiting for complete retained camera views before changing aircraft profile."
                   : resume_requested && !pose_ready ? "Retained cameras are closed; waiting for fresh aircraft pose calibration."
                                                     : "Camera pair retained with gates closed; ready for the new aircraft profile.";
  const std::lock_guard lock(runtime.mutex);
  if (token == runtime.profile_transition_token) {
    runtime.profile_transition = transition;
    if (validated)
      runtime.aircraft_profile = runtime.requested_profile;
    if (pose_ready && runtime.retained_restart_requested && start_revision == runtime.requested_start_revision &&
        runtime.recovery.requested() && resume_epoch == get_aircraft_session_epoch()) {
      runtime.profile_transition.consume();
      runtime.retained_restart_requested = false;
      runtime.scene_session_epoch = resume_epoch;
      runtime.schedule.reset();
      scene_handoff().begin_scene();
    }
  }
}
bool initialize(void* opaque, ec::DescriptorStorage& descriptor) noexcept {
  auto& runtime = *static_cast<Runtime*>(opaque);
  runtime.lifecycle_touched = true;
  // Only a real creation request reaches this callback, after prior cleanup.
  // Stop requests cannot call the pose getters or descriptor initializer.
  if (runtime.creations == 0) {
    runtime.creation_pose_unavailable = false;
    runtime.allocation_panes = runtime.aircraft_profile->camera_panes;
    try {
      runtime.message.clear();
      runtime.stage_error = "";
      LocalMemoryReader reader;
      const auto pool = inspected(runtime, [&] { return ec::inspect_view_pool(reader, runtime.renderer); });
      runtime.creation_valid = pool.valid && pool.free_count >= 2;
      if (!runtime.creation_valid)
        runtime.stage_error = pool.valid ? "Two free engine views are required." : "The view pool could not be validated.";
      else if (!(runtime.creation_valid = capture_pose(runtime))) {
        runtime.creation_error =
            runtime.message.empty() ? "A verified aircraft body pose is required before creating mounted views." : runtime.message;
        runtime.stage_error = runtime.creation_error.c_str();
        runtime.creation_pose_unavailable = runtime.pose_busy;
      }
    } catch (...) {
      runtime.creation_valid = false;
    }
  }
  if (!runtime.creation_valid)
    return false;
  function<void (*)(void*)>(runtime, runtime.contract.functions.initialize_descriptor)(descriptor.bytes.data());
  return true;
}

ec::EntryId create(void* opaque, ec::ManagerToken token, const ec::DescriptorStorage& descriptor) noexcept {
  auto& runtime = *static_cast<Runtime*>(opaque);
  runtime.lifecycle_touched = true;
  if (runtime.creations >= runtime.resized_ids.size()) {
    runtime.creation_valid = false;
    runtime.stage_error = "The owned-view creation limit was reached.";
    return 0;
  }
  if (!runtime.creation_valid || !manager_context(runtime, reinterpret_cast<void*>(token.identity)) || token != runtime.token) {
    runtime.stage_error = "Manager identity changed before camera creation.";
    return 0;
  }
  LocalMemoryReader reader;
  const auto pool = inspected(runtime, [&] { return ec::inspect_view_pool(reader, runtime.renderer); });
  if (!pool.valid || pool.free_count < (runtime.creations == 0 ? 2u : 1u)) {
    runtime.creation_valid = false;
    runtime.stage_error = "Available view capacity changed before camera creation.";
    return 0;
  }
  const auto id = function<std::uint64_t (*)(void*, const void*)>(runtime, runtime.contract.functions.create_entry)(
      reinterpret_cast<void*>(token.identity), descriptor.bytes.data());
  if (!id) {
    runtime.stage_error = "The native creation call returned no owned ID.";
    return 0;
  }
  runtime.owned_control = runtime.control;
  ++runtime.creations;
  ++runtime.created_total;
  auto view = inspect_entry(runtime, id);
  runtime.creation_valid = view.complete && view.ready;
  if (!runtime.creation_valid)
    runtime.stage_error = view.complete ? "Entry setup was pending; rolling back the new pair."
                          : *view.error ? view.error
                                        : "The new entry failed owned-view validation.";
  // Always return a created nonzero ID, even if validation fails. The controller
  // must retain ownership so the next failed stage can remove it.
  if (runtime.creation_valid) {
    // Setup may already have queued its initial primary-size allocation. Close
    // this new owned gate before pose/resolution work, while no original update
    // has run. The existing engine mismatch path replaces its own outputs.
    function<void (*)(void*, std::uint64_t, bool)>(runtime, runtime.contract.functions.activate_entry)(
        reinterpret_cast<void*>(runtime.manager), id, false);
    view = inspect_entry(runtime, id);
    runtime.creation_valid = view.complete && view.ready && (view.flags[0] & 1u);
    if (!runtime.creation_valid) {
      runtime.stage_error = "The new owned view could not be validated with its render gate closed.";
      return id;
    }
    if (!prepare_owned_view_aa(runtime, id, view)) {
      runtime.creation_valid = false;
      return id;
    }
    apply_pose(runtime, view, runtime.mounted_poses[runtime.creations - 1]);
    ViewDimensions desired{};
    if (!plan_view_resize(view.dimensions, runtime.creations - 1, desired, runtime.allocation_panes)) {
      runtime.creation_valid = false;
      runtime.stage_error = "The owned view dimensions cannot be validated for its requested PFD pane.";
      return id;
    }
    // Retain the requested dimensions, but do not resize yet. The original
    // manager gets one closed-gate update to populate its primary-size cache.
    runtime.resized_dimensions[runtime.creations - 1] = desired;
  }
  return id;
}

bool resize_closed_entry(Runtime& runtime, ec::EntryId id, unsigned index, bool initialize_output = true) {
  const auto view = inspect_entry(runtime, id);
  if (!view.complete || !view.ready || !(view.flags[0] & 1u) || index >= runtime.resized_dimensions.size()) {
    runtime.stage_error = "The owned view could not be validated with its render gate closed.";
    return false;
  }
  const auto desired = runtime.resized_dimensions[index];
  const ViewResizeCallbacks resize_callbacks{
      &runtime,
      [](void* opaque, std::uint64_t address) noexcept {
        auto& current = *static_cast<Runtime*>(opaque);
        function<void (*)(void*)>(current, current.contract.functions.update_view)(reinterpret_cast<void*>(address));
        return true;
      },
      [](void* opaque, std::uint64_t address) noexcept -> std::uint64_t {
        auto& current = *static_cast<Runtime*>(opaque);
        return reinterpret_cast<std::uintptr_t>(
            function<void* (*)(void*)>(current, current.contract.functions.refresh_output)(reinterpret_cast<void*>(address)));
      }};
  const auto resized = initialize_output ? resize_owned_view(view, index, desired, resize_callbacks, runtime.allocation_panes)
                                         : restore_owned_view_dimensions(view, index, desired, resize_callbacks, runtime.allocation_panes);
  if (!resized.complete) {
    runtime.stage_error = view_resize_status_name(resized.status);
    return false;
  }
  const auto confirmed = inspect_entry(runtime, id);
  const bool valid =
      confirmed.complete && confirmed.ready && confirmed.view_address == view.view_address && confirmed.node_address == view.node_address &&
      confirmed.camera_address == view.camera_address && confirmed.dimensions == desired && (confirmed.flags[0] & 1u) &&
      (initialize_output || (confirmed.mode == 2 && confirmed.resource_present && confirmed.output_dimensions == desired[0] &&
                             confirmed.resource_address == view.resource_address));
  if (!valid) {
    runtime.stage_error = "The resized owned-view chain did not remain stable; its render gate stays closed.";
    return false;
  }
  runtime.resized_ids[index] = id;
  return true;
}

bool erase(void* opaque, ec::ManagerToken token, ec::EntryId id) noexcept {
  auto& runtime = *static_cast<Runtime*>(opaque);
  runtime.lifecycle_touched = true;
  if (!manager_context(runtime, reinterpret_cast<void*>(token.identity)) || token != runtime.token)
    return false;
  LocalMemoryReader reader;
  auto entries = inspected(runtime, [&] { return ec::inspect_owned_entries(reader, token.identity, {id, 0}); });
  if (!entries.complete) {
    runtime.retirement.forget(token, id);
    return false;
  }
  if (!entries.entries[0].found) {
    runtime.retirement.forget(token, id);
    return true;
  }
  // An ID-table match alone does not authorize native removal. Its ready path
  // indexes the view pool; its pending path skips normal view detachment while
  // still destroying entry-held references. Never enter either with an
  // unavailable chain, or erase on the same update that closes an active gate.
  const auto view = inspect_entry(runtime, id);
  const auto action = runtime.retirement.observe(token, id, runtime.updates, view);
  if (action == ViewRetirement::Action::close_gate) {
    function<void (*)(void*, std::uint64_t, bool)>(runtime, runtime.contract.functions.activate_entry)(
        reinterpret_cast<void*>(token.identity), id, false);
    return false;
  }
  if (action != ViewRetirement::Action::erase)
    return false;
  function<void (*)(void*, std::uint64_t)>(runtime, runtime.contract.functions.erase_entry)(reinterpret_cast<void*>(token.identity), id);
  reader.reset_budget();
  entries = inspected(runtime, [&] { return ec::inspect_owned_entries(reader, token.identity, {id, 0}); });
  const bool absent = entries.complete && !entries.entries[0].found;
  if (absent)
    runtime.retirement.forget(token, id);
  return absent;
}

void record_stop(Runtime& runtime,
                 SceneStopReason reason,
                 const char* detail,
                 std::uint64_t now,
                 std::optional<std::uint64_t> expected_start_revision = std::nullopt) {
  const std::lock_guard lock(runtime.mutex);
  // An inspection belongs to the request it sampled. Do not overwrite recovery
  // state that a newer explicit Start/Stop has already replaced.
  if (expected_start_revision && runtime.requested_start_revision != *expected_start_revision)
    return;
  // Preserve the matching detail while a terminal identity/startup failure is
  // retained. Only an explicit Start/Stop may leave that state.
  if (latched_scene_stop(runtime.recovery.reason()) && reason != runtime.recovery.reason())
    return;
  if (runtime.recovery.reason() == reason && runtime.stop_detail == detail && (runtime.recovery.pending() || !retryable_scene_stop(reason)))
    return;
  runtime.recovery.failed(reason, now);
  runtime.stop_detail = detail;
}

// Recheck at destructive fallback boundaries too: the public epoch may change
// while a full native inspection is running. A flight change parks the pair.
bool hold_changed_session(Runtime& runtime, const ec::Snapshot& pair) {
  const auto epoch = get_aircraft_session_epoch();
  const std::lock_guard lock(runtime.mutex);
  if (!RetainedProfileTransition::session_changed(pair, runtime.scene_session_epoch, epoch))
    return false;
  runtime.suspended.store(true);
  scene_handoff().stop_scene();
  runtime.requested_start = false;
  ++runtime.requested_start_revision;
  runtime.retained_restart_requested = false;
  if (!runtime.profile_transition.holding()) {
    runtime.profile_transition.begin(runtime.aircraft_profile->id, pair, runtime.resized_dimensions);
    if (runtime.profile_transition_token != UINT64_MAX)
      ++runtime.profile_transition_token;
    else
      runtime.profile_transition.refuse();
  }
  return true;
}
void observer(void* manager) noexcept {
  auto& runtime = state();
  if (!runtime.enabled.load(std::memory_order_acquire) || runtime.observing.test_and_set(std::memory_order_acquire))
    return;
  struct Guard {
    Runtime& runtime;
    ~Guard() { runtime.observing.clear(std::memory_order_release); }
  } guard{runtime};
  bool serviced = false;
  LARGE_INTEGER started{};
  LocalMemoryMetrics memory_metrics;
  ScopedLocalMemoryMetrics memory_scope(memory_metrics);
  try {
    ++runtime.updates;
    const auto before = runtime.pair.snapshot();
    const auto now = GetTickCount64();
    const auto settings = runtime.requested_settings.load(std::memory_order_acquire);
    bool requested_start = false;
    bool mount_changed = false;
    bool recovery_pending = false;
    bool pair_ready = false;
    bool profile_hold = false;
    const auto session_epoch = get_aircraft_session_epoch();
    std::uint64_t start_revision = 0;
    {
      const std::lock_guard lock(runtime.mutex);
      if (!before.owned_ids[0] && !before.owned_ids[1] && !before.creation_pending)
        runtime.aircraft_profile = runtime.requested_profile;
      if (RetainedProfileTransition::session_changed(before, runtime.scene_session_epoch, session_epoch) &&
          !runtime.profile_transition.holding()) {
        runtime.suspended.store(true);
        scene_handoff().stop_scene();
        runtime.requested_start = false;
        ++runtime.requested_start_revision;
        runtime.retained_restart_requested = false;
        runtime.profile_transition.begin(runtime.aircraft_profile->id, before, runtime.resized_dimensions);
        if (runtime.profile_transition_token != UINT64_MAX)
          ++runtime.profile_transition_token;
        else
          runtime.profile_transition.refuse();
      }
      profile_hold = runtime.profile_transition.holding() &&
                     (before.owned_ids[0] || before.owned_ids[1] || runtime.profile_transition.awaiting_pair());
      requested_start = runtime.requested_start;
      start_revision = runtime.requested_start_revision;
      recovery_pending = runtime.recovery.pending() || runtime.published.view_waiting || runtime.published.pose_waiting;
      pair_ready = runtime.published.ready[0] && runtime.published.ready[1];
      if (runtime.mount_revision != runtime.requested_mount_revision) {
        mount_changed = true;
        runtime.mounts = runtime.requested_mounts;
        runtime.mount_revision = runtime.requested_mount_revision;
      }
    }
    auto next_schedule = runtime.schedule;
    next_schedule.configure(settings & 0xffu, settings >> 8);
    std::array<bool, 2> desired{};
    const bool scheduled_pair = before.state == ec::State::active && runtime.scheduled_ids == before.owned_ids;
    const bool suspended = runtime.suspended.load();
    if (scheduled_pair)
      desired = next_schedule.tick(now, suspended);
    const bool gate_change = scheduled_pair && desired != runtime.gates;
    const ProbeInspectionState inspection_state{scheduled_pair && before.owner.valid() && before.owned_ids[0] && before.owned_ids[1] &&
                                                    before.owned_ids[0] != before.owned_ids[1] && runtime.resized_ids == before.owned_ids &&
                                                    before.failure == ec::Failure::none && before.blocked == ec::Blocked::none,
                                                suspended,
                                                !runtime.gates[0] && !runtime.gates[1],
                                                pair_ready,
                                                before.request_pending || before.creation_pending,
                                                requested_start,
                                                runtime.resize_warmup.pending(),
                                                recovery_pending || profile_hold,
                                                mount_changed};
    const auto inspection = runtime.inspection_gate.decide(inspection_state);
    if (inspection == ProbeInspectionDecision::idle) {
      // No native pointer is used while idle. The first resume/lifecycle/mount
      // update bypasses the periodic throttle and repeats the complete guard.
      runtime.schedule = next_schedule;
      return;
    }
    if (inspection != ProbeInspectionDecision::required && !before.request_pending && !gate_change && !runtime.resize_warmup.pending() &&
        now - runtime.last_inspection < 250) {
      runtime.schedule = next_schedule;
      return;
    }
    runtime.last_inspection = now;
    serviced = true;
    ++runtime.inspection_count;
    if (!runtime.counter_frequency.QuadPart)
      QueryPerformanceFrequency(&runtime.counter_frequency);
    QueryPerformanceCounter(&started);
    runtime.performance = {};
    runtime.inspection_changed = false;
    ProbeSnapshot report;
    bool body_pose_failed = false;
    report.hook_installed = true;
    report.accepting_requests = true;
    report.updates = runtime.updates;
    report.thread_id = GetCurrentThreadId();
    const bool close_only = inspection_state.established_pair && pair_ready && gate_change && !desired[0] && !desired[1] &&
                            (runtime.gates[0] || runtime.gates[1]) && !before.request_pending && !before.creation_pending &&
                            !requested_start && !runtime.resize_warmup.pending() && !runtime.resize_recovery.pending() &&
                            !runtime.resize_recovery.failed() && !recovery_pending && !mount_changed;
    // Only an established pair can combine these adjacent read-only stages.
    // No lifecycle callback, publication or private call runs inside the cache.
    // A callback requested meanwhile invalidates the prepared view result below.
    const bool fuse_pair = inspection_state.established_pair && pair_ready && !before.request_pending && !before.creation_pending &&
                           !requested_start && !runtime.resize_warmup.pending() && !runtime.resize_recovery.pending() &&
                           !runtime.resize_recovery.failed() && !recovery_pending && !mount_changed && !profile_hold;
    bool prepared_pair = false;
    std::array<ec::OwnedViewSnapshot, 2> prepared_views{};
    SceneCaptureTicket prepared_ticket{};
    std::uint32_t prepared_free_views = 0;
    ManagerInspection manager_inspection;
    const auto inspect_manager = [&] {
      if (fuse_pair) {
        prepared_ticket = timed(runtime, ProbeStage::handoff, [] { return scene_handoff().begin_capture(); });
        ScopedLocalMemoryQueryCache queries(LocalMemoryQueryMode::private_pages);
        const bool manager_valid =
            timed(runtime, ProbeStage::manager, [&] { return manager_context(runtime, manager, &queries, &manager_inspection); });
        if (manager_valid && runtime.token == before.owner)
          inspect_pair(runtime, before.owned_ids, report, prepared_views, false, &queries);
        const bool stable = queries.finish();
        if (manager_valid && stable) {
          prepared_pair = runtime.token == before.owner;
          prepared_free_views = report.free_views;
          return true;
        }
        // Never consume a provisional identity after failed endpoint validation.
        // The ordinary fresh path retains its existing refusal/recovery policy.
        prepared_views = {};
        report.ready = report.resource_present = {};
        report.dimensions = {};
        report.flags = {};
      }
      return timed(runtime, ProbeStage::manager, [&] { return manager_context(runtime, manager, nullptr, &manager_inspection); });
    };
    const auto start_body = requested_start ? sample_body_pose(now) : BodyPoseSnapshot{};
    if (profile_hold) {
      service_profile_transition(runtime, manager, report);
    } else if (park_initial_scene(suspended, before.owned_ids[0] || before.owned_ids[1])) {
      // OFF also parks an unfinished background creation request. Keep its
      // mailbox request for an explicit resume; never create behind a lost
      // heartbeat, cutoff or expired prewarm budget, and never erase a pair.
      report.pair = before;
      report.message = "Initial camera creation parked until render demand resumes.";
    } else if (requested_start && !before.owned_ids[0] && !before.owned_ids[1] && !start_body.valid && !start_body.calibration_required) {
      // Public startup is asynchronous. Do not walk private manager, pool or
      // aircraft graphs repeatedly while its first telemetry is still pending.
      report.pair = before;
      report.message = std::string("Waiting for read-only aircraft telemetry: ") + start_body.error;
    } else if (close_only && close_owned_pair(runtime, manager, before, report)) {
      runtime.schedule = next_schedule;
    } else if (!inspect_manager()) {
      scene_handoff().stop_scene();
      const bool temporary = manager_inspection.temporary();
      record_stop(runtime, temporary ? SceneStopReason::inspection_unavailable : SceneStopReason::identity_refused,
                  manager_inspection.detail, now, start_revision);
      {
        const std::lock_guard lock(runtime.mutex);
        // Consume a queued initial start too: after this stop, only the bounded
        // recovery path may create again after fresh guards and confirmed cleanup.
        if (requested_start && runtime.requested_start && runtime.requested_start_revision == start_revision) {
          runtime.requested_start = false;
          ++runtime.requested_start_revision;
        }
      }
      // Mailbox only. No cleanup/native call is allowed in this failed pass.
      // The next valid update repeats every guard and retains IDs until erase
      // confirms absence. This also handles a temporary failure of an active pair.
      if (temporary)
        runtime.pair.request_disable();
      report.message =
          std::string(temporary ? "Camera manager inspection temporarily unavailable: " : "Camera manager identity refused: ") +
          manager_inspection.detail;
      report.pair = runtime.pair.snapshot();
    } else {
      LocalMemoryReader reader;
      ec::ViewPoolSnapshot pool;
      // Creation still needs its own pre-call pool validation. An established
      // pair is inspected once, below, after processing any lifecycle request.
      if (before.state != ec::State::active || before.request_pending || requested_start || runtime.resize_warmup.pending())
        pool = timed(runtime, ProbeStage::pool,
                     [&] { return inspected(runtime, [&] { return ec::inspect_view_pool(reader, runtime.renderer); }); });
      report.free_views = pool.valid ? pool.free_count : 0;
      runtime.creations = 0;
      runtime.creation_pose_unavailable = false;
      runtime.lifecycle_touched = false;
      runtime.creation_valid = pool.valid && pool.free_count >= 2;
      ec::EngineCallbacks callbacks{&runtime, initialize, create, erase};
      timed(runtime, ProbeStage::lifecycle, [&] { runtime.pair.process_update(runtime.token, callbacks); });
      auto pair = runtime.pair.snapshot();
      bool retry_pose_waiting = false;
      if (!requested_start && !pair.owned_ids[0] && !pair.owned_ids[1]) {
        const auto retry_body = sample_body_pose(GetTickCount64());
        bool retry_pending = false, calibrate_retry = false;
        {
          const std::lock_guard lock(runtime.mutex);
          retry_pending = runtime.recovery.pending();
          calibrate_retry = !runtime.requested_start &&
                            initial_retry_calibration_allowed(runtime.recovery, pair, retry_body.calibration_required,
                                                              runtime.suspended.load(), start_revision, runtime.requested_start_revision);
        }
        bool fresh_pose = retry_pending && retry_body.valid;
        if (calibrate_retry) {
          // A transient manager failure may consume the initial Start before
          // calibration has run. Progress its existing read-only pose path;
          // do not consume a retry or call a native initializer/create until
          // the full pose succeeds. Stop/new requests are rechecked below.
          fresh_pose = timed(runtime, ProbeStage::pose, [&] { return capture_pose(runtime); });
          retry_pose_waiting = report.pose_waiting = !fresh_pose;
        }
        const std::lock_guard lock(runtime.mutex);
        if (!runtime.requested_start && runtime.requested_start_revision == start_revision && !runtime.suspended.load() &&
            runtime.recovery.retry(GetTickCount64(), pair, fresh_pose)) {
          runtime.requested_start = requested_start = true;
          start_revision = ++runtime.requested_start_revision;
        }
      }
      bool waiting_for_body = retry_pose_waiting;
      bool resolution_pause = false;
      if (requested_start && !pair.owned_ids[0] && !pair.owned_ids[1]) {
        waiting_for_body = !timed(runtime, ProbeStage::pose, [&] { return capture_pose(runtime); });
        bool create_requested = false;
        if (!waiting_for_body) {
          // Stop and a new Start share this small mailbox transaction. A slow
          // calibration cannot resurrect an enable that the UI has cancelled.
          const std::lock_guard lock(runtime.mutex);
          if (runtime.requested_start && runtime.requested_start_revision == start_revision && !runtime.suspended.load()) {
            scene_handoff().begin_scene();
            runtime.pair.request_independent_pose();
            runtime.requested_start = false;
            create_requested = true;
          }
        }
        if (create_requested) {
          timed(runtime, ProbeStage::lifecycle, [&] { runtime.pair.process_update(runtime.token, callbacks); });
          pair = runtime.pair.snapshot();
          if (pair.state != ec::State::active) {
            bool deferred = false;
            {
              const std::lock_guard lock(runtime.mutex);
              deferred = defer_initial_pose_failure(runtime.pair, runtime.token, runtime.recovery, runtime.creation_pose_unavailable,
                                                    runtime.creations, start_revision, runtime.requested_start_revision, now);
              if (deferred)
                runtime.stop_detail = runtime.creation_error;
            }
            if (deferred) {
              pair = runtime.pair.snapshot();
              waiting_for_body = report.pose_waiting = true;
            } else {
              record_stop(runtime, SceneStopReason::creation_failed,
                          *runtime.stage_error ? runtime.stage_error : "Owned camera creation did not complete.", now, start_revision);
            }
          }
        }
      }
      if (pair.state == ec::State::active) {
        // Creation may claim previously free slots. Recheck that transition;
        // an established pair validates its pool inside inspect_pair below.
        if (runtime.creations != 0) {
          reader.reset_budget();
          pool = timed(runtime, ProbeStage::pool,
                       [&] { return inspected(runtime, [&] { return ec::inspect_view_pool(reader, runtime.renderer); }); });
          report.free_views = pool.valid ? pool.free_count : 0;
          runtime.creation_valid = runtime.creation_valid && runtime.resize_warmup.begin(pair.owned_ids, runtime.updates);
          runtime.gates = {};
        }
        const bool closed_warmup = runtime.creations != 0 && runtime.creation_valid && runtime.resize_warmup.pending();
        bool initial_resize_failed = false;
        if (!closed_warmup && runtime.resize_warmup.pending()) {
          runtime.stage_error = "The initial closed-gate resize phase could not be validated.";
          runtime.creation_valid = timed(runtime, ProbeStage::lifecycle, [&] {
            return runtime.resize_warmup.may_resize(pair.owned_ids, runtime.updates) &&
                   resize_closed_entry(runtime, pair.owned_ids[0], 0) && resize_closed_entry(runtime, pair.owned_ids[1], 1);
          });
          if (runtime.creation_valid)
            runtime.resize_warmup.finish(pair.owned_ids, runtime.updates);
          else
            initial_resize_failed = true;
        }
        std::array<ec::OwnedViewSnapshot, 2> views{};
        if (!closed_warmup && !initial_resize_failed) {
          if (prepared_pair && !runtime.lifecycle_touched && pair.owner == before.owner && pair.owned_ids == before.owned_ids &&
              pair.failure == ec::Failure::none && pair.blocked == ec::Blocked::none && !pair.request_pending && !pair.creation_pending) {
            views = prepared_views;
            report.free_views = prepared_free_views;
            if (report.ready[0] && report.ready[1])
              report.outputs_matched = timed(runtime, ProbeStage::handoff, [&] {
                return scene_handoff().publish(prepared_ticket, {runtime.token.identity, runtime.token.generation}, pair.owned_ids,
                                               {views[0].resource_address, views[1].resource_address});
              });
          } else {
            inspect_pair(runtime, pair.owned_ids, report, views);
          }
        }
        const bool wait_for_views = runtime.view_wait.observe(now, pair,
                                                              !runtime.resize_warmup.pending() && runtime.scheduled_ids == pair.owned_ids &&
                                                                  runtime.resized_ids == pair.owned_ids &&
                                                                  runtime.inspection_stop == SceneStopReason::inspection_unavailable,
                                                              views);
        resolution_pause = runtime.resize_recovery.pending() || runtime.resize_recovery.failed() ||
                           (runtime.inspection_stop == SceneStopReason::resolution_changed && runtime.scheduled_ids == pair.owned_ids &&
                            runtime.resized_ids == pair.owned_ids);
        if (initial_resize_failed && hold_changed_session(runtime, pair)) {
          report.view_waiting = true;
          runtime.message = "Aircraft changed during initial resizing; retaining camera IDs for fresh validation.";
        } else if (initial_resize_failed) {
          // A refused/partial first resize is a startup failure, not drift of an
          // established output. Preserve its cause before any further inspector
          // can replace it, then use the existing guarded retirement path. No
          // automatic retry may repeat an allocation whose outcome is uncertain.
          runtime.message = std::string("Initial camera resize failed: ") + runtime.stage_error;
          record_stop(runtime, SceneStopReason::creation_failed, runtime.message.c_str(), now, start_revision);
          scene_handoff().stop_scene();
          runtime.pair.request_disable();
          timed(runtime, ProbeStage::lifecycle, [&] { runtime.pair.process_update(runtime.token, callbacks); });
          pair = runtime.pair.snapshot();
        } else if (resolution_pause) {
          // Never remove/recreate live camera entries for a primary-size change.
          // Mode2's original update only overwrites P16..39: restore those fields
          // if its already allocated Bitmap still has the exact desired size.
          if (!runtime.resize_recovery.pending() && !runtime.resize_recovery.failed()) {
            runtime.resize_recovery.begin(pair.owner, pair.owned_ids);
            scene_handoff().stop_scene();
            record_stop(runtime, SceneStopReason::resolution_changed,
                        "Primary dimensions changed; camera IDs retained and output allocation prohibited.", now);
          }
          const auto action = runtime.resize_recovery.observe(pair.owner, pair.owned_ids, runtime.updates, views);
          // A temporarily unavailable feed grants no native-call permission.
          // Close only independently fresh ready chains; never use failed ones.
          for (unsigned i = 0; i < views.size(); ++i) {
            const auto& view = views[i];
            if (view.complete && view.ready && view.status == ec::OwnedViewStatus::ready) {
              if ((view.flags[0] & 1u) == 0)
                function<void (*)(void*, std::uint64_t, bool)>(runtime, runtime.contract.functions.activate_entry)(
                    reinterpret_cast<void*>(runtime.manager), pair.owned_ids[i], false);
              runtime.gates[i] = false;
            }
          }
          runtime.message = "Graphics settings changed; camera IDs retained while both render gates close.";
          if (action == ViewResizeRecovery::Action::resize) {
            const bool outputs_unchanged = views[0].mode == 2 && views[1].mode == 2 && views[0].resource_present &&
                                           views[1].resource_present && views[0].output_dimensions == runtime.allocation_panes[0] &&
                                           views[1].output_dimensions == runtime.allocation_panes[1];
            const bool restored = outputs_unchanged && timed(runtime, ProbeStage::lifecycle, [&] {
                                    return resize_closed_entry(runtime, pair.owned_ids[0], 0, false) &&
                                           resize_closed_entry(runtime, pair.owned_ids[1], 1, false);
                                  });
            if (restored && runtime.resize_recovery.finish(pair.owner, pair.owned_ids)) {
              // Reopening requires a new full pair inspection and fresh captures
              // on the next observer, never this pre-restoration snapshot.
              scene_handoff().begin_scene();
              runtime.schedule.reset();
              const std::lock_guard lock(runtime.mutex);
              runtime.recovery.resumed_retained_resolution();
              runtime.stop_detail.clear();
              runtime.message = "Camera dimensions restored without replacing entries or output textures; waiting for fresh frames.";
            } else {
              runtime.resize_recovery.mark_failed();
            }
          }
          if (runtime.resize_recovery.failed())
            runtime.message = "Camera paused after graphics change: existing output could not be safely retained. Restart MSFS to resume.";
          report.ready = report.resource_present = {};
          report.outputs_matched = false;
          report.view_waiting = true;
        } else if (closed_warmup) {
          // No publication, resize or activation yet. The tail-called original
          // initializes its cache while both verified new gates remain closed.
        } else if (!runtime.resize_warmup.pending() && report.ready[0] && report.ready[1] &&
                   (runtime.creations == 0 || runtime.creation_valid)) {
          const bool new_pair = runtime.scheduled_ids != pair.owned_ids;
          // The early tick only decides whether validation is necessary. Anchor
          // the committed pulse near its call after potentially slow reads.
          next_schedule = runtime.schedule;
          next_schedule.configure(settings & 0xffu, settings >> 8);
          if (new_pair) {
            // A changed pair means the controller removed the prior IDs before
            // creation. Only this lifecycle transition resets pulse deadlines.
            next_schedule.reset();
          }
          desired = next_schedule.tick(GetTickCount64(), runtime.suspended.load());
          const bool needs_pose = desired[0] || desired[1];
          const bool pose_ready = !needs_pose || timed(runtime, ProbeStage::pose, [&] { return capture_pose(runtime); });
          bool aa_ready = true;
          if (pose_ready && needs_pose) {
            for (unsigned i = 0; i < desired.size(); ++i) {
              if (desired[i] &&
                  !timed(runtime, ProbeStage::aa, [&] { return prepare_owned_view_aa(runtime, pair.owned_ids[i], views[i]); })) {
                aa_ready = false;
                break;
              }
              report.flags[i] = views[i].flags;
            }
          }
          if (pose_ready && aa_ready) {
            // Position changes happen only inside the validated observer phase,
            // before the original manager update can consume a newly opened gate.
            if (needs_pose)
              timed(runtime, ProbeStage::pose, [&] {
                for (unsigned i = 0; i < desired.size(); ++i)
                  if (desired[i])
                    apply_pose(runtime, views[i], runtime.mounted_poses[i]);
              });
            timed(runtime, ProbeStage::activation, [&] { apply_gates(runtime, pair.owned_ids, desired, report, new_pair); });
            runtime.scheduled_ids = pair.owned_ids;
            runtime.schedule = next_schedule;
          } else if (!aa_ready) {
            timed(runtime, ProbeStage::activation, [&] { apply_gates(runtime, pair.owned_ids, {}, report, true); });
            runtime.scheduled_ids = pair.owned_ids;
            runtime.schedule = next_schedule;
            report.view_waiting = true;
            runtime.message = std::string("Camera AA configuration unavailable; retaining the closed camera pair: ") + runtime.stage_error;
          } else {
            body_pose_failed = true;
            timed(runtime, ProbeStage::activation, [&] { apply_gates(runtime, pair.owned_ids, {}, report, true); });
            if (runtime.pose_busy) {
              // Missing/stale telemetry is not a lost aircraft lifetime. Never
              // apply an old transform. Retain the closed pair and completed
              // image while the next pulse waits for a fresh validated pose.
              runtime.scheduled_ids = pair.owned_ids;
              runtime.schedule = next_schedule;
              report.pose_waiting = true;
            } else if (hold_changed_session(runtime, pair)) {
              report.pose_waiting = true;
              runtime.message = "Aircraft changed during pose inspection; retaining the closed camera pair.";
            } else {
              record_stop(runtime, SceneStopReason::pose_invalid, runtime.message.c_str(), now);
              scene_handoff().stop_scene();
              runtime.pair.request_disable();
              timed(runtime, ProbeStage::lifecycle, [&] { runtime.pair.process_update(runtime.token, callbacks); });
              pair = runtime.pair.snapshot();
              runtime.stage_error = "Aircraft body pose became unavailable; the owned camera gates were closed and removal requested.";
            }
          }
        } else if (wait_for_views) {
          // A pending entry exposes no validated view/node/camera pointers.
          // Only close the other freshly verified ready view, and never issue
          // activation/pose/resize calls against the pending one. Completed GPU
          // images retain the normal handoff resource-generation guards.
          timed(runtime, ProbeStage::activation, [&] {
            for (unsigned i = 0; i < pair.owned_ids.size(); ++i)
              if (report.ready[i]) {
                if (runtime.gates[i] || (report.flags[i][0] & 1u) == 0)
                  function<void (*)(void*, std::uint64_t, bool)>(runtime, runtime.contract.functions.activate_entry)(
                      reinterpret_cast<void*>(runtime.manager), pair.owned_ids[i], false);
                runtime.gates[i] = false;
              }
          });
          runtime.schedule = next_schedule;
          report.view_waiting = true;
        } else if (hold_changed_session(runtime, pair)) {
          report.view_waiting = true;
          runtime.message = "Aircraft changed during view inspection; retaining camera IDs for fresh validation.";
        } else {
          record_stop(runtime,
                      runtime.inspection_stop == SceneStopReason::none ? SceneStopReason::identity_refused : runtime.inspection_stop,
                      *runtime.stage_error ? runtime.stage_error : "Owned view validation failed; removal requested.", now);
          scene_handoff().stop_scene();
          runtime.pair.request_disable();
          timed(runtime, ProbeStage::lifecycle, [&] { runtime.pair.process_update(runtime.token, callbacks); });
          pair = runtime.pair.snapshot();
          runtime.message = *runtime.stage_error ? runtime.stage_error : "Owned view validation failed; removal requested.";
        }
      }
      if (!pair.owned_ids[0] && !pair.owned_ids[1]) {
        scene_handoff().stop_scene();
        runtime.owned_control = 0;
        runtime.scheduled_ids = {};
        runtime.resized_ids = {};
        runtime.resized_dimensions = {};
        runtime.resize_warmup.clear();
        runtime.resize_recovery.clear();
        runtime.view_wait.clear();
        runtime.retirement.clear();
        runtime.gates = {};
        runtime.schedule.reset();
      }
      report.pair = pair;
      if (pair.state != ec::State::active) {
        report.ready = {};
        report.resource_present = {};
        report.dimensions = {};
        report.flags = {};
      }
      report.pose_captured = runtime.pose_captured;
      if (resolution_pause)
        report.message = runtime.message;
      else if (body_pose_failed || waiting_for_body)
        report.message = runtime.message.empty() ? runtime.stage_error : runtime.message;
      else if (report.view_waiting)
        report.message = "Owned view inspection unavailable; camera IDs retained while waiting for fresh validation.";
      else if (pair.state == ec::State::active && runtime.resize_warmup.pending())
        report.message = "New scene views are closed for their initial engine update; final resizing is pending.";
      else if (pair.state == ec::State::active)
        report.message = "Nose and tail scene views use separate aircraft mounts and refresh before alternating activation pulses.";
      else if (pair.state == ec::State::cleanup_pending)
        report.message = "Removal waits for freshly validated, closed views; owned IDs remain retained until confirmed absent.";
      else if (pair.state == ec::State::disabled)
        report.message = "Scene test stopped; owned IDs are confirmed absent.";
      else if (!runtime.message.empty())
        report.message = runtime.message;
      else if (*runtime.stage_error)
        report.message = runtime.stage_error;
      else
        report.message = pool.valid && pool.free_count >= 2 ? "Scene creation failed; inspect readiness and owned IDs."
                                                            : "Scene creation refused: two free engine views are required.";
    }
    // Preserve cumulative diagnostics through the status replacement. The new
    // duration is published after this operation has itself been timed.
    report.inspection_count = runtime.inspection_count;
    report.created_total = runtime.created_total;
    report.view_wait_count = runtime.view_wait.episodes();
    report.observer_last_ms = runtime.observer_last_ms;
    report.observer_max_ms = runtime.observer_max_ms;
    report.gates = runtime.gates;
    report.activation_counts = runtime.activation_counts;
    report.mounts = runtime.mounts;
    report.mounted_poses = runtime.mounted_poses;
    timed(runtime, ProbeStage::publication, [&] {
      const std::lock_guard lock(runtime.mutex);
      runtime.published_allocation = {report.pair.owner, report.pair.owned_ids, runtime.resized_dimensions};
      report.profile_transition_token = runtime.profile_transition_token;
      report.profile_transition_id = runtime.profile_transition.id();
      report.profile_transition_pending = runtime.profile_transition.pending();
      report.profile_transition_ready = runtime.profile_transition.ready();
      report.profile_transition_failed = runtime.profile_transition.failed();
      report.accepting_requests = runtime.recovery.requested();
      report.recovery_pending = runtime.recovery.pending();
      report.restart_pending = runtime.requested_start;
      report.recovery_attempts = runtime.recovery.attempts();
      report.stop_sequence = runtime.recovery.sequence();
      report.stop_reason = runtime.recovery.reason();
      report.stop_detail = runtime.stop_detail;
      runtime.published = std::move(report);
    });
  } catch (...) {
    // C++ allocation/metadata failures do not escape across the engine ABI.
    // This is not an access-violation catcher or a private-engine ABI guarantee.
    scene_handoff().stop_scene();
    runtime.pair.request_disable();
    try {
      record_stop(runtime, SceneStopReason::exception, "Observer exception; owned cleanup requested without automatic retry.",
                  GetTickCount64());
    } catch (...) {
    }
  }
  if (serviced) {
    runtime.performance.query_calls = memory_metrics.query_calls;
    runtime.performance.query_allocation_calls = memory_metrics.query_allocation_calls;
    runtime.performance.query_page_calls = memory_metrics.query_page_calls;
    runtime.performance.query_fallback_calls = memory_metrics.query_fallback_calls;
    runtime.performance.read_calls = memory_metrics.read_calls;
    runtime.performance.requested_bytes = memory_metrics.requested_bytes;
    runtime.performance.query_cache_hits = memory_metrics.query_cache_hits;
    runtime.performance.query_cache_validation_failures = memory_metrics.query_cache_validation_failures;
    if (runtime.counter_frequency.QuadPart > 0) {
      const auto milliseconds_per_tick = 1000.0 / static_cast<double>(runtime.counter_frequency.QuadPart);
      runtime.performance.query_ms = memory_metrics.query_ticks * milliseconds_per_tick;
      runtime.performance.read_ms = memory_metrics.read_ticks * milliseconds_per_tick;
    }
    // Sample after diagnostic/private work and status publication. The original
    // manager update executes only after this observer returns through the thunk.
    LARGE_INTEGER finished{};
    if (QueryPerformanceCounter(&finished) && started.QuadPart > 0 && runtime.counter_frequency.QuadPart > 0 &&
        finished.QuadPart >= started.QuadPart) {
      runtime.observer_last_ms =
          static_cast<double>(finished.QuadPart - started.QuadPart) * 1000.0 / static_cast<double>(runtime.counter_frequency.QuadPart);
      if (runtime.observer_last_ms > runtime.observer_max_ms)
        runtime.observer_max_ms = runtime.observer_last_ms;
    }
    try {
      const std::lock_guard lock(runtime.mutex);
      runtime.published.inspection_count = runtime.inspection_count;
      runtime.published.created_total = runtime.created_total;
      runtime.published.observer_last_ms = runtime.observer_last_ms;
      runtime.published.observer_max_ms = runtime.observer_max_ms;
      runtime.published.performance = runtime.performance;
      runtime.published.gates = runtime.gates;
      runtime.published.activation_counts = runtime.activation_counts;
    } catch (...) {
      runtime.pair.request_disable();
    }
  }
}
}  // namespace

void request_scene_test(bool reuse_calibration) noexcept {
  auto& runtime = state();
  const std::lock_guard start_lock(runtime.start_mutex);
  {
    const std::lock_guard lock(runtime.mutex);
    runtime.published.accepting_requests = false;
  }
  try {
    if (!runtime.hooked.load(std::memory_order_acquire)) {
      wchar_t path[32768]{};
      const auto length = GetModuleFileNameW(nullptr, path, 32768);
      const auto* basename = std::wcsrchr(path, L'\\');
      if (!length || length >= 32768 || _wcsicmp(basename ? basename + 1 : path, L"FlightSimulator2024.exe") != 0) {
        const std::lock_guard lock(runtime.mutex);
        runtime.published.message = "Native camera access requires the FlightSimulator2024.exe process.";
        return;
      }
      runtime.image = parse_verified_main_image();
      runtime.base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
      LocalImageReader reader(reinterpret_cast<HMODULE>(runtime.base), runtime.image.image_size);
      const auto contract = resolve_camera_contract(reader, runtime.image, runtime.base);
      if (!runtime.image.valid_image || !contract.valid) {
        const std::lock_guard lock(runtime.mutex);
        runtime.published.message =
            "Camera compatibility check failed: " + (runtime.image.valid_image ? contract.error : runtime.image.error);
        return;
      }
      runtime.contract = contract.contract;
      const auto disable_mask = inspect_activation_disable_mask(reader, runtime.image, runtime.contract.layout);
      if (!disable_mask.valid)
        throw std::runtime_error("Native activation-mask verification refused: " + disable_mask.error);
      std::uint64_t original = 0;
      const auto update_slot = runtime.contract.layout.manager_vtable + 15 * 8;
      const auto update = runtime.contract.functions.manager_update;
      if (!reader.read(update_slot, &original, sizeof(original)) || original != runtime.base + update)
        throw std::runtime_error("Manager update slot does not match the resolved camera contract.");
      HMODULE pinned = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&observer),
                              &pinned))
        throw std::runtime_error("Pinning the observer module failed.");
      // This build declares the slot as read-only, non-executable image data,
      // while the Xbox loader maps its page executable/write-copy. The hook
      // rechecks the bounds and preserves execute permission through Windows'
      // copy-on-write promotion. Anonymous/executable PE sections are refused.
      const auto section = std::find_if(runtime.image.sections.begin(), runtime.image.sections.end(), [update_slot](const auto& item) {
        return (item.flags & 0x40000000u) && !(item.flags & 0xa2000000u) && update_slot >= item.rva &&
               update_slot - item.rva <= item.size && sizeof(void*) <= item.size - (update_slot - item.rva);
      });
      if (section == runtime.image.sections.end())
        throw std::runtime_error("Camera compatibility check failed: manager update slot moved outside static image data.");
      const engine_hook::ImageDataSlotProof image_data{reinterpret_cast<HMODULE>(runtime.base), runtime.image.image_size, section->rva,
                                                       section->size};
      const auto hook = engine_hook::install(reinterpret_cast<void**>(runtime.base + update_slot),
                                             reinterpret_cast<void*>(runtime.base + update), observer, &image_data);
      runtime.hooked.store(hook.pointer_changed, std::memory_order_release);
      runtime.protection_ready = hook.protection_restored;
      if (!hook.pointer_changed || !hook.protection_restored) {
        const std::lock_guard lock(runtime.mutex);
        runtime.published.hook_installed = hook.pointer_changed;
        runtime.published.message = std::string("Update observer refused: ") + engine_hook::status_name(hook.status);
        return;
      }
    }
    if (!runtime.protection_ready) {
      const auto restored = engine_hook::restore_protection();
      runtime.protection_ready = restored.protection_restored;
      if (!runtime.protection_ready)
        throw std::runtime_error("Update-slot page protection has not been restored; native requests remain disabled.");
    }
    if (!initialize_body_pose_provider())
      throw std::runtime_error("The read-only aircraft telemetry provider could not be started.");
    if (!reuse_calibration || !sample_body_pose(GetTickCount64()).valid)
      reset_body_pose_calibration();
    const std::lock_guard lock(runtime.mutex);
    const auto pair = runtime.pair.snapshot();
    const bool retained = runtime.profile_transition.can_resume(pair);
    if (runtime.profile_transition.holding() && (pair.owned_ids[0] || pair.owned_ids[1]) && !retained) {
      runtime.published.message = "Retained transition is not ready; no creation or removal requested.";
      return;
    }
    if (retained)
      runtime.retained_restart_requested = true;
    else {
      runtime.pair.request_disable();
      runtime.profile_transition.consume();
      runtime.scene_session_epoch = get_aircraft_session_epoch();
    }
    runtime.recovery.start();
    runtime.stop_detail.clear();
    runtime.requested_start = !retained;
    ++runtime.requested_start_revision;
    runtime.enabled.store(true, std::memory_order_release);
    runtime.published.hook_installed = true;
    runtime.published.accepting_requests = true;
    runtime.published.message = "Mounted scene test queued; waiting for a verified aircraft body pose.";
  } catch (const std::exception& error) {
    const std::lock_guard lock(runtime.mutex);
    runtime.published.message = error.what();
  } catch (...) {
    const std::lock_guard lock(runtime.mutex);
    runtime.published.message = "Native scene test initialization failed.";
  }
}

std::uint64_t request_scene_profile_transition(std::uint32_t id) noexcept {
  const auto* profile = profiles::find(id);
  if (!profile)
    return 0;
  auto& runtime = state();
  const std::lock_guard start_lock(runtime.start_mutex);
  const std::lock_guard lock(runtime.mutex);
  if (runtime.profile_transition_token == UINT64_MAX)
    return 0;
  runtime.suspended.store(true);
  scene_handoff().stop_scene();
  runtime.requested_start = false;
  ++runtime.requested_start_revision;
  runtime.retained_restart_requested = false;
  runtime.recovery.stop();
  runtime.requested_profile = profile;
  // Observer fields are not read from this thread. Allocation expectations are
  // mailbox-protected and survive temporarily empty diagnostic reports.
  const auto cancelled = runtime.pair.cancel_uncreated_request();
  if (cancelled == ec::EmptyPairCancel::busy)
    runtime.profile_transition.defer_pair(id);
  else
    runtime.profile_transition.begin_published(id, runtime.pair.snapshot(), runtime.published_allocation);
  const auto token = ++runtime.profile_transition_token;
  runtime.published.profile_transition_token = token;
  runtime.published.profile_transition_id = id;
  runtime.published.profile_transition_pending = runtime.profile_transition.pending();
  runtime.published.profile_transition_ready = runtime.profile_transition.ready();
  runtime.published.profile_transition_failed = runtime.profile_transition.failed();
  return token;
}
void request_scene_stop(bool keep_telemetry) noexcept {
  auto& runtime = state();
  const std::lock_guard start_lock(runtime.start_mutex);
  {
    const std::lock_guard lock(runtime.mutex);
    runtime.requested_start = false;
    runtime.retained_restart_requested = false;
    runtime.published.accepting_requests = false;
    ++runtime.requested_start_revision;
    runtime.recovery.stop();
    runtime.stop_detail = "Explicit camera OFF/Stop requested.";
    scene_handoff().stop_scene();
    const auto pair = runtime.pair.snapshot();
    if (!runtime.profile_transition.holding() || (!pair.owned_ids[0] && !pair.owned_ids[1])) {
      runtime.profile_transition.consume();
      runtime.pair.request_disable();
    }
  }
  if (!keep_telemetry)
    shutdown_body_pose_provider();
}

void note_scene_capture_progress(std::uint64_t now_ms) noexcept {
  auto& runtime = state();
  const std::lock_guard lock(runtime.mutex);
  runtime.recovery.capture_progress(now_ms);
}

void suspend_scene_rendering(bool suspended) noexcept {
  state().suspended.store(suspended);
}

void request_scene_rate(unsigned rate, unsigned feeds) noexcept {
  const auto settings = std::clamp(rate, kMinimumCameraRate, kMaximumCameraRate) | (std::clamp(feeds, 1u, 2u) << 8);
  state().requested_settings.store(settings, std::memory_order_release);
}

bool request_scene_profile(std::uint32_t id) noexcept {
  const auto* profile = profiles::find(id);
  if (!profile)
    return false;
  auto& runtime = state();
  const std::lock_guard lock(runtime.mutex);
  const auto pair = runtime.pair.snapshot();
  if (pair.owned_ids[0] || pair.owned_ids[1] || pair.creation_pending)
    return false;
  runtime.requested_profile = profile;
  return true;
}
bool request_scene_mounts(const MountPair& mounts) noexcept {
  if (!valid_mounts(mounts))
    return false;
  try {
    auto& runtime = state();
    const std::lock_guard lock(runtime.mutex);
    runtime.requested_mounts = mounts;
    ++runtime.requested_mount_revision;
    return true;
  } catch (...) {
    return false;
  }
}

ProbeSnapshot scene_snapshot() {
  auto& runtime = state();
  const std::lock_guard lock(runtime.mutex);
  auto result = runtime.published;
  result.pair = runtime.pair.snapshot();
  result.profile_transition_token = runtime.profile_transition_token;
  result.profile_transition_id = runtime.profile_transition.id();
  result.profile_transition_pending = runtime.profile_transition.pending();
  result.profile_transition_ready = runtime.profile_transition.ready();
  result.profile_transition_failed = runtime.profile_transition.failed();
  result.recovery_pending = runtime.recovery.pending();
  result.restart_pending = runtime.requested_start;
  result.recovery_attempts = runtime.recovery.attempts();
  result.stop_sequence = runtime.recovery.sequence();
  result.stop_reason = runtime.recovery.reason();
  result.stop_detail = runtime.stop_detail;
  const auto settings = runtime.requested_settings.load(std::memory_order_acquire);
  result.requested_rate = settings & 0xffu;
  result.requested_feeds = settings >> 8;
  result.mounts = runtime.requested_mounts;
  return result;
}

}  // namespace taxi_camera::native_camera
