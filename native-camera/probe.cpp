#include "probe.hpp"

#include "../discovery/aircraft_inventory.hpp"
#include "../engine-camera/owned_entry_inventory.hpp"
#include "../engine-camera/owned_view.hpp"
#include "../engine-hook/observer_hook.hpp"
#include "../src/scene_handoff.hpp"
#include "activation_mask.hpp"
#include "body_pose_provider.hpp"
#include "local_memory.hpp"
#include "profile.hpp"
#include "render_schedule.hpp"
#include "source_view.hpp"
#include "view_readiness_wait.hpp"
#include "view_resize.hpp"
#include "view_retirement.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <stdexcept>

namespace taxi_camera::native_camera {
namespace {
namespace ec = engine_camera;

constexpr std::uint32_t kManagerTable = 133571232;
constexpr std::uint32_t kUpdateSlot = kManagerTable + 15 * 8;
constexpr std::uint32_t kUpdate = 17648544;
constexpr std::uint32_t kOwnerGlobal = 173790440;
constexpr std::uint32_t kRendererGlobal = 173790384;

struct Runtime {
  std::mutex mutex;
  std::mutex start_mutex;
  ProbeSnapshot published;
  ec::PairController pair;
  std::atomic<bool> hooked{false};
  std::atomic<bool> enabled{false};
  std::atomic<bool> suspended{false};
  std::atomic<unsigned> requested_settings{15u | (2u << 8)};
  // Protected by mutex; never used directly by a native engine call.
  MountPair requested_mounts = default_mounts();
  const profiles::AircraftProfile* requested_profile = &profiles::A380;
  const profiles::AircraftProfile* aircraft_profile = &profiles::A380;
  std::uint64_t requested_mount_revision = 0;
  bool requested_start = false;
  std::uint64_t requested_start_revision = 0;
  SceneRecovery recovery;
  std::string stop_detail;
  bool protection_ready = false;
  std::atomic_flag observing = ATOMIC_FLAG_INIT;
  std::uintptr_t base = 0;
  discovery::Inventory image;
  // Observer-thread fields. No borrowed camera/output pointers cross into UI.
  std::uint64_t updates = 0;
  std::uint64_t inspection_count = 0;
  std::uint64_t created_total = 0;
  LARGE_INTEGER counter_frequency{};
  double observer_last_ms = 0;
  double observer_max_ms = 0;
  ProbePerformance performance;
  RenderSchedule schedule;
  std::array<ec::EntryId, 2> scheduled_ids{};
  std::array<bool, 2> gates{};
  std::array<std::uint64_t, 2> activation_counts{};
  std::array<ec::EntryId, 2> resized_ids{};
  std::array<ViewDimensions, 2> resized_dimensions{};
  ViewResizeWarmup resize_warmup;
  ViewReadinessWait view_wait;
  ViewRetirement retirement;
  ULONGLONG last_inspection = 0;
  std::uint64_t manager = 0;
  std::uint64_t control = 0;
  std::uint64_t owned_control = 0;
  std::uint64_t renderer = 0;
  ec::ManagerToken token{};
  unsigned creations = 0;
  bool creation_valid = false;
  bool pose_captured = false;
  bool pose_busy = false;
  bool inspection_changed = false;
  SceneStopReason inspection_stop = SceneStopReason::identity_refused;
  MountPair mounts = default_mounts();
  std::uint64_t mount_revision = 0;
  std::array<MountedPose, 2> mounted_poses{};
  const char* stage_error = "";
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
auto inspected(Runtime& runtime, Operation&& operation) {
  ScopedLocalMemoryQueryCache queries;
  auto result = operation();
  if (!queries.finish()) {
    runtime.inspection_changed = true;
    runtime.stage_error = "Memory-region metadata changed during inspection; no result was accepted.";
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

bool image_word(Runtime& runtime, std::uint32_t rva, std::uint64_t& output) {
  LocalImageReader reader(reinterpret_cast<HMODULE>(runtime.base), runtime.image.image_size);
  return reader.read(rva, &output, sizeof(output));
}

struct Handle {
  std::uint64_t control = 0;
  std::uint32_t generation = 0;
  std::uint32_t extra = 0;
};

// Called only from this manager's update thunk, immediately before its original.
// Cached pointer, weak handle and vptr must all agree, then be reread.
bool manager_context(Runtime& runtime, void* current_manager) {
  ScopedLocalMemoryQueryCache queries;
  LocalMemoryReader reader(512);
  std::uint64_t owner = 0, renderer = 0, cached = 0, payload = 0, vptr = 0;
  std::uint32_t generation = 0;
  Handle handle{};
  if (!image_word(runtime, kOwnerGlobal, owner) || !owner || !image_word(runtime, kRendererGlobal, renderer) || !renderer ||
      !word(reader, owner + 2496, cached) || cached != reinterpret_cast<std::uintptr_t>(current_manager) ||
      !word(reader, owner + 2480, handle) || !handle.control || !word(reader, handle.control + 28, generation) ||
      generation != handle.generation || !word(reader, handle.control, payload) || payload != cached || !word(reader, cached, vptr) ||
      vptr != runtime.base + kManagerTable)
    return false;
  std::uint64_t second = 0;
  Handle handle_again{};
  std::uint32_t generation_again = 0;
  if (!image_word(runtime, kOwnerGlobal, second) || second != owner || !image_word(runtime, kRendererGlobal, second) ||
      second != renderer || !word(reader, owner + 2496, second) || second != cached || !word(reader, owner + 2480, handle_again) ||
      std::memcmp(&handle, &handle_again, sizeof(handle)) != 0 || !word(reader, handle.control + 28, generation_again) ||
      generation_again != generation || !word(reader, handle.control, second) || second != cached || !word(reader, cached, second) ||
      second != vptr)
    return false;
  if (runtime.owned_control && runtime.owned_control != handle.control)
    return false;
  if (!queries.finish())
    return false;
  runtime.manager = cached;
  runtime.renderer = renderer;
  runtime.control = handle.control;
  runtime.token = {cached, static_cast<std::uint64_t>(generation) + 1};
  return true;
}

bool capture_pose(Runtime& runtime) {
  runtime.pose_captured = false;
  runtime.pose_busy = false;
  runtime.mounted_poses = {};
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
    LocalImageReader image(reinterpret_cast<HMODULE>(runtime.base), runtime.image.image_size);
    std::uint64_t source = 0;
    const auto aircraft = inspected(runtime, [&] {
      return discovery::inspect_aircraft_metadata(image, objects, runtime.image, runtime.base, 133538936, true, true, false, &source);
    });
    if (!aircraft.valid || !aircraft.available || !source) {
      runtime.message = "Aircraft body calibration is waiting for a stable loaded source: " + aircraft.stage + ". " + aircraft.error;
      return false;
    }
    objects.reset_budget();
    Vector3 position{};
    const auto camera = inspected(runtime, [&] { return inspect_source_pose(objects, source, &position); });
    if (!camera.complete || !calibrate_body_pose(position, camera.fov, GetTickCount64())) {
      runtime.message = "Aircraft body calibration is waiting for matching fresh public and private camera coordinates.";
      return false;
    }
    body = sample_body_pose(GetTickCount64());
  }
  if (!body.valid || !make_mounted_pair(body.pose, runtime.mounts, runtime.mounted_poses)) {
    runtime.pose_busy = temporary_pose_unavailable(body.error);
    runtime.message = std::string("Aircraft body pose unavailable: ") + body.error;
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

// Both IDs share one complete table walk and a fresh pool from this observer
// phase. Each view retains its own full trace reread and bounded-reader budget.
void inspect_pair(Runtime& runtime,
                  const std::array<ec::EntryId, 2>& ids,
                  const ec::ViewPoolSnapshot& pool,
                  ProbeSnapshot& report,
                  std::array<ec::OwnedViewSnapshot, 2>& views) {
  views = {};
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
  const auto ticket = timed(runtime, ProbeStage::handoff, [] { return scene_handoff().begin_capture(); });
  std::array<std::uint64_t, 2> resources{};
  if (!pool.valid) {
    refuse((runtime.inspection_changed && pool.status == ec::ViewPoolStatus::not_inspected) ||
                   pool.status == ec::ViewPoolStatus::read_failed || pool.status == ec::ViewPoolStatus::changed
               ? SceneStopReason::inspection_unavailable
               : SceneStopReason::identity_refused,
           "The current view pool could not be validated.");
    return;
  }
  LocalMemoryReader reader;
  const auto entries = timed(runtime, ProbeStage::entries,
                             [&] { return inspected(runtime, [&] { return ec::inspect_owned_entries(reader, runtime.manager, ids); }); });
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
    const auto view = timed(runtime, i == 0 ? ProbeStage::first_view : ProbeStage::second_view, [&] {
      return inspected(runtime, [&] { return ec::inspect_owned_view(reader, entries.entries[i].address, ids[i], pool); });
    });
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
      // Primary-view resizing can overwrite mode2 dimensions in the original
      // update. Stop instead of silently rendering at that larger resolution.
      // Reallocation is confined to new inactive views, never in-flight outputs.
      if (runtime.resized_ids[i] != ids[i] || runtime.resized_dimensions[i] != view.dimensions) {
        report.ready[i] = report.resource_present[i] = false;
        refuse(SceneStopReason::resolution_changed, "Owned-view resolution changed; retiring the pair before a guarded resize retry.");
      }
    }
  }
  if (report.ready[0] && report.ready[1])
    report.outputs_matched = timed(runtime, ProbeStage::handoff, [&] {
      return scene_handoff().publish(ticket, {runtime.token.identity, runtime.token.generation}, ids, resources);
    });
}

void apply_pose(Runtime& runtime, const ec::OwnedViewSnapshot& view, const MountedPose& pose) noexcept {
  using SetVector = void (*)(void*, const double*);
  function<SetVector>(runtime, 66324928)(reinterpret_cast<void*>(view.node_address), pose.position.data());
  function<SetVector>(runtime, 66859376)(reinterpret_cast<void*>(view.camera_address), pose.up.data());
  function<SetVector>(runtime, 66859344)(reinterpret_cast<void*>(view.camera_address), pose.target.data());
  function<void (*)(void*, float)>(runtime, 66859296)(reinterpret_cast<void*>(view.camera_address), pose.fov);
  function<void (*)(void*)>(runtime, 66825216)(reinterpret_cast<void*>(view.view_address));
}

void apply_gates(Runtime& runtime,
                 const std::array<ec::EntryId, 2>& ids,
                 const std::array<bool, 2>& desired,
                 const ProbeSnapshot& inspected,
                 bool initialize_gates) {
  using Activate = void (*)(void*, std::uint64_t, bool);
  const auto activate = function<Activate>(runtime, 17641776);
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

bool initialize(void* opaque, ec::DescriptorStorage& descriptor) noexcept {
  auto& runtime = *static_cast<Runtime*>(opaque);
  // Only a real creation request reaches this callback, after prior cleanup.
  // Stop requests cannot call the pose getters or descriptor initializer.
  if (runtime.creations == 0) {
    try {
      runtime.message.clear();
      runtime.stage_error = "";
      LocalMemoryReader reader;
      const auto pool = inspected(runtime, [&] { return ec::inspect_view_pool(reader, runtime.renderer); });
      runtime.creation_valid = pool.valid && pool.free_count >= 2;
      if (!runtime.creation_valid)
        runtime.stage_error = pool.valid ? "Two free engine views are required." : "The view pool could not be validated.";
      else if (!(runtime.creation_valid = capture_pose(runtime)))
        runtime.stage_error = "A verified aircraft body pose is required before creating mounted views.";
    } catch (...) {
      runtime.creation_valid = false;
    }
  }
  if (!runtime.creation_valid)
    return false;
  function<void (*)(void*)>(runtime, 17640240)(descriptor.bytes.data());
  return true;
}

ec::EntryId create(void* opaque, ec::ManagerToken token, const ec::DescriptorStorage& descriptor) noexcept {
  auto& runtime = *static_cast<Runtime*>(opaque);
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
  const auto id =
      function<std::uint64_t (*)(void*, const void*)>(runtime, 17646976)(reinterpret_cast<void*>(token.identity), descriptor.bytes.data());
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
    function<void (*)(void*, std::uint64_t, bool)>(runtime, 17641776)(reinterpret_cast<void*>(runtime.manager), id, false);
    view = inspect_entry(runtime, id);
    runtime.creation_valid = view.complete && view.ready && (view.flags[0] & 1u);
    if (!runtime.creation_valid) {
      runtime.stage_error = "The new owned view could not be validated with its render gate closed.";
      return id;
    }
    apply_pose(runtime, view, runtime.mounted_poses[runtime.creations - 1]);
    ViewDimensions desired{};
    if (!plan_view_resize(view.dimensions, runtime.creations - 1, desired, runtime.aircraft_profile->camera_panes)) {
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

bool resize_new_entry(Runtime& runtime, ec::EntryId id, unsigned index) {
  const auto view = inspect_entry(runtime, id);
  if (!view.complete || !view.ready || !(view.flags[0] & 1u) || index >= runtime.resized_dimensions.size()) {
    runtime.stage_error = "The owned view did not stay closed during its initial engine update.";
    return false;
  }
  const auto desired = runtime.resized_dimensions[index];
  const ViewResizeCallbacks resize_callbacks{
      &runtime,
      [](void* opaque, std::uint64_t address) noexcept {
        auto& current = *static_cast<Runtime*>(opaque);
        function<void (*)(void*)>(current, 66825216)(reinterpret_cast<void*>(address));
        return true;
      },
      [](void* opaque, std::uint64_t address) noexcept -> std::uint64_t {
        auto& current = *static_cast<Runtime*>(opaque);
        return reinterpret_cast<std::uintptr_t>(function<void* (*)(void*)>(current, 66809728)(reinterpret_cast<void*>(address)));
      }};
  const auto resized = resize_owned_view(view, index, desired, resize_callbacks, runtime.aircraft_profile->camera_panes);
  if (!resized.complete) {
    runtime.stage_error = view_resize_status_name(resized.status);
    return false;
  }
  const auto confirmed = inspect_entry(runtime, id);
  const bool valid = confirmed.complete && confirmed.ready && confirmed.view_address == view.view_address &&
                     confirmed.node_address == view.node_address && confirmed.camera_address == view.camera_address &&
                     confirmed.dimensions == desired && (confirmed.flags[0] & 1u);
  if (!valid) {
    runtime.stage_error = "The resized owned-view chain did not remain stable; its render gate stays closed.";
    return false;
  }
  runtime.resized_ids[index] = id;
  return true;
}

bool erase(void* opaque, ec::ManagerToken token, ec::EntryId id) noexcept {
  auto& runtime = *static_cast<Runtime*>(opaque);
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
    function<void (*)(void*, std::uint64_t, bool)>(runtime, 17641776)(reinterpret_cast<void*>(token.identity), id, false);
    return false;
  }
  if (action != ViewRetirement::Action::erase)
    return false;
  function<void (*)(void*, std::uint64_t)>(runtime, 17646000)(reinterpret_cast<void*>(token.identity), id);
  reader.reset_budget();
  entries = inspected(runtime, [&] { return ec::inspect_owned_entries(reader, token.identity, {id, 0}); });
  const bool absent = entries.complete && !entries.entries[0].found;
  if (absent)
    runtime.retirement.forget(token, id);
  return absent;
}

void record_stop(Runtime& runtime, SceneStopReason reason, const char* detail, std::uint64_t now) {
  const std::lock_guard lock(runtime.mutex);
  if (runtime.recovery.reason() == reason && runtime.stop_detail == detail && (runtime.recovery.pending() || !retryable_scene_stop(reason)))
    return;
  runtime.recovery.failed(reason, now);
  runtime.stop_detail = detail;
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
    std::uint64_t start_revision = 0;
    {
      const std::lock_guard lock(runtime.mutex);
      if (!before.owned_ids[0] && !before.owned_ids[1] && !before.creation_pending)
        runtime.aircraft_profile = runtime.requested_profile;
      requested_start = runtime.requested_start;
      start_revision = runtime.requested_start_revision;
      if (runtime.mount_revision != runtime.requested_mount_revision) {
        runtime.mounts = runtime.requested_mounts;
        runtime.mount_revision = runtime.requested_mount_revision;
      }
    }
    auto next_schedule = runtime.schedule;
    next_schedule.configure(settings & 0xffu, settings >> 8);
    std::array<bool, 2> desired{};
    const bool scheduled_pair = before.state == ec::State::active && runtime.scheduled_ids == before.owned_ids;
    if (scheduled_pair)
      desired = next_schedule.tick(now, runtime.suspended.load());
    const bool gate_change = scheduled_pair && desired != runtime.gates;
    if (!before.request_pending && !gate_change && !runtime.resize_warmup.pending() && now - runtime.last_inspection < 250) {
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
    const auto start_body = requested_start ? sample_body_pose(now) : BodyPoseSnapshot{};
    if (requested_start && !before.owned_ids[0] && !before.owned_ids[1] && !start_body.valid && !start_body.calibration_required) {
      // Public startup is asynchronous. Do not walk private manager, pool or
      // aircraft graphs repeatedly while its first telemetry is still pending.
      report.pair = before;
      report.message = std::string("Waiting for read-only aircraft telemetry: ") + start_body.error;
    } else if (!timed(runtime, ProbeStage::manager, [&] { return manager_context(runtime, manager); })) {
      scene_handoff().stop_scene();
      record_stop(runtime, SceneStopReason::identity_refused, "Manager/renderer identity did not pass its complete guard.", now);
      report.message = "Waiting for a stable manager/renderer lifetime; no native call made.";
      report.pair = before;
    } else {
      LocalMemoryReader reader;
      auto pool = timed(runtime, ProbeStage::pool,
                        [&] { return inspected(runtime, [&] { return ec::inspect_view_pool(reader, runtime.renderer); }); });
      report.free_views = pool.valid ? pool.free_count : 0;
      runtime.creations = 0;
      runtime.creation_valid = pool.valid && pool.free_count >= 2;
      ec::EngineCallbacks callbacks{&runtime, initialize, create, erase};
      timed(runtime, ProbeStage::lifecycle, [&] { runtime.pair.process_update(runtime.token, callbacks); });
      auto pair = runtime.pair.snapshot();
      if (!requested_start && !pair.owned_ids[0] && !pair.owned_ids[1]) {
        bool retry_pending = false;
        {
          const std::lock_guard lock(runtime.mutex);
          retry_pending = runtime.recovery.pending();
        }
        const bool fresh_pose = retry_pending && sample_body_pose(GetTickCount64()).valid;
        const std::lock_guard lock(runtime.mutex);
        if (!runtime.requested_start && runtime.recovery.retry(GetTickCount64(), pair, fresh_pose)) {
          runtime.requested_start = requested_start = true;
          start_revision = ++runtime.requested_start_revision;
        }
      }
      bool waiting_for_body = false;
      if (requested_start && !pair.owned_ids[0] && !pair.owned_ids[1]) {
        waiting_for_body = !timed(runtime, ProbeStage::pose, [&] { return capture_pose(runtime); });
        bool create_requested = false;
        if (!waiting_for_body) {
          // Stop and a new Start share this small mailbox transaction. A slow
          // calibration cannot resurrect an enable that the UI has cancelled.
          const std::lock_guard lock(runtime.mutex);
          if (runtime.requested_start && runtime.requested_start_revision == start_revision) {
            scene_handoff().begin_scene();
            runtime.pair.request_independent_pose();
            runtime.requested_start = false;
            create_requested = true;
          }
        }
        if (create_requested) {
          timed(runtime, ProbeStage::lifecycle, [&] { runtime.pair.process_update(runtime.token, callbacks); });
          pair = runtime.pair.snapshot();
          if (pair.state != ec::State::active)
            record_stop(runtime, SceneStopReason::creation_failed,
                        *runtime.stage_error ? runtime.stage_error : "Owned camera creation did not complete.", now);
        }
      }
      if (pair.state == ec::State::active) {
        // Creation may claim previously free slots. Only that transition needs
        // another pool snapshot; an existing pair reuses the one above.
        if (runtime.creations != 0) {
          reader.reset_budget();
          pool = timed(runtime, ProbeStage::pool,
                       [&] { return inspected(runtime, [&] { return ec::inspect_view_pool(reader, runtime.renderer); }); });
          report.free_views = pool.valid ? pool.free_count : 0;
          runtime.creation_valid = runtime.creation_valid && runtime.resize_warmup.begin(pair.owned_ids, runtime.updates);
          runtime.gates = {};
        }
        const bool closed_warmup = runtime.creations != 0 && runtime.creation_valid && runtime.resize_warmup.pending();
        if (!closed_warmup && runtime.resize_warmup.pending()) {
          runtime.creation_valid = timed(runtime, ProbeStage::lifecycle, [&] {
            return runtime.resize_warmup.may_resize(pair.owned_ids, runtime.updates) && resize_new_entry(runtime, pair.owned_ids[0], 0) &&
                   resize_new_entry(runtime, pair.owned_ids[1], 1);
          });
          if (runtime.creation_valid)
            runtime.resize_warmup.finish(pair.owned_ids, runtime.updates);
        }
        std::array<ec::OwnedViewSnapshot, 2> views{};
        if (!closed_warmup)
          inspect_pair(runtime, pair.owned_ids, pool, report, views);
        const bool wait_for_views = runtime.view_wait.observe(now, pair,
                                                              !runtime.resize_warmup.pending() && runtime.scheduled_ids == pair.owned_ids &&
                                                                  runtime.resized_ids == pair.owned_ids &&
                                                                  runtime.inspection_stop == SceneStopReason::inspection_unavailable,
                                                              views);
        if (closed_warmup) {
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
          if (pose_ready) {
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
                  function<void (*)(void*, std::uint64_t, bool)>(runtime, 17641776)(reinterpret_cast<void*>(runtime.manager),
                                                                                    pair.owned_ids[i], false);
                runtime.gates[i] = false;
              }
          });
          runtime.schedule = next_schedule;
          report.view_waiting = true;
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
      if (body_pose_failed || waiting_for_body)
        report.message = runtime.message.empty() ? runtime.stage_error : runtime.message;
      else if (report.view_waiting)
        report.message = "Owned view temporarily pending; waiting up to one second while retaining the last valid camera image.";
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
      report.accepting_requests = runtime.recovery.requested();
      report.recovery_pending = runtime.recovery.pending();
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
      const auto contract = verify_code_contract(reader, runtime.image, verified_profile());
      if (!runtime.image.valid_image || !contract.valid) {
        const std::lock_guard lock(runtime.mutex);
        runtime.published.message =
            "Camera compatibility check failed: " + (runtime.image.valid_image ? contract.error : runtime.image.error);
        return;
      }
      const auto disable_mask = inspect_activation_disable_mask(reader, runtime.image);
      if (!disable_mask.valid)
        throw std::runtime_error("Native activation-mask verification refused: " + disable_mask.error);
      std::uint64_t original = 0;
      if (!reader.read(kUpdateSlot, &original, sizeof(original)) || original != runtime.base + kUpdate)
        throw std::runtime_error("Manager update slot does not match the verified profile.");
      HMODULE pinned = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&observer),
                              &pinned))
        throw std::runtime_error("Pinning the observer module failed.");
      // This build declares the slot as read-only, non-executable image data,
      // while the Xbox loader maps its page executable/write-copy. The hook
      // rechecks the bounds and preserves execute permission through Windows'
      // copy-on-write promotion. Anonymous/executable PE sections are refused.
      const auto section = std::find_if(runtime.image.sections.begin(), runtime.image.sections.end(), [](const auto& item) {
        return (item.flags & 0x40000000u) && !(item.flags & 0xa2000000u) && kUpdateSlot >= item.rva &&
               kUpdateSlot - item.rva <= item.size && sizeof(void*) <= item.size - (kUpdateSlot - item.rva);
      });
      if (section == runtime.image.sections.end())
        throw std::runtime_error("Camera compatibility check failed: manager update slot moved outside static image data.");
      const engine_hook::ImageDataSlotProof image_data{reinterpret_cast<HMODULE>(runtime.base), runtime.image.image_size, section->rva,
                                                       section->size};
      const auto hook = engine_hook::install(reinterpret_cast<void**>(runtime.base + kUpdateSlot),
                                             reinterpret_cast<void*>(runtime.base + kUpdate), observer, &image_data);
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
    runtime.pair.request_disable();
    runtime.recovery.start();
    runtime.stop_detail.clear();
    runtime.requested_start = true;
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

void request_scene_stop(bool keep_telemetry) noexcept {
  auto& runtime = state();
  const std::lock_guard start_lock(runtime.start_mutex);
  {
    const std::lock_guard lock(runtime.mutex);
    runtime.requested_start = false;
    runtime.published.accepting_requests = false;
    ++runtime.requested_start_revision;
    runtime.recovery.stop();
    runtime.stop_detail = "Explicit camera OFF/Stop requested.";
    scene_handoff().stop_scene();
    runtime.pair.request_disable();
  }
  if (!keep_telemetry)
    shutdown_body_pose_provider();
}

bool request_capture_recovery() noexcept {
  auto& runtime = state();
  const std::lock_guard start_lock(runtime.start_mutex);
  const std::lock_guard lock(runtime.mutex);
  if (!runtime.recovery.requested() || runtime.recovery.pending() || runtime.requested_start ||
      runtime.recovery.attempts() >= SceneRecovery::maximum_retries ||
      (runtime.recovery.reason() != SceneStopReason::none && !retryable_scene_stop(runtime.recovery.reason())) ||
      runtime.pair.snapshot().state != ec::State::active)
    return false;
  runtime.recovery.failed(SceneStopReason::capture_stalled, GetTickCount64());
  runtime.stop_detail = "Source draws continued but capture state stayed unknown; retiring owned views for fresh allocation.";
  scene_handoff().stop_scene();
  runtime.pair.request_disable();
  return true;
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
  const auto settings = std::clamp(rate, 15u, 60u) | (std::clamp(feeds, 1u, 2u) << 8);
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
  result.recovery_pending = runtime.recovery.pending();
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
