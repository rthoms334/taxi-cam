#include <algorithm>
#include <atomic>
#include <cstring>
#include "../camera/body_pose_provider.hpp"
#include "../camera/mount_config.hpp"
#include "../camera/probe.hpp"
#include "../graphics/capture_progress.hpp"
#include "../graphics/display_exposure.hpp"
#include "../graphics/taxi_button_routes.hpp"
#include "../hooks/render_boundary_observer.hpp"
#include "../shared/companion_control.hpp"
#include "../shared/protocol.hpp"
#include "../shared/rotating_log.hpp"
#include "../shared/scene_demand.hpp"
#include "camera_status.hpp"
#include "crash_evidence.hpp"
#include "d3d12_bridge.hpp"
#include "native_hooks.hpp"

namespace {
using namespace taxi_camera;
namespace win = standalone;
std::atomic<bool> started{};
void log_status(const win::Status& s, const char* detail = "") noexcept {
  try {
    wchar_t directory[32768]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, 32768);
    if (!n || n >= 32700)
      return;
    std::wstring path(directory);
    path += L"\\Taxi Cam";
    CreateDirectoryW(path.c_str(), nullptr);
    path += L"\\bridge.log";
    char line[2048];
    const auto length = std::snprintf(
        line, sizeof(line),
        "%llu pid=%lu tid=%lu native=%u scene=%u mask=%u left=%llu right=%llu captured=%llu composed=%llu stamps=%llu "
        "hooks_failed=%llu cutoff=%u | %s | %s\r\n",
        static_cast<unsigned long long>(GetTickCount64()), GetCurrentProcessId(), GetCurrentThreadId(), s.graphics_ready, s.scene_ready,
        s.taxi_mask, static_cast<unsigned long long>(s.left_id), static_cast<unsigned long long>(s.right_id),
        static_cast<unsigned long long>(s.captures), static_cast<unsigned long long>(s.composed), static_cast<unsigned long long>(s.stamps),
        static_cast<unsigned long long>(s.hook_failures), s.speed_inhibited, s.message, detail);
    if (length > 0 && static_cast<size_t>(length) < sizeof(line))
      win::append_rotating_log(path, std::string_view(line, static_cast<std::size_t>(length)), win::BridgeLogBytes);
  } catch (...) {
    // Diagnostics must not interrupt bridge operation.
  }
}
struct StartupTiming {
  unsigned intent_mask{}, attempts{};
  bool observed{}, target_ready{}, output_ready{}, stamped{}, waiting_logged{};
  std::uint64_t intent_ms{}, target_ms{}, prepare_begin_ms{}, prepare_end_ms{}, request_begin_ms{}, request_end_ms{};
  std::uint64_t output_ms{}, stamp_ms{}, baseline_stamps{};
};
void log_startup(const win::Status& status, const StartupTiming& timing, const char* phase) {
  char detail[768];
  std::snprintf(detail, sizeof(detail),
                "Startup phase=%s intent_mask=%u attempt=%u intent_observed_ms=%llu targets_observed_ms=%llu "
                "prepare_begin_ms=%llu prepare_end_ms=%llu prepare_duration_ms=%llu "
                "request_begin_ms=%llu request_end_ms=%llu request_duration_ms=%llu "
                "output_observed_ms=%llu stamp_observed_ms=%llu intent_to_output_ms=%llu intent_to_stamp_ms=%llu",
                phase, timing.intent_mask, timing.attempts, static_cast<unsigned long long>(timing.intent_ms),
                static_cast<unsigned long long>(timing.target_ms), static_cast<unsigned long long>(timing.prepare_begin_ms),
                static_cast<unsigned long long>(timing.prepare_end_ms),
                static_cast<unsigned long long>(timing.prepare_end_ms ? timing.prepare_end_ms - timing.prepare_begin_ms : 0),
                static_cast<unsigned long long>(timing.request_begin_ms), static_cast<unsigned long long>(timing.request_end_ms),
                static_cast<unsigned long long>(timing.request_end_ms ? timing.request_end_ms - timing.request_begin_ms : 0),
                static_cast<unsigned long long>(timing.output_ms), static_cast<unsigned long long>(timing.stamp_ms),
                static_cast<unsigned long long>(timing.output_ready ? timing.output_ms - timing.intent_ms : 0),
                static_cast<unsigned long long>(timing.stamped ? timing.stamp_ms - timing.intent_ms : 0));
  log_status(status, detail);
}
DWORD run_impl() {
  win::Mailbox mailbox;
  if (!mailbox.open(GetCurrentProcessId(), false))
    return ERROR_INVALID_DATA;
  win::Status status{};
  log_status(status, "Bridge worker started.");
  const bool fault_evidence_ready = win::crash_evidence::initialize();
  log_status(status, fault_evidence_ready ? "Renderer fault evidence armed." : "Renderer fault evidence unavailable.");
  log_status(status, "Native graphics initialization started.");
  if (!win::initialize_graphics()) {
    const auto graphics = win::graphics_status();
    std::snprintf(status.message, sizeof(status.message), "Native graphics refused: %s", graphics.error);
    status.hook_failures = graphics.hook_failures;
    status.heartbeat = GetTickCount64();
    if (mailbox.lock(100)) {
      mailbox.data()->status = status;
      mailbox.unlock();
    }
    log_status(status);
    return ERROR_NOT_SUPPORTED;
  }
  log_status(status, "Native graphics initialization completed.");
  const auto key = win::graphics_status().device;
  wchar_t gpu_timing_option[2]{};
  const bool gpu_timing = GetEnvironmentVariableW(L"TAXI_CAM_GPU_TIMING", gpu_timing_option, 2) == 1 && gpu_timing_option[0] == L'1';
  scene_runtime::set_gpu_timing_enabled(gpu_timing);
  wchar_t graphics_diagnostics_option[2]{};
  const bool graphics_diagnostics = GetEnvironmentVariableW(L"TAXI_CAM_GRAPHICS_DIAGNOSTICS", graphics_diagnostics_option, 2) == 1 &&
                                    graphics_diagnostics_option[0] == L'1';
  win::set_graphics_diagnostics_enabled(graphics_diagnostics);
  win::set_graphics_observation_demand(false);
  // The Windows companion owns mount settings for native sessions.
  TaxiButtonIntent intent;
  DisplayExposureController exposure;
  CaptureProgress progress;
  StartupTiming startup, warmup_startup;
  win::ScenePrewarm prewarm;
  std::uint64_t next_background_start = 0;
  bool requested = false, failed = false, last_output = false;
  std::uint64_t last_view_wait_count = 0;
  std::uint64_t next_telemetry{}, next_discovery{}, next_recovery{}, next_log{}, route_request{}, last_frames{};
  std::uint64_t next_inventory{};
  std::uint64_t discovery_max_ms{}, service_max_ms{}, loop_max_ms{};
  const auto service_scene = [&] {
    const auto begin = GetTickCount64();
    win::service_display_patches();
    scene_runtime::service();
    service_max_ms = std::max(service_max_ms, GetTickCount64() - begin);
  };
  std::vector<PfdTargetObservation> inventory;
  unsigned rate{}, feeds{}, applied_profile{};
  std::uint64_t applied_profile_request{}, applied_session_epoch{};
  std::uint64_t pending_profile_request{}, pending_session_epoch{}, transition_token{};
  unsigned pending_profile{};
  bool changing_profile = false;
  bool pending_full_reset = false;
  bool pending_telemetry_selected = false;
  std::uint64_t pending_gpu_generation{};
  win::CompanionControl control;
  win::CompanionSetupSession connection_session;
  std::uint64_t applied_connection{}, pending_connection{};
  win::Status last_logged{};
  bool logged = false, last_connected = false, last_requested = false;
  std::uint64_t last_stop_sequence{};
  std::array<std::array<double, 6>, 2> applied_mounts{};
  for (;;) {
    control.refresh(mailbox);
    const auto now = GetTickCount64();
    const auto& settings = control.settings();
    const auto owner_pid = control.owner_pid();
    const bool connected = control.connected(now);
    const auto connection = connection_session.observe(connected, settings.enabled != 0, control.owner_pid(), settings.profile_request);
    if (connection.stopped) {
      win::set_target_mask(0);
      win::set_calibration(0, settings.calibration_budget);
      win::set_graphics_observation_demand(false);
      native_camera::suspend_scene_rendering(true);
      // The observer closes/revalidates owned views and cancels uncreated
      // requests. Keep the native lifetime transaction, including in-flight GPU
      // leases; reconnect will start a fresh setup token over these live objects.
      if (!pending_full_reset)
        native_camera::request_scene_profile_transition(applied_profile ? applied_profile : settings.profile);
      scene_runtime::manager().stop_source_tracking();
      scene_handoff().stop_scene();
      scene_runtime::reset_feed(key);
      changing_profile = false;
      transition_token = 0;
      requested = failed = false;
      intent = {};
      progress = {};
      prewarm = {};
      startup = warmup_startup = {};
      route_request = 0;
      log_status(status, "Connection stopped: camera output closed; next Connect will rescan and set up again.");
    }
    const auto session_epoch = native_camera::get_aircraft_session_epoch();
    const auto session = native_camera::get_aircraft_session_readiness();
    const bool session_settings = settings.aircraft_session_epoch == session_epoch;
    const bool command_context = connected && session_settings && session.ready && settings.enabled && !changing_profile &&
                                 !connection.started && connection.generation == applied_connection &&
                                 settings.profile == applied_profile && settings.profile_request == applied_profile_request &&
                                 session_epoch == applied_session_epoch && native_camera::aircraft_matches_profile();
    native_camera::update_taxi_button_request(
        {settings.taxi_request, settings.aircraft_session_epoch, settings.profile, settings.taxi_selected_mask, settings.taxi_desired_mask},
        command_context);
    if (connected && settings.enabled &&
        (changing_profile || connection.started || connection.generation != applied_connection || settings.profile != applied_profile ||
         settings.profile_request != applied_profile_request || session_epoch != applied_session_epoch)) {
      if (!changing_profile || settings.profile != pending_profile || settings.profile_request != pending_profile_request ||
          session_epoch != pending_session_epoch || connection.generation != pending_connection) {
        const bool full_reset =
            pending_full_reset || (applied_profile && (settings.profile != applied_profile || session_epoch != applied_session_epoch));
        win::set_target_mask(0);
        win::set_calibration(0, settings.calibration_budget);
        win::set_graphics_observation_demand(false);
        native_camera::suspend_scene_rendering(true);
        scene_runtime::manager().stop_source_tracking();
        scene_handoff().stop_scene();
        scene_runtime::reset_feed(key);
        if (full_reset) {
          // Forget discovery and completed frames, but keep every outstanding
          // GPU lease on its existing fence. Native retirement is observer-only.
          win::reset_display_session();
          pending_gpu_generation = scene_runtime::reset_session(key);
        }
        pending_full_reset = full_reset;
        pending_profile = settings.profile;
        pending_profile_request = settings.profile_request;
        pending_session_epoch = session_epoch;
        pending_connection = connection.generation;
        transition_token = 0;
        pending_telemetry_selected = false;
        changing_profile = true;
        requested = failed = false;
        route_request = 0;
      }
      native_camera::suspend_scene_rendering(true);
      if (pending_full_reset && !pending_gpu_generation)
        pending_gpu_generation = scene_runtime::reset_session(key);
      if (!transition_token)
        transition_token = pending_full_reset ? native_camera::request_scene_session_reset(pending_profile)
                                              : native_camera::request_scene_profile_transition(pending_profile);
      const auto transition = native_camera::scene_snapshot();
      const bool ready = transition_token && transition.profile_transition_token == transition_token &&
                         transition.profile_transition_id == pending_profile && transition.profile_transition_ready &&
                         !transition.profile_transition_pending && !transition.profile_transition_failed;
      if (ready && !pending_telemetry_selected)
        pending_telemetry_selected = native_camera::select_aircraft_profile(pending_profile);
      const bool telemetry_ready = pending_telemetry_selected;
      const auto transition_session = native_camera::get_aircraft_session_readiness();
      const bool public_ready = transition_session.ready && transition_session.epoch == pending_session_epoch;
      const bool gpu_ready =
          ready && telemetry_ready && public_ready &&
          (!pending_full_reset || (pending_gpu_generation && scene_runtime::resume_session(key, pending_gpu_generation)));
      if (!ready || !telemetry_ready || !public_ready || !gpu_ready) {
        const auto identity = native_camera::get_aircraft_identity();
        const auto graphics = win::graphics_status();
        win::Status pending{};
        pending.heartbeat = now;
        pending.graphics_ready = graphics.ready;
        pending.hook_failures = graphics.hook_failures;
        pending.probe_cpu_ms = transition.observer_last_ms;
        pending.probe_max_ms = transition.observer_max_ms;
        pending.aircraft_session_epoch = session_epoch;
        pending.active_profile = applied_profile;
        pending.detected_profile = identity.fresh ? identity.detected_profile : 0;
        pending.identity_sample_ms = identity.fresh ? identity.sample_ms : 0;
        std::memcpy(pending.aircraft_type, identity.type.data(), sizeof(pending.aircraft_type));
        std::memcpy(pending.aircraft_path, identity.path.data(), sizeof(pending.aircraft_path));
        std::snprintf(pending.message, sizeof(pending.message), "%s",
                      !ready             ? camera_transition_message(transition)
                      : !telemetry_ready ? "Waiting for aircraft telemetry to stop."
                      : !public_ready    ? "Waiting for the flight to finish loading and fresh camera telemetry."
                                         : "Waiting for camera GPU session reset.");
        if (mailbox.lock()) {
          mailbox.data()->status = pending;
          mailbox.unlock();
        }
        if (now >= next_log) {
          char detail[512]{};
          std::snprintf(detail, sizeof(detail),
                        "Aircraft transition waiting: token=%llu session=%llu profile=%u failed=%u request_pending=%u "
                        "creation_pending=%u telemetry_pending=%u entries=%llu/%llu created_total=%llu "
                        "full_reset=%u gpu_generation=%llu public_ready=%u loading=%u flow_subscribed=%u flow=%u reason=%s",
                        static_cast<unsigned long long>(transition_token), static_cast<unsigned long long>(session_epoch), pending_profile,
                        transition.profile_transition_failed, transition.pair.request_pending, transition.pair.creation_pending,
                        ready && !telemetry_ready, static_cast<unsigned long long>(transition.pair.owned_ids[0]),
                        static_cast<unsigned long long>(transition.pair.owned_ids[1]),
                        static_cast<unsigned long long>(transition.created_total), pending_full_reset,
                        static_cast<unsigned long long>(pending_gpu_generation), public_ready, transition_session.loading,
                        transition_session.flow_subscribed, transition_session.last_flow_event, transition_session.error);
          log_status(pending, detail);
          next_log = now + 5000;
        }
        service_scene();
        if (now >= next_telemetry) {
          native_camera::initialize_body_pose_provider();
          next_telemetry = now + 2000;
        }
        Sleep(25);
        continue;
      }
      win::set_aircraft_profile(pending_profile);
      prewarm = {};
      warmup_startup = {};
      rate = feeds = 0;
      applied_profile = pending_profile;
      applied_profile_request = pending_profile_request;
      applied_session_epoch = pending_session_epoch;
      applied_connection = pending_connection;
      char transition_detail[384]{};
      std::snprintf(
          transition_detail, sizeof(transition_detail),
          "Aircraft transition: token=%llu session=%llu profile=%u entries=%llu/%llu connection=%llu full_reset=%u gpu_generation=%llu "
          "public_ready=%u loading=%u flow_subscribed=%u flow=%u",
          static_cast<unsigned long long>(transition_token), static_cast<unsigned long long>(applied_session_epoch), applied_profile,
          static_cast<unsigned long long>(transition.pair.owned_ids[0]), static_cast<unsigned long long>(transition.pair.owned_ids[1]),
          static_cast<unsigned long long>(applied_connection), pending_full_reset, static_cast<unsigned long long>(pending_gpu_generation),
          transition_session.ready, transition_session.loading, transition_session.flow_subscribed, transition_session.last_flow_event);
      log_status(status, transition_detail);
      applied_mounts = {};
      intent = {};
      progress = {};
      startup = {};
      next_background_start = 0;
      exposure = {};
      next_telemetry = next_discovery = 0;
      next_inventory = 0;
      inventory.clear();
      changing_profile = false;
      pending_full_reset = false;
      pending_gpu_generation = 0;
    }
    if (now >= next_telemetry) {
      native_camera::initialize_body_pose_provider();
      next_telemetry = now + 2000;
    }
    if (now >= next_discovery) {
      const auto begin = GetTickCount64();
      win::discover_pfds(settings.auto_detect != 0 ? now : 0);
      discovery_max_ms = std::max(discovery_max_ms, GetTickCount64() - begin);
      next_discovery = now + 1000;
    }
    if (connected && settings.enabled && session_settings && settings.route_request && settings.route_request != route_request) {
      if (win::assign_targets(settings.left_id, settings.right_id))
        route_request = settings.route_request;
    }
    const auto identity = native_camera::get_aircraft_identity();
    const bool aircraft_matches =
        native_camera::aircraft_matches_profile() && (!settings.auto_profile || identity.detected_profile == settings.profile);
    const auto* profile = profiles::find(settings.profile);
    const bool manual_only = profile && profile->taxi_control == profiles::TaxiControl::manual_only;
    const auto buttons = native_camera::get_taxi_buttons();
    const auto cutoff = native_camera::get_taxi_cutoff();
    const auto desired = intent.observe(now, buttons.valid, buttons.left_on, buttons.right_on);
    const unsigned mask =
        connected && session_settings && session.ready && settings.enabled && aircraft_matches && win::graphics_ready() && !cutoff.inhibited
            ? (settings.follow_taxi && !manual_only ? desired.buttons
               : session_settings                   ? settings.manual_mask
                                                    : 0)
            : 0;
    const bool test_scene =
        connected && session_settings && session.ready && settings.enabled && aircraft_matches && settings.scene_test && !cutoff.inhibited;
    const auto intent_observed_ms = GetTickCount64();
    if (!mask && !test_scene && !prewarm.active())
      failed = false;
    const auto targets = win::target_ids();
    const unsigned assigned = (targets[0] ? 1u : 0u) | (targets[1] ? 2u : 0u);
    const auto speed = native_camera::get_ground_speed();
    const auto setup_current = [&]() {
      const auto epoch = native_camera::get_aircraft_session_epoch();
      const auto current_session = native_camera::get_aircraft_session_readiness();
      return current_session.ready && current_session.epoch == epoch && control.connected(GetTickCount64()) && settings.enabled &&
             control.owner_pid() == owner_pid && connection.generation == applied_connection && settings.aircraft_session_epoch == epoch &&
             epoch == applied_session_epoch && settings.profile == applied_profile && settings.profile_request == applied_profile_request;
    };
    const auto warm_readiness = [&]() {
      const auto sampled_now = GetTickCount64();
      const auto ground = native_camera::get_on_ground();
      const auto velocity = native_camera::get_ground_speed();
      const auto pose = native_camera::sample_body_pose(sampled_now);
      const auto aircraft = native_camera::get_aircraft_identity();
      const auto current_epoch = native_camera::get_aircraft_session_epoch();
      return win::ScenePrewarmReadiness{
          control.connected(GetTickCount64()),
          setup_current() && settings.aircraft_session_epoch == current_epoch && current_epoch == applied_session_epoch &&
              settings.profile == applied_profile && settings.profile_request == applied_profile_request,
          settings.enabled != 0,
          native_camera::aircraft_matches_profile() && aircraft.fresh && aircraft.detected_profile == settings.profile,
          win::graphics_ready(),
          native_camera::get_taxi_cutoff().inhibited,
          manual_only || native_camera::get_taxi_buttons().valid,
          pose.valid || pose.calibration_required,
          ground.valid,
          ground.on_ground,
          velocity.valid,
          settings.calibration_mask != 0 || settings.single_camera != 0,
          velocity.knots,
          native_camera::get_aircraft_session_readiness().ready};
    };
    bool background_warmup = false;
    if (prewarm.pending()) {
      const auto warm_scene = native_camera::scene_snapshot();
      const auto warm_output = scene_runtime::snapshot(key);
      const auto previous_warm_phase = prewarm.phase();
      background_warmup = prewarm.observe(
          now, warm_readiness().eligible(), mask || test_scene,
          {requested && warm_scene.ready[0] && warm_scene.ready[1], warm_output.output, warm_output.frames, warm_output.completed_frames},
          failed || warm_output.failed || warm_scene.pair.state == engine_camera::State::failed ||
              warm_scene.pair.state == engine_camera::State::blocked);
      if (prewarm.phase() != previous_warm_phase) {
        char detail[384]{};
        std::snprintf(
            detail, sizeof(detail),
            "Prewarm phase=%s elapsed_ms=%llu entries=%llu/%llu created_total=%llu output=%u completed_pairs=%llu/%llu", prewarm.name(),
            static_cast<unsigned long long>(prewarm.started_ms() && now >= prewarm.started_ms() ? now - prewarm.started_ms() : 0),
            static_cast<unsigned long long>(warm_scene.pair.owned_ids[0]), static_cast<unsigned long long>(warm_scene.pair.owned_ids[1]),
            static_cast<unsigned long long>(warm_scene.created_total), warm_output.output,
            static_cast<unsigned long long>(
                warm_output.completed_frames >= prewarm.baseline_pairs() ? warm_output.completed_frames - prewarm.baseline_pairs() : 0),
            static_cast<unsigned long long>(win::ScenePrewarm::RequiredPairs));
        log_status(status, detail);
      }
    }
    const auto demand = win::scene_demand(mask, test_scene, assigned, requested, failed, background_warmup);
    unsigned active = demand.stamp_mask;
    const unsigned calibration = connected && session_settings && session.ready && settings.enabled && aircraft_matches && !cutoff.inhibited
                                     ? settings.calibration_mask
                                     : 0;
    // Warmup and scene-only diagnostics need capture observation without PFD
    // writes. Settled OFF may bypass PFD state while lifetime tracking remains.
    win::set_graphics_observation_demand(!demand.suspend || calibration != 0);
    win::set_target_mask(active);
    win::set_calibration(calibration, settings.calibration_budget);
    const win::OwnedWork owned;
    if (connected && (rate != settings.camera_rate || feeds != (settings.single_camera ? 1u : 2u))) {
      rate = settings.camera_rate;
      feeds = settings.single_camera ? 1u : 2u;
      native_camera::request_scene_rate(rate, feeds);
      scene_runtime::manager().set_source_rate(rate);
    }
    if (connected && applied_mounts != settings.mounts) {
      native_camera::MountPair mounts;
      for (unsigned i = 0; i < 2; ++i) {
        const auto& m = settings.mounts[i];
        mounts[i] = {{m[0], m[1], m[2]}, m[3], m[4], static_cast<float>(m[5])};
      }
      if (native_camera::request_scene_mounts(mounts))
        applied_mounts = settings.mounts;
    }
    if (!startup.observed && (mask || test_scene)) {
      startup.observed = true;
      startup.intent_mask = mask & 3;
      startup.intent_ms = intent_observed_ms;
      startup.baseline_stamps = scene_runtime::snapshot(key).stamps;
      log_startup(status, startup, "accepted_intent");
    }
    if (startup.observed && !startup.target_ready && startup.intent_mask && (assigned & startup.intent_mask) == startup.intent_mask) {
      startup.target_ready = true;
      startup.target_ms = GetTickCount64();
      log_startup(status, startup, "targets_ready");
    }
    // Background warmup has zero display demand and closes after three GPU-completed
    // pairs or its bounded budget. Otherwise OFF, cutoff, pause and heartbeat
    // loss close the render gates immediately.
    // Keep the owned pair and ordered source-state evidence for the next ON.
    native_camera::suspend_scene_rendering(demand.suspend);
    auto composition = profiles::find(applied_profile ? applied_profile : settings.profile)->composition;
    composition.speed_color = settings.speed_color;
    composition.guide_color = settings.guide_color;
    composition.nose_dot = settings.nose_dot;
    composition.tail_upper = settings.tail_upper;
    composition.tail_corner = settings.tail_corner;
    composition.tail_inner = settings.tail_inner;
    if (demand.start && !(background_warmup && GetTickCount64() < next_background_start)) {
      auto& start_timing = background_warmup ? warmup_startup : startup;
      if (background_warmup) {
        start_timing.observed = true;
        start_timing.intent_ms = prewarm.started_ms();
      }
      ++start_timing.attempts;
      start_timing.prepare_begin_ms = GetTickCount64();
      start_timing.prepare_end_ms = start_timing.request_begin_ms = start_timing.request_end_ms = 0;
      log_startup(status, start_timing, "prepare_begin");
      const bool prepared = scene_runtime::prepare(key);
      start_timing.prepare_end_ms = GetTickCount64();
      log_startup(status, start_timing, prepared ? "prepare_ready" : "prepare_failed");
      // Shader preparation can outlast a Disconnect or a new companion owner.
      // Recheck the exact setup before either warmup or manual camera creation.
      control.refresh(mailbox);
      if (!setup_current()) {
        win::set_target_mask(0);
        win::set_calibration(0, settings.calibration_budget);
        win::set_graphics_observation_demand(false);
        native_camera::suspend_scene_rendering(true);
        continue;
      }
      bool warm_start_allowed = true;
      if (background_warmup) {
        // Preparation may compile shaders. Re-read the companion and public
        // session/ground evidence before queuing any native camera creation.
        const auto warm_output = scene_runtime::snapshot(key);
        warm_start_allowed = prewarm.observe(GetTickCount64(), warm_readiness().eligible(), false,
                                             {false, false, warm_output.frames, warm_output.completed_frames}, !prepared);
        if (!warm_start_allowed)
          log_status(status, "Prewarm preparation ended without a native request; eligibility or budget was lost.");
      }
      if (prepared && warm_start_allowed) {
        start_timing.request_begin_ms = GetTickCount64();
        log_startup(status, start_timing, "request_begin");
        scene_runtime::set_composition(key, composition);
        scene_runtime::reset_feed(key);
        scene_runtime::manager().begin_source_tracking();
        native_camera::request_scene_test(true);
        const auto shot = native_camera::scene_snapshot();
        requested = shot.accepting_requests;
        const bool retain = win::retain_background_prewarm(background_warmup, requested, shot.readiness_deferred);
        start_timing.request_end_ms = GetTickCount64();
        if (retain) {
          // The contract scan can outlast fresh telemetry. Do not park the only
          // background attempt; the same start is retried once readiness returns.
          next_background_start = start_timing.request_end_ms + 500;
          if (!start_timing.waiting_logged) {
            start_timing.waiting_logged = true;
            char phase[320];
            std::snprintf(phase, sizeof(phase), "request_waiting %.200s", shot.message.c_str());
            log_startup(status, start_timing, phase);
          }
        } else {
          next_background_start = 0;
          char phase[320];
          std::snprintf(phase, sizeof(phase), "%s %.200s", requested ? "request_accepted" : "request_refused",
                        requested ? "" : shot.message.c_str());
          log_startup(status, start_timing, phase);
        }
        if (!retain)
          failed = win::finish_scene_start(prewarm, background_warmup, requested);
      } else
        failed = win::finish_scene_start(prewarm, background_warmup, requested);
      if (!requested) {
        active = 0;
        win::set_target_mask(0);
        native_camera::suspend_scene_rendering(true);
        win::set_graphics_observation_demand(calibration != 0);
      }
    }
    // Normal button changes never call request_scene_stop/reset_feed or release
    // source leases. The profile-change transaction above still owns teardown.
    scene_runtime::set_composition(key, composition);
    const auto light = native_camera::get_lighting();
    const auto display = exposure.update(now, settings.exposure, settings.automatic_exposure != 0, settings.night_boost, light.valid,
                                         light.ambient, light.sample_ms);
    scene_runtime::set_display_exposure(key, display.applied_ev);
    scene_runtime::set_ground_speed(key, static_cast<float>(speed.knots), speed.valid);
    service_scene();
    const auto scene = native_camera::scene_snapshot();
    const auto output = scene_runtime::snapshot(key);
    if (scene.stop_sequence != last_stop_sequence) {
      progress.reset();
      scene_runtime::manager().rearm_source_states();
    }
    if (now >= next_recovery) {
      const bool eligible = (mask || test_scene) && !failed && !output.failed && scene.pair.state == engine_camera::State::active &&
                            scene.requested_feeds == 2 && !scene.pose_waiting && !scene.view_waiting &&
                            native_camera::sample_body_pose(now).valid;
      if (eligible && output.frames != last_frames)
        native_camera::note_scene_capture_progress(now);
      last_frames = output.frames;
      // Lost GPU-state evidence says nothing about native camera lifetime.
      // Keep the pair and its resource generations. AA/upscaler switches can
      // wipe source-state without new RT barriers; rearm retained RT models
      // so later draws can capture again. Never erase/recreate cameras here.
      if (progress.observe(now, eligible, output.frames, output.capture.source_draws,
                           std::strcmp(output.capture.tail_status, "unknown_source_state") == 0))
        scene_runtime::manager().rearm_source_states();
      next_recovery = now + 250;
    }
    const auto graphics = win::graphics_status();
    status = {};
    status.heartbeat = now;
    status.active_profile = applied_profile;
    status.detected_profile = identity.fresh ? identity.detected_profile : 0;
    status.identity_sample_ms = identity.fresh ? identity.sample_ms : 0;
    status.aircraft_session_epoch = session_epoch;
    // Read acknowledgement first: a retired command must never be paired with
    // the earlier rendering sample from before its lamp change was observed.
    const auto taxi_request_status = native_camera::get_taxi_button_request_status();
    const auto command_buttons = native_camera::get_taxi_buttons();
    status.taxi_buttons_valid = command_buttons.valid;
    status.taxi_buttons_mask = (command_buttons.left_on ? 1u : 0u) | (command_buttons.right_on ? 2u : 0u);
    status.taxi_buttons_sample_ms = command_buttons.sample_ms;
    status.taxi_request_seen = taxi_request_status.serial;
    status.taxi_request_retired = taxi_request_status.pending_mask ? 0 : taxi_request_status.serial;
    status.taxi_request_pending = taxi_request_status.pending_mask;
    status.taxi_request_failed = taxi_request_status.failed;
    std::memcpy(status.aircraft_type, identity.type.data(), sizeof(status.aircraft_type));
    std::memcpy(status.aircraft_path, identity.path.data(), sizeof(status.aircraft_path));
    status.graphics_ready = graphics.ready;
    status.hook_failures = graphics.hook_failures;
    status.scene_ready = scene.ready[0] && scene.ready[1];
    status.taxi_mask = active;
    status.speed_inhibited = cutoff.inhibited;
    status.left_id = targets[0];
    status.right_id = targets[1];
    status.speed = speed.valid ? static_cast<float>(speed.knots) : -1;
    status.exposure = display.applied_ev;
    status.probe_cpu_ms = scene.observer_last_ms;
    status.probe_max_ms = scene.observer_max_ms;
    // Preserve the existing ten wire-stage indices. The appended AA stage is
    // exposed in bridge diagnostics without changing the shared-memory ABI.
    static_assert(static_cast<std::size_t>(native_camera::ProbeStage::aa) == std::tuple_size_v<decltype(status.stage_ms)>);
    std::copy_n(scene.performance.stage_ms.begin(), status.stage_ms.size(), status.stage_ms.begin());
    status.captures = output.capture.captures;
    status.composed = output.frames;
    status.stamps = output.stamps;
    // These are polling observations, not GPU timestamps. They include all work
    // since accepted intent and do not redefine request duration as cold latency.
    if (startup.observed && requested && !startup.output_ready && output.output) {
      startup.output_ready = true;
      startup.output_ms = GetTickCount64();
      log_startup(status, startup, "first_output");
    }
    if (startup.observed && requested && !startup.stamped && output.stamps > startup.baseline_stamps) {
      startup.stamped = true;
      startup.stamp_ms = GetTickCount64();
      log_startup(status, startup, "first_stamp");
    }
    // Candidate rows are UI diagnostics, not the authoritative assignment or
    // discovery state. Avoid rebuilding the full inventory on every 25 ms tick.
    if (now >= next_inventory) {
      inventory = win::pfd_inventory();
      next_inventory = now + 1000;
      win::service_live_backfill(now, inventory.size());
    }
    status.candidate_count = static_cast<UINT>(std::min<size_t>(inventory.size(), 16));
    for (UINT i = 0; i < status.candidate_count; ++i)
      status.candidates[i] = {inventory[i].id,     inventory[i].draws,  inventory[i].width,
                              inventory[i].height, inventory[i].levels, inventory[i].format};
    char aircraft_message[sizeof(status.message)] = "Waiting for a supported aircraft identity or profile switch.";
    const auto* detected_profile = identity.fresh ? profiles::find(identity.detected_profile) : nullptr;
    const auto* selected_profile = profiles::find(applied_profile);
    if (!aircraft_matches && detected_profile && selected_profile && detected_profile != selected_profile)
      std::snprintf(aircraft_message, sizeof(aircraft_message), "Detected %ls; selected %ls. %s", detected_profile->name,
                    selected_profile->name,
                    settings.auto_profile ? "Waiting for Auto aircraft to switch profiles."
                                          : "Enable Auto aircraft or select the detected profile on Overview.");
    else if (!aircraft_matches)
      std::snprintf(aircraft_message, sizeof(aircraft_message), "%s Aircraft type: %.96s.",
                    identity.fresh ? "The loaded aircraft is not supported by the selected profile." : "Waiting for aircraft identity.",
                    identity.type[0] ? identity.type.data() : "unavailable");
    const char* target_message = graphics.ready && inventory.empty()
                                     ? "Waiting for cockpit displays to be drawn. They are learned on first use; Restart Flight if the "
                                       "list stays empty."
                                     : "Select the left and right displays, or wait for automatic assignment.";
    if (selected_profile && selected_profile->pfd_detection == profiles::PfdDetectionPolicy::ini_a380_allocation_group) {
      const auto is = [&](const char* reason) { return std::strcmp(graphics.target_detection, reason) == 0; };
      target_message = !settings.auto_detect        ? "Automatic PFD selection is off. Select the left and right displays manually."
                       : is("incomplete_inventory") ? "Display tracking was incomplete. Select the left and right PFDs manually."
                       : is("ini_group_incomplete") ? "Waiting for all eight iniBuilds A380 display textures."
                       : is("ini_group_ambiguous")
                           ? "Extra iniBuilds A380 display textures make automatic selection ambiguous. Select PFDs manually."
                       : is("ini_group_format")   ? "This iniBuilds A380 display format needs manual PFD selection."
                       : is("ini_group_inactive") ? "Waiting for all eight iniBuilds A380 displays to update."
                       : is("detected")           ? "PFD identities changed. Re-select the aircraft profile or choose the PFDs manually."
                                                  : "Checking the iniBuilds A380 display group across three active samples.";
    }
    const auto stopped_camera = camera_stop_message(scene);
    const char* message =
        !connected || !settings.enabled ? "Disconnected. Use Connect in the Windows companion."
        : !aircraft_matches             ? aircraft_message
        : cutoff.inhibited ? (manual_only ? "Above 60 knots: camera displays inhibited." : "Above 60 knots: TAXI buttons commanded off.")
        : failed                                         ? scene.message.c_str()
        : scene.pose_waiting && requested               ? scene.message.c_str()
        : scene.view_waiting && scene.stop_reason == native_camera::SceneStopReason::resolution_changed ? scene.message.c_str()
        : !manual_only && !buttons.valid                                                                ? buttons.error
        : !manual_only && taxi_request_status.failed && taxi_request_status.serial == settings.taxi_request
            ? "Aircraft TAXI-button change was not confirmed. Try the shortcut again."
        : (!targets[0] || !targets[1])         ? target_message
        : !active && settings.calibration_mask ? "Calibration requested on the selected display."
        : stopped_camera                       ? stopped_camera
        : background_warmup                    ? "Preparing camera views in the background; TAXI displays remain off."
        : !active && manual_only               ? "Ready. Use camera hotkeys or the left/right preview controls."
        : !active && !settings.follow_taxi     ? "Manual control selected. Enable a preview or TAXI buttons on Overview."
        : !active                              ? "Ready. Use the aircraft's left or right TAXI button."
        : !requested || failed                 ? scene.message.c_str()
        : progress.stalled()                   ? "Capture paused: waiting for verified GPU state; camera views retained."
        : output.output && !output.stamps      ? "Camera images ready; waiting for a verified PFD write opportunity."
                                               : output.message;
    std::snprintf(status.message, sizeof(status.message), "%s", message);
    if (mailbox.lock()) {
      mailbox.data()->status = status;
      mailbox.unlock();
    }
    const bool changed = !logged || connected != last_connected || requested != last_requested ||
                         status.taxi_mask != last_logged.taxi_mask || status.left_id != last_logged.left_id ||
                         status.right_id != last_logged.right_id || status.speed_inhibited != last_logged.speed_inhibited ||
                         scene.stop_sequence != last_stop_sequence || output.output != last_output ||
                         scene.view_wait_count != last_view_wait_count;
    loop_max_ms = std::max(loop_max_ms, GetTickCount64() - now);
    if (changed || now >= next_log) {
      if (!logged || status.active_profile != last_logged.active_profile || status.detected_profile != last_logged.detected_profile ||
          status.aircraft_session_epoch != last_logged.aircraft_session_epoch ||
          std::strcmp(status.aircraft_type, last_logged.aircraft_type) || std::strcmp(status.aircraft_path, last_logged.aircraft_path)) {
        char identity_detail[768];
        std::snprintf(identity_detail, sizeof(identity_detail),
                      "Aircraft identity: session=%llu selected=%u detected=%u fresh=%u type=%.255s path=%.259s",
                      static_cast<unsigned long long>(session_epoch), applied_profile, identity.detected_profile, identity.fresh,
                      identity.type.data(), identity.path.data());
        log_status(status, identity_detail);
      }
      char detail[1536];
      const auto boundaries = engine_hook::render_boundary::statistics();
      std::snprintf(detail, sizeof(detail),
                    "profile=%u matched=%u connected=%u requested=%u ipc_busy=%llu buttons_valid=%u held=%u expired=%u output=%u "
                    "stop_seq=%llu stop=%s "
                    "retry=%u pending=%u pose_wait=%u view_wait=%u waits=%llu ready=%u/%u inspection=%s/%s entries=%llu/%llu suspended=%u "
                    "gates=%u/%u tail=%s "
                    "draws=%llu unknown_lists=%llu invalid_recordings=%llu scoped_invalidations=%llu invalid_draws=%llu "
                    "lease_failures=%llu global_aliases=%llu overflows=%llu reasons=0x%x capture_stalled=%u "
                    "barrier_max=%llu barrier_truncated=%llu probe_ms=%.3f queries=%llu query_ms=%.3f read_ms=%.3f "
                    "allocation_queries=%llu page_queries=%llu region_queries=%llu aa_ms=%.3f "
                    "inspections=%llu updates=%llu clear_states=%llu | %.256s",
                    applied_profile, aircraft_matches, connected, requested, static_cast<unsigned long long>(control.busy_reads()),
                    buttons.valid, desired.held, desired.timed_out, output.output, static_cast<unsigned long long>(scene.stop_sequence),
                    native_camera::scene_stop_reason_name(scene.stop_reason), scene.recovery_attempts, scene.recovery_pending,
                    scene.pose_waiting, scene.view_waiting, static_cast<unsigned long long>(scene.view_wait_count), scene.ready[0],
                    scene.ready[1], scene.inspection_status[0], scene.inspection_status[1],
                    static_cast<unsigned long long>(scene.pair.owned_ids[0]), static_cast<unsigned long long>(scene.pair.owned_ids[1]),
                    demand.suspend, scene.gates[0], scene.gates[1], output.capture.tail_status,
                    static_cast<unsigned long long>(output.capture.source_draws),
                    static_cast<unsigned long long>(output.capture.unknown_submitted_lists),
                    static_cast<unsigned long long>(output.capture.invalid_source_recordings),
                    static_cast<unsigned long long>(output.capture.scoped_source_invalidations),
                    static_cast<unsigned long long>(output.capture.invalid_draws),
                    static_cast<unsigned long long>(output.capture.source_lease_failures),
                    static_cast<unsigned long long>(output.capture.global_aliases),
                    static_cast<unsigned long long>(output.capture.recording_overflows), output.capture.last_invalidation_reasons,
                    progress.stalled(), static_cast<unsigned long long>(boundaries.maximum_legacy_batch),
                    static_cast<unsigned long long>(boundaries.metadata_truncated_calls), scene.observer_last_ms,
                    static_cast<unsigned long long>(scene.performance.query_calls), scene.performance.query_ms, scene.performance.read_ms,
                    static_cast<unsigned long long>(scene.performance.query_allocation_calls),
                    static_cast<unsigned long long>(scene.performance.query_page_calls),
                    static_cast<unsigned long long>(scene.performance.query_fallback_calls),
                    scene.performance.stage_ms[static_cast<std::size_t>(native_camera::ProbeStage::aa)],
                    static_cast<unsigned long long>(scene.inspection_count), static_cast<unsigned long long>(scene.updates),
                    static_cast<unsigned long long>(graphics.clear_states),
                    scene.stop_reason != native_camera::SceneStopReason::none ? scene.stop_detail.c_str()
                    : !scene.pair.owned_ids[0] && !scene.pair.owned_ids[1]      ? scene.message.c_str()
                                                                                : "");
      log_status(status, detail);
      char selection_detail[256];
      std::snprintf(selection_detail, sizeof(selection_detail),
                    "PFD selection: automatic=%u candidates=%zu last_result=%s requested=%llu/%llu", settings.auto_detect, inventory.size(),
                    graphics.target_detection, static_cast<unsigned long long>(settings.left_id),
                    static_cast<unsigned long long>(settings.right_id));
      log_status(status, selection_detail);
      char control_detail[256];
      std::snprintf(control_detail, sizeof(control_detail),
                    "Control loop timing: discovery_max_ms=%llu service_max_ms=%llu observed_loop_max_ms=%llu",
                    static_cast<unsigned long long>(discovery_max_ms), static_cast<unsigned long long>(service_max_ms),
                    static_cast<unsigned long long>(loop_max_ms));
      log_status(status, control_detail);
      char candidate_detail[1024] = "PFD candidate activity (id:draws/completions):";
      std::size_t candidate_used = std::strlen(candidate_detail);
      for (std::size_t i = 0; i < std::min<std::size_t>(inventory.size(), 16); ++i) {
        const int written =
            std::snprintf(candidate_detail + candidate_used, sizeof(candidate_detail) - candidate_used, " %llu:%llu/%llu",
                          static_cast<unsigned long long>(inventory[i].id), static_cast<unsigned long long>(inventory[i].draws),
                          static_cast<unsigned long long>(inventory[i].submission_activity));
        if (written < 0 || static_cast<std::size_t>(written) >= sizeof(candidate_detail) - candidate_used)
          break;
        candidate_used += static_cast<std::size_t>(written);
      }
      log_status(status, candidate_detail);
      char pfd_detail[640];
      std::snprintf(pfd_detail, sizeof(pfd_detail),
                    "PFD copy admission: selected_draws=%llu rt_metadata=%llu rt_callbacks=%llu pending_matches=%llu "
                    "view_resolved=%llu view_rejected=%llu attempts=%llu rejected=%llu state_skips=%llu "
                    "boundary_batches_refused=%llu boundary_passes_refused=%llu queue_plans=%llu queue_copies=%llu reason=%s",
                    static_cast<unsigned long long>(graphics.selected_draws),
                    static_cast<unsigned long long>(graphics.selected_rt_metadata),
                    static_cast<unsigned long long>(graphics.selected_rt_callbacks),
                    static_cast<unsigned long long>(graphics.selected_pending_matches),
                    static_cast<unsigned long long>(graphics.selected_view_resolved),
                    static_cast<unsigned long long>(graphics.selected_view_rejected),
                    static_cast<unsigned long long>(graphics.copy_attempts), static_cast<unsigned long long>(graphics.copy_rejected),
                    static_cast<unsigned long long>(output.state_skips), static_cast<unsigned long long>(boundaries.batch_refusals),
                    static_cast<unsigned long long>(boundaries.pass_refusals), static_cast<unsigned long long>(graphics.queue_patch_plans),
                    static_cast<unsigned long long>(output.capture.display_copies), graphics.copy_error);
      log_status(status, pfd_detail);
      // Fixed-size counters only on submission threads; formatting and bounded
      // rotating-file output stay on this existing five-second control cadence.
      const auto log_counters = [&](const char* label, const auto& counters, const auto& name) {
        char detail[1280];
        auto used = static_cast<std::size_t>(std::snprintf(detail, sizeof(detail), "%s close_verified=%u proof_flags=%u:", label,
                                                           graphics.queue_close_verified, graphics.queue_last_proof_flags));
        for (unsigned i = 0; i < counters.size() && used < sizeof(detail); ++i) {
          if (!counters[i])
            continue;
          const auto written =
              std::snprintf(detail + used, sizeof(detail) - used, " %s=%llu", name(i), static_cast<unsigned long long>(counters[i]));
          if (written < 0 || static_cast<std::size_t>(written) >= sizeof(detail) - used)
            break;
          used += static_cast<std::size_t>(written);
        }
        log_status(status, detail);
      };
      log_counters("PFD queue admission", graphics.queue_outcomes,
                   [](unsigned i) { return win::display_submission_outcome_name(static_cast<win::DisplaySubmissionOutcome>(i)); });
      log_counters("PFD queue proof refusals", graphics.queue_proof_refusals,
                   [](unsigned i) { return win::PfdSubmissionProof::refusal_name(static_cast<win::PfdSubmissionProof::Refusal>(i)); });
      log_counters("PFD prefix blocking commands", graphics.queue_prefix_blockers,
                   [](unsigned i) { return win::pfd_gpu_operation_name(i); });
      char scopes[896]{};
      std::size_t used = 0;
      for (unsigned i = 0; i < graphics.selected_exit_scopes.size(); ++i) {
        if (!graphics.selected_exit_scopes[i])
          continue;
        const auto written = std::snprintf(scopes + used, sizeof(scopes) - used, "%s%u:%llu", used ? "," : "", i,
                                           static_cast<unsigned long long>(graphics.selected_exit_scopes[i]));
        if (written < 0 || static_cast<std::size_t>(written) >= sizeof(scopes) - used)
          break;
        used += static_cast<std::size_t>(written);
      }
      char scope_detail[1280];
      std::snprintf(scope_detail, sizeof(scope_detail),
                    "PFD scope: flags=count [%s] base=%llu nonbase=%llu split=%llu calibration_clears=%llu "
                    "follow_taxi=%u manual_mask=%u calibration_mask=%u",
                    scopes, static_cast<unsigned long long>(graphics.selected_exit_base),
                    static_cast<unsigned long long>(graphics.selected_exit_nonbase),
                    static_cast<unsigned long long>(graphics.selected_exit_split),
                    static_cast<unsigned long long>(graphics.calibration_clears), settings.follow_taxi, settings.manual_mask,
                    settings.calibration_mask);
      log_status(status, scope_detail);
      char draw_detail[384];
      std::snprintf(
          draw_detail, sizeof(draw_detail),
          "PFD guarded draw: attempts=%llu stamps=%llu query_refused=%llu state_refused=%llu "
          "recording_end=%llu deferred=%llu close_forward_refused=%llu",
          static_cast<unsigned long long>(graphics.fallback_attempts), static_cast<unsigned long long>(graphics.fallback_stamps),
          static_cast<unsigned long long>(graphics.fallback_query_refused),
          static_cast<unsigned long long>(graphics.fallback_state_refused), static_cast<unsigned long long>(graphics.recording_end_draws),
          static_cast<unsigned long long>(graphics.shader_deferred), static_cast<unsigned long long>(graphics.close_forward_refused));
      log_status(status, draw_detail);
      char retention_detail[384];
      std::snprintf(
          retention_detail, sizeof(retention_detail),
          "Camera retention: created_total=%llu snapshot_bytes=%llu quarantined=%llu prewarm=%s patch_requests=%u patch_draws=%llu "
          "retirement_deferrals=%llu retirement_waiting=%u retirement_status=%s retirement_queues=%u/%u",
          static_cast<unsigned long long>(scene.created_total), static_cast<unsigned long long>(output.capture.bytes),
          static_cast<unsigned long long>(output.capture.quarantined), prewarm.name(), output.patch_requests,
          static_cast<unsigned long long>(output.patch_draws), static_cast<unsigned long long>(scene.retirement_deferrals),
          scene.retirement_waiting, scene.retirement_status, scene.retirement_queue_counts[0], scene.retirement_queue_counts[1]);
      log_status(status, retention_detail);
      if (graphics_diagnostics) {
        char graphics_detail[512];
        std::snprintf(
            graphics_detail, sizeof(graphics_detail),
            "Graphics work: observing=%u epoch=%llu invalidations=%llu lookups=%llu cache_hits=%llu registry_lookups=%llu "
            "idle_setters=%llu idle_callbacks=%llu",
            graphics.observing, static_cast<unsigned long long>(graphics.observation_epoch),
            static_cast<unsigned long long>(graphics.observation_invalidations),
            static_cast<unsigned long long>(graphics.list_lookup_calls), static_cast<unsigned long long>(graphics.list_cache_hits),
            static_cast<unsigned long long>(graphics.list_registry_lookups), static_cast<unsigned long long>(graphics.idle_state_bypasses),
            static_cast<unsigned long long>(graphics.idle_callback_bypasses));
        log_status(status, graphics_detail);
      }
      if (output.gpu_timing_enabled) {
        const auto log_gpu_span = [&](const char* name, const GpuTimingStatistics& timing) {
          char gpu_detail[256];
          std::snprintf(gpu_detail, sizeof(gpu_detail), "GPU timing: stage=%s samples=%llu rejected=%llu total_ms=%.6f max_ms=%.6f", name,
                        static_cast<unsigned long long>(timing.samples), static_cast<unsigned long long>(timing.rejected), timing.total_ms,
                        timing.maximum_ms);
          log_status(status, gpu_detail);
        };
        log_gpu_span("private_capture_copy", output.capture.capture_copy_gpu);
        log_gpu_span("composition", output.composition_gpu);
        log_gpu_span("output_copy", output.output_copy_gpu);
        log_gpu_span("patches", output.patch_gpu);
      }
      char copy_detail[512];
      std::snprintf(copy_detail, sizeof(copy_detail),
                    "PFD boundary copy: attempts=%llu copies=%llu no_proof=%llu reason=%s | "
                    "dynamic_bias_calls=%llu dynamic_strip_calls=%llu sample_position_calls=%llu",
                    static_cast<unsigned long long>(graphics.preferred_copy_attempts),
                    static_cast<unsigned long long>(graphics.preferred_copy_stamps),
                    static_cast<unsigned long long>(graphics.preferred_copy_no_proof), graphics.preferred_copy_reason,
                    static_cast<unsigned long long>(graphics.dynamic_depth_bias_calls),
                    static_cast<unsigned long long>(graphics.dynamic_strip_cut_calls),
                    static_cast<unsigned long long>(graphics.sample_position_calls));
      log_status(status, copy_detail);
      if (!logged || status.active_profile != last_logged.active_profile || std::strcmp(status.aircraft_type, last_logged.aircraft_type) ||
          std::strcmp(status.aircraft_path, last_logged.aircraft_path)) {
        char identity_detail[640];
        std::snprintf(identity_detail, sizeof(identity_detail), "Aircraft identity: type=%.255s | path=%.259s | detected=%u",
                      status.aircraft_type, status.aircraft_path, status.detected_profile);
        log_status(status, identity_detail);
      }
      last_logged = status;
      last_connected = connected;
      last_requested = requested;
      last_stop_sequence = scene.stop_sequence;
      last_output = output.output;
      last_view_wait_count = scene.view_wait_count;
      logged = true;
      next_log = now + 5000;
    }
    Sleep(25);
  }
}
DWORD WINAPI run(void*) noexcept {
  try {
    return run_impl();
  } catch (...) {
    win::set_target_mask(0);
    win::set_calibration(0, 4096);
    win::set_graphics_observation_demand(false);
    scene_runtime::manager().stop_source_tracking();
    native_camera::request_scene_stop(true);
    return ERROR_NOT_ENOUGH_MEMORY;
  }
}
}  // namespace
extern "C" __declspec(dllexport) DWORD WINAPI TaxiCameraStart(void*) {
  wchar_t path[32768]{};
  const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
  const wchar_t* name = std::wcsrchr(path, L'\\');
  if (!length || length >= 32768 || _wcsicmp(name ? name + 1 : path, L"FlightSimulator2024.exe"))
    return ERROR_BAD_ENVIRONMENT;
  if (GetModuleHandleW(L"taxi-camera-native.addon64"))
    return ERROR_ALREADY_EXISTS;
  if (started.exchange(true))
    return ERROR_SUCCESS;
  HMODULE pinned{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&TaxiCameraStart), &pinned))
    return GetLastError();
  HANDLE thread = CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
  if (!thread) {
    started = false;
    return GetLastError();
  }
  CloseHandle(thread);
  return ERROR_SUCCESS;
}
BOOL WINAPI DllMain(HMODULE value, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH)
    DisableThreadLibraryCalls(value);
  return TRUE;
}
