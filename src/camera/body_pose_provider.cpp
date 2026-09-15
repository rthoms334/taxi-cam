#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "body_pose_provider.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <utility>
#include "../profiles/catalog.hpp"
#include "body_pose_math.hpp"
#include "taxi_speed_cutoff.hpp"

namespace taxi_camera::native_camera {
namespace {
using Open = HRESULT(WINAPI*)(HANDLE*, const char*, HWND, DWORD, HANDLE, DWORD);
using Close = HRESULT(WINAPI*)(HANDLE);
using Define = HRESULT(WINAPI*)(HANDLE, DWORD, const char*, const char*, DWORD, float, DWORD);
using Request = HRESULT(WINAPI*)(HANDLE, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
using Dispatch = HRESULT(WINAPI*)(HANDLE, void**, DWORD*);
using SystemState = HRESULT(WINAPI*)(HANDLE, DWORD, const char*);
using Subscribe = HRESULT(WINAPI*)(HANDLE, DWORD, const char*);
using CameraGet = HRESULT(WINAPI*)(HANDLE, DWORD);
using LastPacket = HRESULT(WINAPI*)(HANDLE, DWORD*);
using MapEvent = HRESULT(WINAPI*)(HANDLE, DWORD, const char*);
using SetData = HRESULT(WINAPI*)(HANDLE, DWORD, DWORD, DWORD, DWORD, DWORD, void*);
using TransmitEvent = HRESULT(WINAPI*)(HANDLE, DWORD, DWORD, DWORD, DWORD, DWORD);
// Public SDK SIMCONNECT_DATA_CAMERA is packed: XYZ24, references/object IDs,
// XYZ24, FLOAT32 PBH12, references/object IDs, FOVdouble. Confirmed current
// 1.8.16.0 CameraGet response ID40, total96 bytes, including SIMCONNECT_RECV12.
#pragma pack(push, 1)
struct CameraData {
  double position[3];
  DWORD position_reference, position_object;
  double target[3];
  float pbh[3];
  DWORD rotation_reference, rotation_object;
  double fov;
};
#pragma pack(pop)
static_assert(sizeof(CameraData) == 84);
static_assert(offsetof(CameraData, fov) == 76);
struct AircraftData {
  body_math::Telemetry pose;
  double ground_speed_knots;
};
static_assert(sizeof(AircraftData) == 56);
static_assert(offsetof(AircraftData, ground_speed_knots) == 48);
struct State {
  SRWLOCK lock = SRWLOCK_INIT;
  HANDLE worker = nullptr, stop = nullptr;
  body_math::Telemetry aircraft{};
  CameraData camera{};
  std::uint64_t aircraft_ms = 0, camera_ms = 0;
  BodyTelemetryTiming timing{};
  double ground_speed_knots = 0;
  std::uint64_t ground_speed_ms = 0;
  const char* ground_speed_error = "not_initialized";
  bool on_ground = false;
  std::uint64_t on_ground_ms = 0;
  const char* on_ground_error = "not_initialized";
  bool taxi_left = false, taxi_right = false;
  AircraftIdentityCache identity;
  AircraftSessionLifecycle aircraft_session;
  std::uint64_t taxi_ms = 0;
  const char* taxi_error = "not_initialized";
  TaxiSpeedCutoff speed_cutoff;
  const char* cutoff_status = "below_speed_limit";
  double ambient = 0, brightness = 0;
  std::uint64_t lighting_ms = 0;
  const char* lighting_error = "not_initialized";
  bool calibrated = false;
  double correction = 0;
  Vector3 calibration_origin{};
  unsigned calibration_samples = 0;
  std::uint64_t calibration_camera_ms = 0;
  double candidate_correction = 0;
  Vector3 candidate_private{}, candidate_public{};
  const char* error = "not_initialized";
};
State state;
std::atomic<std::uint32_t> profile_id{1};
SRWLOCK lifecycle = SRWLOCK_INIT;
void reset_session_locked() noexcept {
  state.identity = {};
  state.aircraft_ms = state.camera_ms = state.ground_speed_ms = state.taxi_ms = state.lighting_ms = 0;
  state.on_ground_ms = 0;
  state.on_ground_error = "aircraft_session_changed";
  state.timing.last_sample_ms = state.timing.last_interval_ms = 0;
  state.taxi_left = state.taxi_right = false;
  state.speed_cutoff = {};
  state.cutoff_status = "below_speed_limit";
  state.calibrated = false;
  state.calibration_samples = 0;
  state.calibration_camera_ms = 0;
  state.error = state.ground_speed_error = state.taxi_error = state.lighting_error = "aircraft_session_changed";
}
bool accept_session_packet(const void* raw, DWORD bytes) noexcept {
  AcquireSRWLockExclusive(&state.lock);
  const bool changed = state.aircraft_session.accept(raw, bytes);
  if (changed || !state.aircraft_session.running())
    reset_session_locked();
  ReleaseSRWLockExclusive(&state.lock);
  return changed;
}
bool accept_identity_packet(const void* raw, DWORD bytes, std::uint64_t now) noexcept {
  AcquireSRWLockExclusive(&state.lock);
  const auto before = state.identity.sample(now);
  const bool accepted = state.identity.accept(raw, bytes, now);
  const auto after = state.identity.sample(now);
  const bool changed = accepted && before.sample_ms && after.sample_ms != before.sample_ms &&
                       (std::strcmp(before.type.data(), after.type.data()) || std::strcmp(before.path.data(), after.path.data()));
  if (changed) {
    state.aircraft_session.changed();
    reset_session_locked();
  }
  ReleaseSRWLockExclusive(&state.lock);
  return changed;
}
void on_ground_failure(const char* text) noexcept {
  AcquireSRWLockExclusive(&state.lock);
  state.on_ground_ms = 0;
  state.on_ground_error = text;
  ReleaseSRWLockExclusive(&state.lock);
}
bool accept_on_ground_packet(const void* raw, DWORD bytes, std::uint64_t sample_ms) noexcept {
  if (!raw || bytes != 48 || !sample_ms)
    return false;
  std::array<DWORD, 10> header{};
  std::memcpy(header.data(), raw, sizeof(header));
  if (header[0] != bytes || header[2] != 8 || header[3] != 7 || header[5] != 7 || header[6] != 0 || header[9] != 1)
    return false;
  double value{};
  std::memcpy(&value, static_cast<const unsigned char*>(raw) + 40, sizeof(value));
  AcquireSRWLockExclusive(&state.lock);
  if (value == 0 || value == 1) {
    state.on_ground = value == 1;
    state.on_ground_ms = sample_ms;
    state.on_ground_error = "";
  } else {
    state.on_ground_ms = 0;
    state.on_ground_error = "on_ground_values";
  }
  ReleaseSRWLockExclusive(&state.lock);
  return true;
}
void lighting_failure(const char* text) noexcept {
  AcquireSRWLockExclusive(&state.lock);
  state.lighting_ms = 0;
  state.lighting_error = text;
  ReleaseSRWLockExclusive(&state.lock);
}
void taxi_failure(const char* text) noexcept {
  AcquireSRWLockExclusive(&state.lock);
  state.taxi_ms = 0;
  state.taxi_error = text;
  ReleaseSRWLockExclusive(&state.lock);
}
void failure(const char* text, bool invalidate_ground_speed = true) noexcept {
  AcquireSRWLockExclusive(&state.lock);
  state.error = text;
  state.aircraft_ms = 0;
  state.camera_ms = 0;
  if (invalidate_ground_speed) {
    state.on_ground_ms = 0;
    state.on_ground_error = text;
    state.ground_speed_ms = 0;
    state.ground_speed_error = text;
    state.taxi_ms = 0;
    state.taxi_error = text;
    state.lighting_ms = 0;
    state.lighting_error = text;
  }
  ReleaseSRWLockExclusive(&state.lock);
}
bool accept_aircraft_packet(const void* raw, DWORD bytes, std::uint64_t sample_ms) noexcept {
  if (!raw || bytes < 40 || bytes > 65536 || !sample_ms)
    return false;
  std::array<DWORD, 10> header{};
  std::memcpy(header.data(), raw, sizeof(header));
  constexpr DWORD PacketBytes = 40 + sizeof(AircraftData);
  // SIMCONNECT_OBJECT_ID_USER(0) is the request alias. The response identifies
  // the actual aircraft object (observed2883584), so correlate our dedicated
  // request/definition IDs rather than requiring a zero returned object ID.
  if (header[2] != 8 || header[0] != PacketBytes || bytes < PacketBytes || header[3] != 1 || header[5] != 1 || header[6] != 0 ||
      header[9] != 7)
    return false;
  AircraftData value{};
  std::memcpy(&value, static_cast<const unsigned char*>(raw) + 40, sizeof(value));
  const bool pose_valid = body_math::valid(value.pose);
  const bool speed_valid = std::isfinite(value.ground_speed_knots) && value.ground_speed_knots >= 0 && value.ground_speed_knots <= 2000;
  AcquireSRWLockExclusive(&state.lock);
  if (pose_valid) {
    if (state.timing.accepted_samples != std::numeric_limits<std::uint64_t>::max())
      ++state.timing.accepted_samples;
    state.timing.last_interval_ms =
        state.timing.last_sample_ms && sample_ms >= state.timing.last_sample_ms ? sample_ms - state.timing.last_sample_ms : 0;
    state.timing.last_sample_ms = sample_ms;
    state.aircraft = value.pose;
    state.aircraft_ms = sample_ms;
    state.error = "";
  } else {
    state.aircraft_ms = state.camera_ms = 0;
    state.error = "aircraft_values";
  }
  // Independent validity: a camera calibration or pose failure must not turn
  // an otherwise current public groundspeed into a fabricated zero.
  if (speed_valid) {
    state.ground_speed_knots = value.ground_speed_knots;
    state.ground_speed_ms = sample_ms;
    state.ground_speed_error = "";
  } else {
    state.ground_speed_ms = 0;
    state.ground_speed_error = "ground_speed_values";
  }
  ReleaseSRWLockExclusive(&state.lock);
  return true;
}
bool accept_taxi_packet(const void* raw, DWORD bytes, std::uint64_t sample_ms) noexcept {
  // Definition3 is deliberately separate from the existing seven-field body
  // packet. USER(0) is a request alias, not the returned aircraft object ID.
  if (!raw || bytes != 56 || !sample_ms)
    return false;
  std::array<DWORD, 10> header{};
  std::memcpy(header.data(), raw, sizeof(header));
  if (header[0] != 56 || header[2] != 8 || header[3] != 3 || header[5] != 3 || header[6] != 0 || header[9] != 2)
    return false;
  std::array<double, 2> values{};
  std::memcpy(values.data(), static_cast<const unsigned char*>(raw) + 40, sizeof(values));
  const bool valid = (values[0] == 0 || values[0] == 1) && (values[1] == 0 || values[1] == 1);
  AcquireSRWLockExclusive(&state.lock);
  if (valid) {
    state.taxi_left = values[0] == 1;
    state.taxi_right = values[1] == 1;
    state.taxi_ms = sample_ms;
    state.taxi_error = "";
  } else {
    state.taxi_ms = 0;
    state.taxi_error = "taxi_button_values";
  }
  ReleaseSRWLockExclusive(&state.lock);
  return true;
}
bool accept_lighting_packet(const void* raw, DWORD bytes, std::uint64_t sample_ms) noexcept {
  // Definition4 cannot change the established body7/TAXI2 packet layouts.
  if (!raw || bytes != 56 || !sample_ms)
    return false;
  std::array<DWORD, 10> header{};
  std::memcpy(header.data(), raw, sizeof(header));
  if (header[0] != 56 || header[2] != 8 || header[3] != 4 || header[5] != 4 || header[6] != 0 || header[9] != 2)
    return false;
  std::array<double, 2> values{};
  std::memcpy(values.data(), static_cast<const unsigned char*>(raw) + 40, sizeof(values));
  const bool valid =
      std::isfinite(values[0]) && values[0] >= 0 && values[0] <= 1e7 && std::isfinite(values[1]) && values[1] >= 0 && values[1] <= 1;
  AcquireSRWLockExclusive(&state.lock);
  if (valid) {
    state.ambient = values[0];
    state.brightness = values[1];
    state.lighting_ms = sample_ms;
    state.lighting_error = "";
  } else {
    state.lighting_ms = 0;
    state.lighting_error = "lighting_values";
  }
  ReleaseSRWLockExclusive(&state.lock);
  return true;
}
DWORD WINAPI worker(void*) noexcept {
  const auto& profile = *profiles::find(profile_id.load());
  wchar_t path[32768]{};
  const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
  if (!length || length >= 32768) {
    failure("main_image_path");
    return 0;
  }
  wchar_t* slash = std::wcsrchr(path, L'\\');
  if (!slash || static_cast<std::size_t>(slash - path) + 25 >= 32768) {
    failure("main_image_path");
    return 0;
  }
  std::wcscpy(slash + 1, L"SimConnect_internal.dll");
#ifdef TAXI_BODY_POSE_PROVIDER_VALIDATION
  // Isolated validation executable only; production derives the client from
  // its already verified simulator image directory.
  std::wcscpy(path, L"C:/XboxGames/Microsoft Flight Simulator 2024/Content/SimConnect_internal.dll");
#endif
#ifdef TAXI_BODY_POSE_PROVIDER_EXTERNAL_VALIDATION
  // The internal client cannot open an external-process connection in the
  // current simulator build. This public external client tests identical
  // request, dispatch and cache code without changing the production path.
  std::wcscpy(path, L"C:/Program Files/Little Navmap/SimConnect_msfs_2024.dll");
#endif
  const auto dll = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dll) {
    failure("simconnect_client_unavailable");
    return 0;
  }
  const auto open = reinterpret_cast<Open>(GetProcAddress(dll, "SimConnect_Open"));
  const auto close = reinterpret_cast<Close>(GetProcAddress(dll, "SimConnect_Close"));
  const auto define = reinterpret_cast<Define>(GetProcAddress(dll, "SimConnect_AddToDataDefinition"));
  const auto request = reinterpret_cast<Request>(GetProcAddress(dll, "SimConnect_RequestDataOnSimObject"));
  const auto dispatch = reinterpret_cast<Dispatch>(GetProcAddress(dll, "SimConnect_GetNextDispatch"));
  const auto system_state = reinterpret_cast<SystemState>(GetProcAddress(dll, "SimConnect_RequestSystemState"));
  const auto subscribe = reinterpret_cast<Subscribe>(GetProcAddress(dll, "SimConnect_SubscribeToSystemEvent"));
  const auto get = reinterpret_cast<CameraGet>(GetProcAddress(dll, "SimConnect_CameraGet"));
  const auto last_packet = reinterpret_cast<LastPacket>(GetProcAddress(dll, "SimConnect_GetLastSentPacketID"));
  const auto map_event = reinterpret_cast<MapEvent>(GetProcAddress(dll, "SimConnect_MapClientEventToSimEvent"));
  const auto set_data = reinterpret_cast<SetData>(GetProcAddress(dll, "SimConnect_SetDataOnSimObject"));
  const auto transmit_event = reinterpret_cast<TransmitEvent>(GetProcAddress(dll, "SimConnect_TransmitClientEvent"));
  if (!open || !close || !define || !request || !dispatch || !get || !system_state || !subscribe) {
    failure("simconnect_exports");
    FreeLibrary(dll);
    return 0;
  }
  HANDLE session = nullptr;
  const HANDLE notification = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!notification) {
    failure("simconnect_notification_event");
    FreeLibrary(dll);
    return 0;
  }
  HRESULT hr = open(&session, "Taxi Cam aircraft mount telemetry", nullptr, 0, notification, 0);
  // Public lifecycle events discard the connection's queued body/camera/TAXI
  // responses before the companion reconnects. This also redefines aircraft
  // Lvars that were unavailable while the new aircraft was loading.
  // https://docs.flightsimulator.com/html/Programming_Tools/SimConnect/API_Reference/Events_And_Data/SimConnect_SubscribeToSystemEvent.htm
  for (const auto event :
       {std::pair{AircraftSessionLifecycle::SimEvent, "Sim"}, std::pair{AircraftSessionLifecycle::AircraftEvent, "AircraftLoaded"},
        std::pair{AircraftSessionLifecycle::FlightEvent, "FlightLoaded"}}) {
    if (SUCCEEDED(hr))
      hr = subscribe(session, event.first, event.second);
  }
  const char* names[]{"PLANE LATITUDE",      "PLANE LONGITUDE",    "PLANE ALTITUDE",
                      "PLANE PITCH DEGREES", "PLANE BANK DEGREES", "PLANE HEADING DEGREES TRUE",
                      "GROUND VELOCITY"};
  // Official SimVar/units: GROUND VELOCITY accepts knots. All seven fields use
  // SIMCONNECT_DATATYPE_FLOAT64; no conversion or integer truncation here.
  // https://docs.flightsimulator.com/msfs2024/retail/sound/rtpc-and-simulation-variables/
  for (unsigned i = 0; SUCCEEDED(hr) && i < std::size(names); ++i)
    hr = define(session, 1, names[i], i == 6 ? "knots" : i == 2 ? "meters" : "degrees", 4, 0, 0xffffffffu);
  if (FAILED(hr)) {
    failure("simconnect_open_or_definition");
    if (session)
      close(session);
    CloseHandle(notification);
    FreeLibrary(dll);
    return 0;
  }
  // A continuous SIM_FRAME stream removes the former50ms request-once hold.
  // Data is still published by SimConnect at frame end; this is no prediction.
  // https://docs.flightsimulator.com/msfs2024/retail/programming-apis/simconnect/api-reference/structures-and-enumerations/simconnect_period/
  if (FAILED(request(session, 1, 1, 0, 3, 0, 0, 0, 0))) {
    failure("simconnect_body_stream");
    close(session);
    CloseHandle(notification);
    FreeLibrary(dll);
    return 0;
  }
  // L variables are directly readable through public SimConnect as FLOAT64.
  // https://docs.flightsimulator.com/msfs2024/retail/programming-apis/simconnect/api-reference/events-and-data/simconnect_addtodatadefinition/
  std::array<DWORD, 64> taxi_packets{};
  unsigned taxi_packet_cursor = 0;
  const auto remember_taxi_packet = [&]() {
    DWORD id = 0;
    if (last_packet && SUCCEEDED(last_packet(session, &id)) && id)
      taxi_packets[taxi_packet_cursor++ % taxi_packets.size()] = id;
  };
  const bool manual_only = profile.taxi_control == profiles::TaxiControl::manual_only;
  bool taxi_defined = !manual_only && last_packet != nullptr;
  for (const auto name : profile.taxi_lvars) {
    if (!taxi_defined)
      break;
    taxi_defined = SUCCEEDED(define(session, 3, name, "number", 4, 0, 0xffffffffu));
    remember_taxi_packet();
  }
  if (!taxi_defined)
    taxi_failure(manual_only ? "taxi_buttons_unavailable_use_manual_control" : "taxi_definition_unavailable");
  std::array<bool, 2> taxi_events{};
  // Isolated telemetry readers never control the aircraft. Production sends
  // the aircraft's real input event; its own controller updates the light.
#if !defined(TAXI_BODY_POSE_PROVIDER_VALIDATION) && !defined(TAXI_BODY_POSE_PROVIDER_EXTERNAL_VALIDATION)
  if (profile.taxi_control == profiles::TaxiControl::push_event && map_event && transmit_event) {
    for (unsigned side = 0; side < 2; ++side) {
      taxi_events[side] = SUCCEEDED(map_event(session, 10 + side, profile.taxi_events[side]));
      remember_taxi_packet();
    }
  }
  if (profile.taxi_control == profiles::TaxiControl::lvar_off && set_data) {
    for (unsigned side = 0; side < 2; ++side) {
      // iniBuilds uses this exact latch for TAXI state and its lamp. A separate
      // single-field definition permits idempotent OFF without touching the
      // opposite side or generating a second toggle while awaiting an ACK.
      taxi_events[side] = SUCCEEDED(define(session, 10 + side, profile.taxi_lvars[side], "number", 4, 0, 0xffffffffu));
      remember_taxi_packet();
    }
  }
#else
  (void)map_event;
#endif
  const bool type_defined = SUCCEEDED(define(session, 5, "ATC TYPE", nullptr, 9, 0, 0xffffffffu));
  // Both public Number values were observed through separate requests in the
  // parked simulator. Official Asobo Emissive.xml maps ambient1..4000; the
  // documented glass-cockpit brightness is0..1. This is optional display data.
  std::array<DWORD, 16> lighting_packets{};
  unsigned lighting_packet_cursor = 0;
  const auto remember_lighting_packet = [&]() {
    DWORD id = 0;
    if (last_packet && SUCCEEDED(last_packet(session, &id)) && id)
      lighting_packets[lighting_packet_cursor++ % lighting_packets.size()] = id;
  };
  bool lighting_defined = last_packet != nullptr;
  for (const auto name : {"AMBIENT LIGHT SENSOR", "GLASSCOCKPIT AUTOMATIC BRIGHTNESS"}) {
    if (!lighting_defined)
      break;
    lighting_defined = SUCCEEDED(define(session, 4, name, "number", 4, 0, 0xffffffffu));
    remember_lighting_packet();
  }
  if (!lighting_defined)
    lighting_failure("lighting_definition_unavailable");
  // Separate optional definition preserves the existing seven-field body ABI.
  // Official SimVar: SIM ON GROUND, Bool (converted to FLOAT64 by SimConnect).
  // https://docs.flightsimulator.com/msfs2024/html/6_Programming_APIs/SimVars/Miscellaneous_Variables.htm
  std::array<DWORD, 16> on_ground_packets{};
  unsigned on_ground_packet_cursor = 0;
  const auto remember_on_ground_packet = [&]() {
    DWORD id = 0;
    if (last_packet && SUCCEEDED(last_packet(session, &id)) && id)
      on_ground_packets[on_ground_packet_cursor++ % on_ground_packets.size()] = id;
  };
  bool on_ground_defined = last_packet && SUCCEEDED(define(session, 7, "SIM ON GROUND", "bool", 4, 0, 0xffffffffu));
  remember_on_ground_packet();
  if (!on_ground_defined)
    on_ground_failure("on_ground_definition_unavailable");
  std::uint64_t previous_on_ground = 0;
  std::uint64_t previous = 0;
  std::uint64_t previous_lighting = 0, previous_type = 0;
  DWORD identity_request = 1000;
  bool reconnect = false;
  const HANDLE waits[]{state.stop, notification};
  while (true) {
    // Incoming packets wake the worker immediately. The50ms timeout only
    // services CameraGet/TAXI (20Hz) and optional lighting (2Hz), without spin.
    const auto wake = WaitForMultipleObjects(2, waits, FALSE, 50);
    if (wake == WAIT_OBJECT_0)
      break;
    if (wake != WAIT_OBJECT_0 + 1 && wake != WAIT_TIMEOUT) {
      failure("simconnect_notification_wait");
      break;
    }
    const auto now = GetTickCount64();
    if (type_defined && now - previous_type >= 1000) {
      previous_type = now;
      if (identity_request > 0xfffffffcu)
        break;
      AcquireSRWLockExclusive(&state.lock);
      state.identity.begin_request(identity_request, identity_request + 1);
      ReleaseSRWLockExclusive(&state.lock);
      request(session, identity_request, 5, 0, 1, 0, 0, 0, 0);
      system_state(session, identity_request + 1, "AircraftLoaded");
      identity_request += 2;
    }
    if (on_ground_defined && now - previous_on_ground >= 250) {
      previous_on_ground = now;
      const auto ground_hr = request(session, 7, 7, 0, 1, 0, 0, 0, 0);
      remember_on_ground_packet();
      if (FAILED(ground_hr)) {
        on_ground_defined = false;
        on_ground_failure("on_ground_request_failed");
      }
    }
    if (lighting_defined && now - previous_lighting >= 500) {
      previous_lighting = now;
      const auto lighting_hr = request(session, 4, 4, 0, 1, 0, 0, 0, 0);
      remember_lighting_packet();
      if (FAILED(lighting_hr))
        lighting_failure("lighting_request_failed");
    }
    if (now - previous >= 50) {
      previous = now;
      if (FAILED(get(session, 2))) {
        failure("simconnect_request");
        break;
      }
      if (taxi_defined) {
        const auto taxi_hr = request(session, 3, 3, 0, 1, 0, 0, 0, 0);
        remember_taxi_packet();
        if (FAILED(taxi_hr))
          taxi_failure("taxi_request_failed");
      }
    }
    for (unsigned n = 0; n < 64; ++n) {
      void* raw = nullptr;
      DWORD bytes = 0;
      if (FAILED(dispatch(session, &raw, &bytes)) || !raw)
        break;
      if (bytes < 12 || bytes > 65536) {
        failure("simconnect_packet_bounds");
        continue;
      }
      std::array<DWORD, 10> header{};
      std::memcpy(header.data(), raw, bytes >= 40 ? 40 : 12);
      if (header[0] < 12 || header[0] > bytes) {
        if (bytes >= 40 && header[2] == 8 && (header[3] == 7 || header[5] == 7)) {
          on_ground_failure("on_ground_packet_bounds");
          continue;
        }
        if (bytes >= 40 && header[2] == 8 && (header[3] == 4 || header[5] == 4)) {
          lighting_failure("lighting_packet_bounds");
          continue;
        }
        if (bytes >= 40 && header[2] == 8 && (header[3] == 3 || header[5] == 3)) {
          taxi_failure("taxi_packet_bounds");
          continue;
        }
        failure("simconnect_packet_bounds");
        continue;
      }
      if (header[2] == 4 || header[2] == 6) {
        reconnect = accept_session_packet(raw, bytes);
        if (reconnect)
          break;
      } else if (header[2] == 15 || (header[2] == 8 && bytes >= 40 && header[5] == 5)) {
        reconnect = accept_identity_packet(raw, bytes, GetTickCount64());
        if (reconnect)
          break;
      } else if (header[2] == 8) {
        if (bytes >= 40 && (header[3] == 7 || header[5] == 7)) {
          if (!accept_on_ground_packet(raw, bytes, GetTickCount64()))
            on_ground_failure("on_ground_packet_layout");
        } else if (bytes >= 40 && (header[3] == 4 || header[5] == 4)) {
          if (!accept_lighting_packet(raw, bytes, GetTickCount64()))
            lighting_failure("lighting_packet_layout");
        } else if (bytes >= 40 && (header[3] == 3 || header[5] == 3)) {
          if (!accept_taxi_packet(raw, bytes, GetTickCount64()))
            taxi_failure("taxi_packet_layout");
        } else {
          accept_aircraft_packet(raw, bytes, GetTickCount64());
        }
      } else if (header[2] == 40 && header[0] == 96 && bytes >= 96) {
        CameraData value{};
        std::memcpy(&value, static_cast<const unsigned char*>(raw) + 12, 84);
        if (value.position_reference != 2 || !std::isfinite(value.fov) || value.fov <= 0 || value.fov >= 3.2 ||
            !std::isfinite(value.position[0]) || !std::isfinite(value.position[1]) || !std::isfinite(value.position[2]) ||
            std::abs(value.position[0]) > 90 || std::abs(value.position[1]) > 180 || std::abs(value.position[2]) > 100000) {
          failure("camera_world_values", false);
          continue;
        }
        AcquireSRWLockExclusive(&state.lock);
        state.camera = value;
        state.camera_ms = GetTickCount64();
        ReleaseSRWLockExclusive(&state.lock);
      } else if (header[2] == 1) {
        // Exception SendID identifies a request, not a data-definition ID.
        // Correlated TAXI failures never clear valid aircraft/groundspeed.
        std::array<DWORD, 6> exception{};
        if (bytes >= sizeof(exception))
          std::memcpy(exception.data(), raw, sizeof(exception));
        bool taxi_exception = false;
        bool lighting_exception = false;
        bool on_ground_exception = false;
        for (const auto sent : taxi_packets)
          taxi_exception = taxi_exception || (sent && sent == exception[4]);
        for (const auto sent : lighting_packets)
          lighting_exception = lighting_exception || (sent && sent == exception[4]);
        for (const auto sent : on_ground_packets)
          on_ground_exception = on_ground_exception || (sent && sent == exception[4]);
        if (on_ground_exception) {
          on_ground_defined = false;
          on_ground_failure("on_ground_simconnect_exception");
        } else if (lighting_exception) {
          lighting_failure("lighting_simconnect_exception");
        } else if (taxi_exception) {
          taxi_failure("taxi_simconnect_exception");
        } else {
          // An uncorrelated asynchronous error does not identify which cache
          // failed. Retain current samples; their500ms freshness bound applies.
          AcquireSRWLockExclusive(&state.lock);
          state.error = "simconnect_exception";
          ReleaseSRWLockExclusive(&state.lock);
        }
      } else if (header[2] == 3) {
        failure("simulator_quit");
        SetEvent(state.stop);
        break;
      }
    }
    if (reconnect)
      break;
    const auto speed = get_ground_speed();
    const auto buttons = get_taxi_buttons();
    AcquireSRWLockExclusive(&state.lock);
    const auto commands = state.speed_cutoff.update(GetTickCount64(), speed.valid, speed.knots, buttons.valid,
                                                    (buttons.left_on ? 1u : 0u) | (buttons.right_on ? 2u : 0u), buttons.sample_ms,
                                                    profile.speed_cutoff_knots, !manual_only);
    state.cutoff_status = state.speed_cutoff.pending()     ? "waiting_for_taxi_off"
                          : state.speed_cutoff.inhibited() ? "ground_speed_above_60_knots"
                                                           : "below_speed_limit";
    ReleaseSRWLockExclusive(&state.lock);
    for (unsigned side = 0; side < 2; ++side) {
      if (!(commands & (1u << side)))
        continue;
      // Public SimConnect priority flag: GroupID is an explicit priority.
      // https://docs.flightsimulator.com/msfs2024/retail/programming-apis/simconnect/api-reference/events-and-data/simconnect_transmitclientevent/
      double off = 0;
      const bool accepted = taxi_events[side] && aircraft_matches_profile() &&
                            (profile.taxi_control == profiles::TaxiControl::lvar_off
                                 ? set_data && SUCCEEDED(set_data(session, 10 + side, 0, 0, 0, sizeof(off), &off))
                                 : transmit_event && SUCCEEDED(transmit_event(session, 0, 10 + side, 0, 1, 16)));
      if (taxi_events[side])
        remember_taxi_packet();
      AcquireSRWLockExclusive(&state.lock);
      state.speed_cutoff.sent(side, accepted);
      state.cutoff_status = accepted ? "waiting_for_taxi_off" : "taxi_off_event_unavailable";
      ReleaseSRWLockExclusive(&state.lock);
    }
  }
  on_ground_failure("on_ground_session_closed");
  taxi_failure("taxi_session_closed");
  lighting_failure("lighting_session_closed");
  close(session);
  CloseHandle(notification);
  FreeLibrary(dll);
  return 0;
}
bool fresh(std::uint64_t sample, std::uint64_t now) noexcept {
  return sample && now >= sample && now - sample <= 500;
}
OnGroundSample on_ground_locked(std::uint64_t now) noexcept {
  OnGroundSample out;
  out.sample_ms = state.on_ground_ms;
  if (fresh(out.sample_ms, now)) {
    out.valid = true;
    out.on_ground = state.on_ground;
    out.error = "";
  } else {
    out.error = out.sample_ms ? "on_ground_telemetry_stale" : state.on_ground_error;
  }
  return out;
}
GroundSpeedSample ground_speed_locked(std::uint64_t now) noexcept {
  GroundSpeedSample out;
  out.sample_ms = state.ground_speed_ms;
  if (fresh(out.sample_ms, now)) {
    out.valid = true;
    out.knots = state.ground_speed_knots;
    out.error = "";
  } else {
    out.error = out.sample_ms ? "ground_speed_telemetry_stale" : state.ground_speed_error;
  }
  return out;
}
TaxiButtonSample taxi_buttons_locked(std::uint64_t now) noexcept {
  TaxiButtonSample out;
  out.sample_ms = state.taxi_ms;
  if (fresh(out.sample_ms, now)) {
    out.valid = true;
    out.left_on = state.taxi_left;
    out.right_on = state.taxi_right;
    out.error = "";
  } else {
    out.error = out.sample_ms ? "taxi_telemetry_stale" : state.taxi_error;
  }
  return out;
}
LightingSample lighting_locked(std::uint64_t now) noexcept {
  LightingSample out;
  out.sample_ms = state.lighting_ms;
  if (out.sample_ms && now >= out.sample_ms && now - out.sample_ms <= LightingSample::MaximumAgeMs) {
    out.valid = true;
    out.ambient = state.ambient;
    out.brightness = state.brightness;
    out.error = "";
  } else {
    out.error = out.sample_ms ? "lighting_telemetry_stale" : state.lighting_error;
  }
  return out;
}
}  // namespace
bool select_aircraft_profile(std::uint32_t id) noexcept {
  if (!profiles::find(id))
    return false;
  shutdown_body_pose_provider();
  profile_id.store(id);
  return true;
}
AircraftIdentitySample get_aircraft_identity() noexcept {
  AcquireSRWLockShared(&state.lock);
  const auto result = state.aircraft_session.running() ? state.identity.sample(GetTickCount64()) : AircraftIdentitySample{};
  ReleaseSRWLockShared(&state.lock);
  return result;
}
std::uint64_t get_aircraft_session_epoch() noexcept {
  AcquireSRWLockShared(&state.lock);
  const auto result = state.aircraft_session.epoch();
  ReleaseSRWLockShared(&state.lock);
  return result;
}
bool aircraft_matches_profile() noexcept {
  const auto identity = get_aircraft_identity();
  // Manual and automatic selection use the same vendor/variant identity. ATC
  // brand text alone cannot approve or reject an aircraft adapter.
  return identity.fresh && identity.detected_profile == profile_id.load();
}
bool initialize_body_pose_provider() noexcept {
  AcquireSRWLockExclusive(&lifecycle);
  if (state.worker && WaitForSingleObject(state.worker, 0) == WAIT_OBJECT_0) {
    CloseHandle(state.worker);
    CloseHandle(state.stop);
    state.worker = nullptr;
    state.stop = nullptr;
  }
  if (state.worker) {
    ReleaseSRWLockExclusive(&lifecycle);
    return true;
  }
  // Automatic EFIS telemetry can start before any native hook pins the addon.
  // FROM_ADDRESS also works for the standalone test executable's main image.
  HMODULE provider_module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&worker),
                          &provider_module)) {
    ReleaseSRWLockExclusive(&lifecycle);
    failure("provider_module_pin");
    taxi_failure("provider_module_pin");
    return false;
  }
  state.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (state.stop)
    state.worker = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
  const bool ready = state.worker != nullptr;
  if (!ready && state.stop) {
    CloseHandle(state.stop);
    state.stop = nullptr;
  }
  ReleaseSRWLockExclusive(&lifecycle);
  return ready;
}
void shutdown_body_pose_provider() noexcept {
  AcquireSRWLockExclusive(&lifecycle);
  if (state.worker) {
    SetEvent(state.stop);
    WaitForSingleObject(state.worker, INFINITE);
    CloseHandle(state.worker);
    CloseHandle(state.stop);
    state.worker = nullptr;
    state.stop = nullptr;
  }
  AcquireSRWLockExclusive(&state.lock);
  state.aircraft_ms = state.camera_ms = 0;
  state.identity = {};
  state.ground_speed_ms = 0;
  state.ground_speed_error = "not_initialized";
  state.on_ground_ms = 0;
  state.on_ground_error = "not_initialized";
  state.taxi_ms = 0;
  state.taxi_error = "not_initialized";
  state.lighting_ms = 0;
  state.lighting_error = "not_initialized";
  state.taxi_left = state.taxi_right = false;
  state.speed_cutoff = {};
  state.cutoff_status = "below_speed_limit";
  state.calibrated = false;
  state.calibration_samples = 0;
  state.calibration_camera_ms = 0;
  state.error = "not_initialized";
  ReleaseSRWLockExclusive(&state.lock);
  ReleaseSRWLockExclusive(&lifecycle);
}
void reset_body_pose_calibration() noexcept {
  AcquireSRWLockExclusive(&state.lock);
  state.calibrated = false;
  state.calibration_samples = 0;
  state.calibration_camera_ms = 0;
  ReleaseSRWLockExclusive(&state.lock);
}
bool calibrate_body_pose(const Vector3& position, float fov, std::uint64_t now) noexcept {
  Vector3 lla{};
  if (!body_math::geodetic(position, lla) || !std::isfinite(fov))
    return false;
  AcquireSRWLockExclusive(&state.lock);
  // The worker may publish after the caller sampled its clock but before this
  // lock. Judge freshness at the locked read, retaining future test deadlines.
  const auto locked_now = GetTickCount64();
  if (now < locked_now)
    now = locked_now;
  bool good = fresh(state.camera_ms, now) && fresh(state.aircraft_ms, now) && now - state.camera_ms <= 100 &&
              now - state.aircraft_ms <= 100 && std::abs(state.camera.fov - fov) <= 0.0001;
  const auto camera_surface = body_math::ecef(state.camera.position[0], state.camera.position[1], 0);
  const auto private_surface = body_math::ecef(lla[0], lla[1], 0);
  const double delta = lla[2] - state.camera.position[2];
  good = good && body_math::distance(camera_surface, private_surface) <= 0.5 && std::isfinite(delta) && std::abs(delta) <= 150;
  bool accepted = false;
  if (!good)
    state.calibration_samples = 0;
  else if (state.camera_ms != state.calibration_camera_ms) {
    const auto public_position = body_math::ecef(state.camera.position[0], state.camera.position[1], state.camera.position[2]);
    // Initial calibration requires a momentarily settled camera. Three
    // distinct responses and both positions stable within2cm prevent a
    // vertically moving pilot camera becoming a permanent altitude bias.
    if (!state.calibration_samples || std::abs(delta - state.candidate_correction) > 0.02 ||
        body_math::distance(position, state.candidate_private) > 0.02 ||
        body_math::distance(public_position, state.candidate_public) > 0.02) {
      state.calibration_samples = 0;
      state.candidate_correction = delta;
      state.candidate_private = position;
      state.candidate_public = public_position;
    }
    state.calibration_camera_ms = state.camera_ms;
    if (++state.calibration_samples >= 3) {
      state.correction = delta;
      state.calibration_origin = body_math::ecef(state.aircraft.latitude, state.aircraft.longitude, 0);
      state.calibrated = true;
      accepted = true;
    }
  }
  ReleaseSRWLockExclusive(&state.lock);
  return accepted;
}
BodyPoseSnapshot sample_body_pose(std::uint64_t now) noexcept {
  BodyPoseSnapshot out;
  AcquireSRWLockShared(&state.lock);
  const auto locked_now = GetTickCount64();
  if (now < locked_now)
    now = locked_now;
  if (!fresh(state.aircraft_ms, now))
    out.error = state.aircraft_ms ? "aircraft_telemetry_stale" : state.error;
  else if (!state.calibrated) {
    out.calibration_required = fresh(state.camera_ms, now);
    out.error = out.calibration_required ? "local_camera_calibration_required" : "camera_telemetry_stale";
  } else if (body_math::distance(body_math::ecef(state.aircraft.latitude, state.aircraft.longitude, 0), state.calibration_origin) > 10000)
    out.error = "outside_local_calibration_radius";
  else {
    out.pose = body_math::body(state.aircraft, state.correction);
    out.sample_ms = state.aircraft_ms;
    out.valid = valid_body_pose(out.pose);
    out.error = out.valid ? "" : "invalid_body_basis";
  }
  ReleaseSRWLockShared(&state.lock);
  return out;
}
OnGroundSample get_on_ground() noexcept {
  AcquireSRWLockShared(&state.lock);
  const auto out = on_ground_locked(GetTickCount64());
  ReleaseSRWLockShared(&state.lock);
  return out;
}
GroundSpeedSample get_ground_speed() noexcept {
  AcquireSRWLockShared(&state.lock);
  const auto out = ground_speed_locked(GetTickCount64());
  ReleaseSRWLockShared(&state.lock);
  return out;
}
BodyTelemetryTiming get_body_telemetry_timing() noexcept {
  AcquireSRWLockShared(&state.lock);
  auto out = state.timing;
  out.fresh = fresh(state.aircraft_ms, GetTickCount64());
  ReleaseSRWLockShared(&state.lock);
  return out;
}
TaxiButtonSample get_taxi_buttons() noexcept {
  AcquireSRWLockShared(&state.lock);
  const auto out = taxi_buttons_locked(GetTickCount64());
  ReleaseSRWLockShared(&state.lock);
  return out;
}
LightingSample get_lighting() noexcept {
  AcquireSRWLockShared(&state.lock);
  const auto out = lighting_locked(GetTickCount64());
  ReleaseSRWLockShared(&state.lock);
  return out;
}
TaxiCutoffStatus get_taxi_cutoff() noexcept {
  AcquireSRWLockShared(&state.lock);
  const TaxiCutoffStatus out{state.speed_cutoff.inhibited(), state.speed_cutoff.pending(), state.cutoff_status};
  ReleaseSRWLockShared(&state.lock);
  return out;
}
#ifdef TAXI_BODY_POSE_PROVIDER_TESTING
namespace body_pose_provider_testing {
bool accept_on_ground_packet(const void* packet, std::uint32_t bytes, std::uint64_t sample_ms) noexcept {
  return ::taxi_camera::native_camera::accept_on_ground_packet(packet, bytes, sample_ms);
}
OnGroundSample on_ground_at(std::uint64_t now_ms) noexcept {
  AcquireSRWLockShared(&state.lock);
  const auto out = on_ground_locked(now_ms);
  ReleaseSRWLockShared(&state.lock);
  return out;
}
bool accept_session_packet(const void* packet, std::uint32_t bytes) noexcept {
  return native_camera::accept_session_packet(packet, bytes);
}
bool accept_identity_packet(const void* packet, std::uint32_t bytes, std::uint64_t now) noexcept {
  return native_camera::accept_identity_packet(packet, bytes, now);
}
bool accept_aircraft_packet(const void* packet, std::uint32_t bytes, std::uint64_t sample_ms) noexcept {
  return native_camera::accept_aircraft_packet(packet, bytes, sample_ms);
}
GroundSpeedSample ground_speed_at(std::uint64_t now_ms) noexcept {
  AcquireSRWLockShared(&state.lock);
  const auto out = ground_speed_locked(now_ms);
  ReleaseSRWLockShared(&state.lock);
  return out;
}
bool accept_taxi_packet(const void* packet, std::uint32_t bytes, std::uint64_t sample_ms) noexcept {
  return native_camera::accept_taxi_packet(packet, bytes, sample_ms);
}
TaxiButtonSample taxi_buttons_at(std::uint64_t now_ms) noexcept {
  AcquireSRWLockShared(&state.lock);
  const auto out = taxi_buttons_locked(now_ms);
  ReleaseSRWLockShared(&state.lock);
  return out;
}
bool accept_lighting_packet(const void* packet, std::uint32_t bytes, std::uint64_t sample_ms) noexcept {
  return native_camera::accept_lighting_packet(packet, bytes, sample_ms);
}
LightingSample lighting_at(std::uint64_t now_ms) noexcept {
  AcquireSRWLockShared(&state.lock);
  const auto out = lighting_locked(now_ms);
  ReleaseSRWLockShared(&state.lock);
  return out;
}
}  // namespace body_pose_provider_testing
#endif
}  // namespace taxi_camera::native_camera
