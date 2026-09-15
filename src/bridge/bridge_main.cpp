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
#include "../shared/scene_demand.hpp"
#include "crash_evidence.hpp"
#include "d3d12_bridge.hpp"
#include "native_hooks.hpp"

namespace {
using namespace taxi_camera;
namespace win = standalone;
std::atomic<bool> started{};
void log_status(const win::Status& s, const char* detail = "") {
  wchar_t directory[32768]{};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, 32768);
  if (!n || n >= 32700)
    return;
  std::wstring path(directory);
  path += L"\\Taxi Cam";
  CreateDirectoryW(path.c_str(), nullptr);
  path += L"\\bridge.log";
  HANDLE file =
      CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  char line[2048];
  const auto length = std::snprintf(line, sizeof(line),
                                    "%llu native=%u scene=%u mask=%u left=%llu right=%llu captured=%llu composed=%llu stamps=%llu "
                                    "hooks_failed=%llu cutoff=%u | %s | %s\r\n",
                                    static_cast<unsigned long long>(GetTickCount64()), s.graphics_ready, s.scene_ready, s.taxi_mask,
                                    static_cast<unsigned long long>(s.left_id), static_cast<unsigned long long>(s.right_id),
                                    static_cast<unsigned long long>(s.captures), static_cast<unsigned long long>(s.composed),
                                    static_cast<unsigned long long>(s.stamps), static_cast<unsigned long long>(s.hook_failures),
                                    s.speed_inhibited, s.message, detail);
  DWORD wrote{};
  if (length > 0 && static_cast<size_t>(length) < sizeof(line))
    WriteFile(file, line, static_cast<DWORD>(length), &wrote, nullptr);
  CloseHandle(file);
}
struct StartupTiming {
  unsigned intent_mask{}, attempts{};
  bool observed{}, target_ready{}, output_ready{}, stamped{};
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
  const bool fault_evidence_ready = win::crash_evidence::initialize();
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
  log_status(status, fault_evidence_ready ? "Renderer fault evidence armed." : "Renderer fault evidence unavailable.");
  const auto key = win::graphics_status().device;
  // The Windows companion owns mount settings for native sessions.
  TaxiButtonIntent intent;
  DisplayExposureController exposure;
  CaptureProgress progress;
  StartupTiming startup, warmup_startup;
  win::ScenePrewarm prewarm;
  bool requested = false, failed = false, last_output = false;
  std::uint64_t last_view_wait_count = 0;
  std::uint64_t next_telemetry{}, next_discovery{}, next_recovery{}, next_log{}, route_request{}, last_frames{};
  unsigned rate{}, feeds{}, applied_profile{};
  std::uint64_t applied_profile_request{}, applied_session_epoch{};
  std::uint64_t pending_profile_request{}, pending_session_epoch{}, transition_token{};
  unsigned pending_profile{};
  bool changing_profile = false;
  win::CompanionControl control;
  win::Status last_logged{};
  bool logged = false, last_connected = false, last_requested = false;
  std::uint64_t last_stop_sequence{};
  std::array<std::array<double, 6>, 2> applied_mounts{};
  for (;;) {
    control.refresh(mailbox);
    const auto now = GetTickCount64();
    const auto& settings = control.settings();
    const bool connected = control.connected(now);
    const auto session_epoch = native_camera::get_aircraft_session_epoch();
    const bool session_settings = settings.aircraft_session_epoch == session_epoch;
    if (connected && (changing_profile || settings.profile != applied_profile || settings.profile_request != applied_profile_request ||
                      session_epoch != applied_session_epoch)) {
      if (!changing_profile || settings.profile != pending_profile || settings.profile_request != pending_profile_request ||
          session_epoch != pending_session_epoch) {
        win::set_target_mask(0);
        win::set_calibration(0, settings.calibration_budget);
        native_camera::suspend_scene_rendering(true);
        scene_runtime::manager().stop_source_tracking();
        scene_handoff().stop_scene();
        scene_runtime::reset_feed(key);
        pending_profile = settings.profile;
        pending_profile_request = settings.profile_request;
        pending_session_epoch = session_epoch;
        transition_token = 0;
        changing_profile = true;
        requested = failed = false;
        route_request = 0;
      }
      native_camera::suspend_scene_rendering(true);
      if (!transition_token)
        transition_token = native_camera::request_scene_profile_transition(pending_profile);
      const auto transition = native_camera::scene_snapshot();
      const bool ready = transition_token && transition.profile_transition_token == transition_token &&
                         transition.profile_transition_id == pending_profile && transition.profile_transition_ready &&
                         !transition.profile_transition_pending && !transition.profile_transition_failed;
      if (!ready) {
        const auto identity = native_camera::get_aircraft_identity();
        win::Status pending{};
        pending.heartbeat = now;
        pending.aircraft_session_epoch = session_epoch;
        pending.active_profile = applied_profile;
        pending.detected_profile = identity.fresh ? identity.detected_profile : 0;
        pending.identity_sample_ms = identity.fresh ? identity.sample_ms : 0;
        std::memcpy(pending.aircraft_type, identity.type.data(), sizeof(pending.aircraft_type));
        std::memcpy(pending.aircraft_path, identity.path.data(), sizeof(pending.aircraft_path));
        std::snprintf(pending.message, sizeof(pending.message), "%s",
                      transition.profile_transition_failed ? transition.message.c_str()
                                                           : "Closing and validating retained camera views for the aircraft profile.");
        if (mailbox.lock()) {
          mailbox.data()->status = pending;
          mailbox.unlock();
        }
        scene_runtime::service();
        if (now >= next_telemetry) {
          native_camera::initialize_body_pose_provider();
          next_telemetry = now + 2000;
        }
        Sleep(25);
        continue;
      }
      if (!native_camera::select_aircraft_profile(pending_profile)) {
        Sleep(25);
        continue;
      }
      win::set_aircraft_profile(pending_profile);
      if (applied_session_epoch != pending_session_epoch) {
        prewarm = {};
        warmup_startup = {};
      }
      applied_profile = pending_profile;
      applied_profile_request = pending_profile_request;
      applied_session_epoch = pending_session_epoch;
      char transition_detail[256]{};
      std::snprintf(transition_detail, sizeof(transition_detail),
                    "Aircraft transition: token=%llu session=%llu profile=%u retained_entries=%llu/%llu",
                    static_cast<unsigned long long>(transition_token), static_cast<unsigned long long>(applied_session_epoch),
                    applied_profile, static_cast<unsigned long long>(transition.pair.owned_ids[0]),
                    static_cast<unsigned long long>(transition.pair.owned_ids[1]));
      log_status(status, transition_detail);
      applied_mounts = {};
      intent = {};
      progress = {};
      startup = {};
      exposure = {};
      next_telemetry = next_discovery = 0;
      changing_profile = false;
    }
    if (now >= next_telemetry) {
      native_camera::initialize_body_pose_provider();
      next_telemetry = now + 2000;
    }
    if (now >= next_discovery) {
      win::discover_pfds(settings.auto_detect != 0 ? now : 0);
      next_discovery = now + 1000;
    }
    if (session_settings && settings.route_request && settings.route_request != route_request) {
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
        connected && session_settings && settings.enabled && aircraft_matches && win::graphics_status().ready && !cutoff.inhibited
            ? (settings.follow_taxi && !manual_only ? desired.buttons
               : session_settings                   ? settings.manual_mask
                                                    : 0)
            : 0;
    const bool test_scene =
        connected && session_settings && settings.enabled && aircraft_matches && settings.scene_test && !cutoff.inhibited;
    const auto intent_observed_ms = GetTickCount64();
    if (!mask && !test_scene && !prewarm.active())
      failed = false;
    const auto targets = win::target_ids();
    const unsigned assigned = (targets[0] ? 1u : 0u) | (targets[1] ? 2u : 0u);
    const auto speed = native_camera::get_ground_speed();
    const auto warm_readiness = [&]() {
      const auto sampled_now = GetTickCount64();
      const auto ground = native_camera::get_on_ground();
      const auto velocity = native_camera::get_ground_speed();
      const auto pose = native_camera::sample_body_pose(sampled_now);
      const auto aircraft = native_camera::get_aircraft_identity();
      const auto current_epoch = native_camera::get_aircraft_session_epoch();
      return win::ScenePrewarmReadiness{
          control.connected(GetTickCount64()),
          settings.aircraft_session_epoch == current_epoch && current_epoch == applied_session_epoch &&
              settings.profile == applied_profile && settings.profile_request == applied_profile_request,
          settings.enabled != 0,
          native_camera::aircraft_matches_profile() && aircraft.fresh && aircraft.detected_profile == settings.profile,
          win::graphics_status().ready,
          native_camera::get_taxi_cutoff().inhibited,
          manual_only || native_camera::get_taxi_buttons().valid,
          pose.valid || pose.calibration_required,
          ground.valid,
          ground.on_ground,
          velocity.valid,
          settings.calibration_mask != 0 || settings.single_camera != 0,
          velocity.knots};
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
    win::set_target_mask(active);
    win::set_calibration(
        connected && session_settings && settings.enabled && aircraft_matches && !cutoff.inhibited ? settings.calibration_mask : 0,
        settings.calibration_budget);
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
    composition.nose_dot = settings.nose_dot;
    composition.tail_upper = settings.tail_upper;
    composition.tail_corner = settings.tail_corner;
    composition.tail_inner = settings.tail_inner;
    if (demand.start) {
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
      bool warm_start_allowed = true;
      if (background_warmup) {
        // Preparation may compile shaders. Re-read the companion and public
        // session/ground evidence before queuing any native camera creation.
        control.refresh(mailbox);
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
        requested = native_camera::scene_snapshot().accepting_requests;
        start_timing.request_end_ms = GetTickCount64();
        log_startup(status, start_timing, requested ? "request_accepted" : "request_refused");
      }
      failed = win::finish_scene_start(prewarm, background_warmup, requested);
      if (!requested) {
        active = 0;
        win::set_target_mask(0);
        native_camera::suspend_scene_rendering(true);
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
    scene_runtime::service();
    const auto scene = native_camera::scene_snapshot();
    const auto output = scene_runtime::snapshot(key);
    if (now >= next_recovery) {
      const bool eligible = (mask || test_scene) && !failed && !output.failed && scene.pair.state == engine_camera::State::active &&
                            scene.requested_feeds == 2 && !scene.pose_waiting && !scene.view_waiting &&
                            native_camera::sample_body_pose(now).valid;
      if (eligible && output.frames != last_frames)
        native_camera::note_scene_capture_progress(now);
      last_frames = output.frames;
      // Lost GPU-state evidence says nothing about native camera lifetime.
      // Keep the pair and its resource generations; only observed barriers and
      // fresh recordings can restore capture. Never erase/recreate cameras here.
      progress.observe(now, eligible, output.frames, output.capture.source_draws,
                       std::strcmp(output.capture.tail_status, "unknown_source_state") == 0);
      next_recovery = now + 250;
    }
    const auto graphics = win::graphics_status();
    status = {};
    status.heartbeat = now;
    status.active_profile = applied_profile;
    status.detected_profile = identity.fresh ? identity.detected_profile : 0;
    status.identity_sample_ms = identity.fresh ? identity.sample_ms : 0;
    status.aircraft_session_epoch = session_epoch;
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
    status.stage_ms = scene.performance.stage_ms;
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
    const auto inventory = win::pfd_inventory();
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
    const char* target_message = "Detecting display textures for the selected aircraft profile.";
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
    const char* message =
        !connected          ? "Waiting for Windows companion heartbeat."
        : !settings.enabled ? "Camera service paused."
        : !aircraft_matches ? aircraft_message
        : cutoff.inhibited  ? (manual_only ? "Above 60 knots: camera displays inhibited." : "Above 60 knots: TAXI buttons commanded off.")
        : failed            ? scene.message.c_str()
        : scene.view_waiting && scene.stop_reason == native_camera::SceneStopReason::resolution_changed ? scene.message.c_str()
        : !manual_only && !buttons.valid                                                                ? buttons.error
        : (!targets[0] || !targets[1])                                                                  ? target_message
        : !active && settings.calibration_mask ? "Calibration requested on the selected display."
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
                    static_cast<unsigned long long>(scene.inspection_count), static_cast<unsigned long long>(scene.updates),
                    static_cast<unsigned long long>(graphics.clear_states),
                    scene.stop_reason == native_camera::SceneStopReason::none ? "" : scene.stop_detail.c_str());
      log_status(status, detail);
      char pfd_detail[640];
      std::snprintf(pfd_detail, sizeof(pfd_detail),
                    "PFD copy admission: selected_draws=%llu rt_metadata=%llu rt_callbacks=%llu pending_matches=%llu "
                    "view_resolved=%llu view_rejected=%llu attempts=%llu rejected=%llu state_skips=%llu "
                    "boundary_batches_refused=%llu boundary_passes_refused=%llu reason=%s",
                    static_cast<unsigned long long>(graphics.selected_draws),
                    static_cast<unsigned long long>(graphics.selected_rt_metadata),
                    static_cast<unsigned long long>(graphics.selected_rt_callbacks),
                    static_cast<unsigned long long>(graphics.selected_pending_matches),
                    static_cast<unsigned long long>(graphics.selected_view_resolved),
                    static_cast<unsigned long long>(graphics.selected_view_rejected),
                    static_cast<unsigned long long>(graphics.copy_attempts), static_cast<unsigned long long>(graphics.copy_rejected),
                    static_cast<unsigned long long>(output.state_skips), static_cast<unsigned long long>(boundaries.batch_refusals),
                    static_cast<unsigned long long>(boundaries.pass_refusals), graphics.copy_error);
      log_status(status, pfd_detail);
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
      char retention_detail[256];
      std::snprintf(
          retention_detail, sizeof(retention_detail),
          "Camera retention: created_total=%llu snapshot_bytes=%llu quarantined=%llu prewarm=%s patch_requests=%u patch_draws=%llu",
          static_cast<unsigned long long>(scene.created_total), static_cast<unsigned long long>(output.capture.bytes),
          static_cast<unsigned long long>(output.capture.quarantined), prewarm.name(), output.patch_requests,
          static_cast<unsigned long long>(output.patch_draws));
      log_status(status, retention_detail);
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
