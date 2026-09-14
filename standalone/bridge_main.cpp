#include <algorithm>
#include <atomic>
#include <cstring>
#include "../native-camera/body_pose_provider.hpp"
#include "../native-camera/mount_config.hpp"
#include "../native-camera/probe.hpp"
#include "../src/capture_progress.hpp"
#include "../src/display_exposure.hpp"
#include "../src/taxi_button_routes.hpp"
#include "d3d12_bridge.hpp"
#include "companion_control.hpp"
#include "native_hooks.hpp"
#include "protocol.hpp"

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
DWORD run_impl() {
  win::Mailbox mailbox;
  if (!mailbox.open(GetCurrentProcessId(), false))
    return ERROR_INVALID_DATA;
  win::Status status{};
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
  const auto key = win::graphics_status().device;
  // The Windows companion owns mount settings for native sessions.
  TaxiButtonIntent intent;
  DisplayExposureController exposure;
  CaptureProgress progress;
  bool requested = false, failed = false, last_output = false;
  std::uint64_t last_view_wait_count = 0;
  std::uint64_t next_telemetry{}, next_discovery{}, next_recovery{}, next_log{}, route_request{}, last_frames{};
  unsigned rate{}, feeds{}, applied_profile{};
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
    if (connected && (changing_profile || settings.profile != applied_profile)) {
      if (!changing_profile) {
        win::set_target_mask(0);
        win::set_calibration(0, settings.calibration_budget);
        scene_runtime::manager().stop_source_tracking();
        if (native_camera::scene_snapshot().hook_installed)
          native_camera::request_scene_stop(true);
        scene_runtime::reset_feed(key);
        changing_profile = true;
        requested = failed = false;
      }
      const auto pair = native_camera::scene_snapshot().pair;
      // Engine cleanup owns the IDs. Never switch adapters beneath live views.
      if (pair.owned_ids[0] || pair.owned_ids[1] || pair.creation_pending) {
        win::Status pending{};
        pending.heartbeat = now;
        std::snprintf(pending.message, sizeof(pending.message), "Waiting for camera cleanup before switching aircraft profile.");
        if (mailbox.lock()) {
          mailbox.data()->status = pending;
          mailbox.unlock();
        }
        scene_runtime::service();
        Sleep(25);
        continue;
      }
      if (!native_camera::request_scene_profile(settings.profile)) {
        Sleep(25);
        continue;
      }
      native_camera::select_aircraft_profile(settings.profile);
      win::set_aircraft_profile(settings.profile);
      applied_profile = settings.profile;
      applied_mounts = {};
      intent = {};
      progress = {};
      exposure = {};
      route_request = next_telemetry = next_discovery = 0;
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
    if (settings.route_request && settings.route_request != route_request) {
      if (win::assign_targets(settings.left_id, settings.right_id))
        route_request = settings.route_request;
    }
    const auto identity = native_camera::get_aircraft_identity();
    const bool aircraft_matches =
        native_camera::aircraft_matches_profile() && (!settings.auto_profile || identity.detected_profile == settings.profile);
    const auto buttons = native_camera::get_taxi_buttons();
    const auto cutoff = native_camera::get_taxi_cutoff();
    const auto desired = intent.observe(now, buttons.valid, buttons.left_on, buttons.right_on);
    const unsigned mask = connected && settings.enabled && aircraft_matches && win::graphics_status().ready && !cutoff.inhibited
                              ? (settings.follow_taxi ? desired.buttons : settings.manual_mask)
                              : 0;
    const bool test_scene = connected && settings.enabled && aircraft_matches && settings.scene_test && !cutoff.inhibited;
    if (!mask && !test_scene)
      failed = false;
    const auto targets = win::target_ids();
    const unsigned active = ((mask & 1) && targets[0] ? 1u : 0u) | ((mask & 2) && targets[1] ? 2u : 0u);
    win::set_target_mask(failed ? 0 : active);
    win::set_calibration(connected && settings.enabled && aircraft_matches && !cutoff.inhibited ? settings.calibration_mask : 0,
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
    if ((active || test_scene) && !requested && !failed) {
      if (scene_runtime::prepare(key)) {
        scene_runtime::set_composition(key, profiles::find(applied_profile)->composition);
        scene_runtime::reset_feed(key);
        scene_runtime::manager().begin_source_tracking();
        native_camera::request_scene_test(true);
        requested = native_camera::scene_snapshot().accepting_requests;
      }
      if (!requested) {
        failed = true;
        win::set_target_mask(0);
      }
    } else if (!active && !test_scene && requested &&
               !(connected && settings.enabled && aircraft_matches && !cutoff.inhibited && settings.follow_taxi && !buttons.valid)) {
      scene_runtime::manager().stop_source_tracking();
      native_camera::request_scene_stop(true);
      scene_runtime::reset_feed(key);
      requested = false;
    }
    // Unknown button state hides the output after its existing grace period,
    // but does not retire a valid scene pair. Fresh OFF and real profile/service
    // changes still take the normal cleanup path above.
    native_camera::suspend_scene_rendering(!active && !test_scene);
    auto composition = profiles::find(applied_profile ? applied_profile : settings.profile)->composition;
    composition.speed_color = settings.speed_color;
    scene_runtime::set_composition(key, composition);
    const auto speed = native_camera::get_ground_speed();
    const auto light = native_camera::get_lighting();
    const auto display = exposure.update(now, settings.exposure, settings.automatic_exposure != 0, settings.night_boost, light.valid,
                                         light.ambient, light.sample_ms);
    scene_runtime::set_display_exposure(key, display.applied_ev);
    scene_runtime::set_ground_speed(key, static_cast<float>(speed.knots), speed.valid);
    scene_runtime::service();
    const auto scene = native_camera::scene_snapshot();
    const auto output = scene_runtime::snapshot(key);
    if (now >= next_recovery) {
      const bool eligible = (active || test_scene) && !failed && !output.failed && scene.pair.state == engine_camera::State::active &&
                            scene.requested_feeds == 2 && !scene.pose_waiting && !scene.view_waiting &&
                            native_camera::sample_body_pose(now).valid;
      if (eligible && output.frames != last_frames)
        native_camera::note_scene_capture_progress(now);
      last_frames = output.frames;
      if (progress.observe(now, eligible, output.frames, output.capture.source_draws,
                           std::strcmp(output.capture.tail_status, "unknown_source_state") == 0) &&
          native_camera::request_capture_recovery())
        scene_runtime::reset_feed(key);
      next_recovery = now + 250;
    }
    const auto graphics = win::graphics_status();
    status = {};
    status.heartbeat = now;
    status.active_profile = applied_profile;
    status.detected_profile = identity.fresh ? identity.detected_profile : 0;
    status.identity_sample_ms = identity.fresh ? identity.sample_ms : 0;
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
    const auto inventory = win::pfd_inventory();
    status.candidate_count = static_cast<UINT>(std::min<size_t>(inventory.size(), 16));
    for (UINT i = 0; i < status.candidate_count; ++i)
      status.candidates[i] = {inventory[i].id,     inventory[i].draws,  inventory[i].width,
                              inventory[i].height, inventory[i].levels, inventory[i].format};
    const char* message = !connected                     ? "Waiting for Windows companion heartbeat."
                          : !settings.enabled            ? "Camera service paused."
                          : !aircraft_matches            ? "Waiting for a supported aircraft identity or profile switch."
                          : cutoff.inhibited             ? "Above 60 knots: TAXI buttons commanded off."
                          : failed                       ? scene.message.c_str()
                          : !buttons.valid               ? buttons.error
                          : (!targets[0] || !targets[1]) ? "Detecting display textures for the selected aircraft profile."
                          : !active                      ? "Ready. Use the aircraft's left or right TAXI button."
                          : !requested || failed         ? scene.message.c_str()
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
      char detail[1536];
      std::snprintf(detail, sizeof(detail),
                    "profile=%u matched=%u connected=%u requested=%u ipc_busy=%llu buttons_valid=%u held=%u expired=%u output=%u "
                    "stop_seq=%llu stop=%s "
                    "retry=%u pending=%u pose_wait=%u view_wait=%u waits=%llu ready=%u/%u inspection=%s/%s entries=%llu/%llu tail=%s "
                    "draws=%llu unknown_lists=%llu invalid_recordings=%llu scoped_invalidations=%llu | %.256s",
                    applied_profile, aircraft_matches, connected, requested, static_cast<unsigned long long>(control.busy_reads()),
                    buttons.valid, desired.held, desired.timed_out, output.output, static_cast<unsigned long long>(scene.stop_sequence),
                    native_camera::scene_stop_reason_name(scene.stop_reason), scene.recovery_attempts, scene.recovery_pending,
                    scene.pose_waiting, scene.view_waiting, static_cast<unsigned long long>(scene.view_wait_count), scene.ready[0],
                    scene.ready[1], scene.inspection_status[0], scene.inspection_status[1],
                    static_cast<unsigned long long>(scene.pair.owned_ids[0]), static_cast<unsigned long long>(scene.pair.owned_ids[1]),
                    output.capture.tail_status, static_cast<unsigned long long>(output.capture.source_draws),
                    static_cast<unsigned long long>(output.capture.unknown_submitted_lists),
                    static_cast<unsigned long long>(output.capture.invalid_source_recordings),
                    static_cast<unsigned long long>(output.capture.scoped_source_invalidations),
                    scene.stop_reason == native_camera::SceneStopReason::none ? "" : scene.stop_detail.c_str());
      log_status(status, detail);
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
