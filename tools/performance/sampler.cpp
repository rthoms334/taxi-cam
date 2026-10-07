#include <windows.h>

#include <bcrypt.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../src/shared/protocol.hpp"

namespace taxi_performance {
namespace ipc = taxi_camera::standalone;
struct Handle {
  HANDLE value{};
  explicit Handle(HANDLE h = nullptr) : value(h) {}
  ~Handle() {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
std::uint64_t number(FILETIME value) {
  return (std::uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
std::string utf8(const std::wstring& value) {
  if (value.empty())
    return {};
  const auto size =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (!size)
    throw std::runtime_error("Cannot encode Windows path");
  std::string out(size, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
  return out;
}
std::string json(const std::string& value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\')
      out << '\\' << c;
    else if (c < 32)
      out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
    else
      out << c;
  }
  return out.str() + '"';
}
std::string json(const std::wstring& value) {
  return json(utf8(value));
}
std::string utc() {
  SYSTEMTIME t{};
  GetSystemTime(&t);
  char out[40]{};
  std::snprintf(out, sizeof(out), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                t.wMilliseconds);
  return out;
}
double clock_ms() {
  LARGE_INTEGER frequency{}, tick{};
  if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&tick))
    throw std::runtime_error("Performance clock unavailable");
  return 1000.0 * static_cast<double>(tick.QuadPart) / static_cast<double>(frequency.QuadPart);
}
std::wstring process_path(HANDLE process) {
  std::wstring value(32768, L'\0');
  DWORD length = static_cast<DWORD>(value.size());
  if (!QueryFullProcessImageNameW(process, 0, value.data(), &length))
    throw std::runtime_error("Cannot read process image path");
  value.resize(length);
  return value;
}
bool named(const std::wstring& path, const wchar_t* name) {
  return _wcsicmp(path.substr(path.find_last_of(L"\\/") + 1).c_str(), name) == 0;
}
struct Cpu {
  std::uint64_t created{}, user{}, kernel{};
  double at{};
};
Cpu process_cpu(HANDLE process) {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user) || number(exited))
    throw std::runtime_error("Target process exited or CPU counters unavailable");
  return {number(created), number(user), number(kernel), clock_ms()};
}
std::string sha256(const std::wstring& path, bool required = true) {
  Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!file) {
    if (!required)
      return "unreadable";
    throw std::runtime_error("Cannot open verified binary for hashing");
  }
  BCRYPT_ALG_HANDLE algorithm{};
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
    throw std::runtime_error("SHA256 unavailable");
  struct AlgorithmGuard {
    BCRYPT_ALG_HANDLE value;
    ~AlgorithmGuard() { BCryptCloseAlgorithmProvider(value, 0); }
  } algorithm_guard{algorithm};
  BCRYPT_HASH_HANDLE hash{};
  if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
    throw std::runtime_error("Cannot create SHA256 hash");
  struct HashGuard {
    BCRYPT_HASH_HANDLE value;
    ~HashGuard() { BCryptDestroyHash(value); }
  } hash_guard{hash};
  std::array<unsigned char, 65536> buffer{};
  DWORD read{};
  do {
    if (!ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
      throw std::runtime_error("Cannot read verified binary");
    if (read && BCryptHashData(hash, buffer.data(), read, 0) < 0)
      throw std::runtime_error("Cannot hash verified binary");
  } while (read);
  std::array<unsigned char, 32> result{};
  if (BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) < 0)
    throw std::runtime_error("Cannot finish SHA256");
  std::ostringstream out;
  for (auto c : result)
    out << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
  return out.str();
}
struct Module {
  std::wstring path;
  std::uintptr_t base{};
  DWORD size{};
};
// required=false is the explicit unloaded-baseline mode: an absent bridge is the
// expected result and a present one is refused, so the window is provably unloaded.
Module bridge_module(DWORD pid, bool required = true) {
  Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!snapshot || !Module32FirstW(snapshot.value, &entry))
    throw std::runtime_error("Cannot enumerate target modules");
  Module found;
  do {
    if (_wcsicmp(entry.szModule, L"taxi-camera-bridge.dll"))
      continue;
    if (found.base)
      throw std::runtime_error("Multiple bridge modules found");
    found = {entry.szExePath, reinterpret_cast<std::uintptr_t>(entry.modBaseAddr), entry.modBaseSize};
  } while (Module32NextW(snapshot.value, &entry));
  if (!required && found.base)
    throw std::runtime_error("Taxi Cam bridge is loaded; the unloaded baseline refuses this window");
  if (required && !found.base)
    throw std::runtime_error("Taxi Cam bridge is not loaded in the explicit target PID");
  return found;
}
bool fresh(std::uint64_t tick, std::uint64_t now) {
  return tick && now >= tick && now - tick <= 5000;
}
template <std::size_t N>
bool terminated(const char (&value)[N]) {
  return std::memchr(value, 0, N) != nullptr;
}
struct IpcSnapshot {
  ipc::Shared value{};
  std::string error;
  bool valid() const { return error.empty(); }
};
class ReadOnlyIpc {
 public:
  explicit ReadOnlyIpc(DWORD target) {
    wchar_t name[128]{};
    std::swprintf(name, 128, L"Local\\380TaxiCamera.Control.%lu", target);
    mutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
    std::swprintf(name, 128, L"Local\\380TaxiCamera.Data.%lu", target);
    mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (mapping_)
      view_ = static_cast<const ipc::Shared*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, sizeof(ipc::Shared)));
  }
  ~ReadOnlyIpc() {
    if (view_)
      UnmapViewOfFile(view_);
    if (mapping_)
      CloseHandle(mapping_);
    if (mutex_)
      CloseHandle(mutex_);
  }
  ReadOnlyIpc(const ReadOnlyIpc&) = delete;
  ReadOnlyIpc& operator=(const ReadOnlyIpc&) = delete;
  IpcSnapshot read() const {
    IpcSnapshot out;
    if (!mutex_ || !view_) {
      out.error = "ipc_unavailable";
      return out;
    }
    const auto wait = WaitForSingleObject(mutex_, 0);
    if (wait != WAIT_OBJECT_0) {
      if (wait == WAIT_ABANDONED)
        ReleaseMutex(mutex_);
      out.error = wait == WAIT_TIMEOUT ? "ipc_busy" : wait == WAIT_ABANDONED ? "ipc_abandoned" : "ipc_lock_failed";
      return out;
    }
    std::memcpy(&out.value, view_, sizeof(out.value));
    const bool released = ReleaseMutex(mutex_) != FALSE;
    if (!released) {
      out.error = "ipc_unlock_failed";
      return out;
    }
    const auto& value = out.value;
    const auto& s = value.status;
    const auto now = GetTickCount64();
    if (value.magic != ipc::ProtocolMagic || value.version != ipc::ProtocolVersion || value.bytes != sizeof(ipc::Shared))
      out.error = "ipc_header_invalid";
    else if (!value.owner_pid || !ipc::valid_settings(value.settings))
      out.error = "ipc_settings_invalid";
    else if (!fresh(value.owner_heartbeat, now) || !fresh(s.heartbeat, now))
      out.error = "ipc_stale_or_future";
    else if ((s.taxi_mask & ~taxi_camera::AllDisplaySides) || s.candidate_count > 16 || s.graphics_ready > 1 || s.scene_ready > 1 || !terminated(s.aircraft_type) ||
             !terminated(s.aircraft_path) || !terminated(s.message) || !std::isfinite(s.speed) || !std::isfinite(s.exposure) ||
             !std::isfinite(s.probe_cpu_ms) || !std::isfinite(s.probe_max_ms))
      out.error = "ipc_status_invalid";
    for (double stage : s.stage_ms)
      if (!std::isfinite(stage))
        out.error = "ipc_status_invalid";
    return out;
  }

 private:
  HANDLE mutex_{}, mapping_{};
  const ipc::Shared* view_{};
};
template <class Values>
void array_json(std::ostream& out, const Values& values) {
  out << '[';
  bool comma = false;
  for (const auto& value : values) {
    if (comma)
      out << ',';
    comma = true;
    out << value;
  }
  out << ']';
}
std::string settings_json(const ipc::Settings& s) {
  std::ostringstream out;
  out << std::setprecision(17);
  out << "{\"enabled\":" << s.enabled << ",\"camera_rate\":" << s.camera_rate << ",\"camera_mode\":" << s.camera_mode
      << ",\"profile\":" << s.profile << ",\"auto_profile\":" << s.auto_profile << ",\"follow_taxi\":" << s.follow_taxi
      << ",\"auto_detect\":" << s.auto_detect << ",\"single_camera\":" << s.single_camera << ",\"manual_mask\":" << s.manual_mask
      << ",\"calibration_mask\":" << s.calibration_mask << ",\"calibration_budget\":" << s.calibration_budget
      << ",\"scene_test\":" << s.scene_test << ",\"route_request\":" << s.route_request << ",\"left_id\":" << s.left_id
      << ",\"right_id\":" << s.right_id << ",\"profile_request\":" << s.profile_request
      << ",\"aircraft_session_epoch\":" << s.aircraft_session_epoch << ",\"taxi_request\":" << s.taxi_request
      << ",\"taxi_selected_mask\":" << s.taxi_selected_mask << ",\"taxi_desired_mask\":" << s.taxi_desired_mask << ",\"speed_color\":";
  array_json(out, s.speed_color);
  out << ",\"nose_dot\":";
  array_json(out, s.nose_dot);
  out << ",\"tail_upper\":";
  array_json(out, s.tail_upper);
  out << ",\"tail_corner\":";
  array_json(out, s.tail_corner);
  out << ",\"tail_inner\":";
  array_json(out, s.tail_inner);
  out << ",\"mounts\":[";
  array_json(out, s.mounts[0]);
  out << ',';
  array_json(out, s.mounts[1]);
  out << ',';
  array_json(out, s.mounts[2]);
  out << "]}";
  return out.str();
}
std::string status_json(const ipc::Status& s) {
  std::ostringstream out;
  out << std::setprecision(17);
  out << "{\"heartbeat\":" << s.heartbeat << ",\"graphics_ready\":" << s.graphics_ready << ",\"scene_ready\":" << s.scene_ready
      << ",\"taxi_mask\":" << s.taxi_mask << ",\"active_profile\":" << s.active_profile << ",\"detected_profile\":" << s.detected_profile
      << ",\"effective_rate\":" << s.effective_rate << ",\"useful_rate\":" << s.useful_rate << ",\"rate_limits\":" << s.rate_limits
      << ",\"update_hz\":" << s.update_hz << ",\"reachable_rate\":" << s.reachable_rate << ",\"auto_level\":" << s.auto_level
      << ",\"aircraft_session_epoch\":" << s.aircraft_session_epoch << ",\"identity_sample_ms\":" << s.identity_sample_ms
      << ",\"aircraft_type\":" << json(s.aircraft_type) << ",\"aircraft_path\":" << json(s.aircraft_path) << ",\"captures\":" << s.captures
      << ",\"composed\":" << s.composed << ",\"stamps\":" << s.stamps << ",\"left_id\":" << s.left_id << ",\"right_id\":" << s.right_id
      << ",\"hook_failures\":" << s.hook_failures << ",\"speed\":" << s.speed << ",\"exposure\":" << s.exposure
      << ",\"probe_elapsed_ms\":" << s.probe_cpu_ms << ",\"probe_max_elapsed_ms\":" << s.probe_max_ms << ",\"stage_elapsed_ms\":";
  array_json(out, s.stage_ms);
  out << ",\"message\":" << json(s.message) << '}';
  return out.str();
}
struct Focus {
  HWND initial{};
  DWORD target{};
  bool changed{};
  unsigned events{};
  static DWORD pid(HWND window) {
    DWORD id{};
    if (window)
      GetWindowThreadProcessId(window, &id);
    return id;
  }
  void observe(HWND window, DWORD owner) {
    if (window != initial || owner != target)
      changed = true;
  }
  void event(HWND window, DWORD owner) {
    ++events;
    observe(window, owner);
  }
};
Focus* active_focus{};
void CALLBACK foreground_event(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD) {
  if (active_focus)
    active_focus->event(window, Focus::pid(window));
}
struct FocusHook {
  HWINEVENTHOOK value{};
  explicit FocusHook(Focus& focus) {
    active_focus = &focus;
    value = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, foreground_event, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!value) {
      active_focus = nullptr;
      throw std::runtime_error("Foreground transition observation unavailable");
    }
  }
  ~FocusHook() {
    if (value)
      UnhookWinEvent(value);
    active_focus = nullptr;
  }
};
void pump() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}
struct ThreadSample {
  DWORD id{};
  Cpu cpu{};
  std::wstring name;
};
std::vector<ThreadSample> threads(DWORD pid, unsigned& failures) {
  Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  if (!snapshot || !Thread32First(snapshot.value, &entry))
    throw std::runtime_error("Cannot enumerate target threads");
  std::vector<ThreadSample> out;
  do {
    if (entry.th32OwnerProcessID != pid)
      continue;
    Handle thread(OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID));
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!thread || GetProcessIdOfThread(thread.value) != pid || !GetThreadTimes(thread.value, &created, &exited, &kernel, &user) ||
        number(exited)) {
      ++failures;
      continue;
    }
    PWSTR description{};
    std::wstring name;
    if (SUCCEEDED(GetThreadDescription(thread.value, &description)) && description) {
      name = description;
      LocalFree(description);
    }
    out.push_back({entry.th32ThreadID, {number(created), number(user), number(kernel), clock_ms()}, name});
  } while (Thread32Next(snapshot.value, &entry));
  return out;
}
struct ThreadTotal {
  ThreadSample last;
  std::uint64_t user{}, kernel{};
  unsigned intervals{};
};
using ThreadKey = std::pair<DWORD, std::uint64_t>;
struct Options {
  DWORD pid{}, profile{}, mask{};
  unsigned duration_ms = 30000, interval_ms = 250;
  std::wstring output, phase;
  // Unloaded baseline: the bridge must be absent for the whole window, the
  // companion is optional, and no IPC readiness/mask/counter validation runs
  // because no bridge publishes status. Mask must be 0.
  bool allow_unloaded = false;
};
unsigned integer(const std::wstring& value) {
  if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
    throw std::runtime_error("Expected unsigned integer argument");
  const auto parsed = std::stoull(value);
  if (parsed > UINT32_MAX)
    throw std::runtime_error("Integer argument is too large");
  return static_cast<unsigned>(parsed);
}
Options arguments(int argc, wchar_t** argv) {
  Options options;
  bool mask = false;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 >= argc)
      throw std::runtime_error("Expected named argument and value");
    const std::wstring key = argv[i], value = argv[i + 1];
    if (key == L"--pid")
      options.pid = integer(value);
    else if (key == L"--profile")
      options.profile = integer(value);
    else if (key == L"--mask") {
      options.mask = integer(value);
      mask = true;
    } else if (key == L"--duration-ms")
      options.duration_ms = integer(value);
    else if (key == L"--interval-ms")
      options.interval_ms = integer(value);
    else if (key == L"--allow-unloaded")
      options.allow_unloaded = integer(value) == 1;
    else if (key == L"--output")
      options.output = value;
    else if (key == L"--phase")
      options.phase = value;
    else
      throw std::runtime_error("Unknown sampler argument");
  }
  if (!options.pid || !taxi_camera::profiles::find(options.profile) || !mask || options.mask > taxi_camera::AllDisplaySides || options.output.empty() ||
      options.phase.empty() || options.phase.size() > 64 || options.duration_ms < 1000 || options.duration_ms > 120000 ||
      options.interval_ms < 100 || options.interval_ms > 1000)
    throw std::runtime_error(
        "Require explicit --pid, --profile, --mask, --phase and --output; duration 1000..120000ms, interval 100..1000ms");
  if (options.allow_unloaded && options.mask)
    throw std::runtime_error("--allow-unloaded 1 requires --mask 0");
  return options;
}
std::string validate_capture(const IpcSnapshot& snapshot, const Options& options) {
  if (!snapshot.valid())
    return snapshot.error;
  const auto& s = snapshot.value.status;
  const auto& settings = snapshot.value.settings;
  if (!s.graphics_ready || !s.scene_ready)
    return "scene_not_ready";
  if (settings.scene_test || settings.calibration_mask || settings.single_camera)
    return "diagnostic_mode_enabled";
  if (!fresh(s.identity_sample_ms, GetTickCount64()) || s.active_profile != options.profile || s.detected_profile != options.profile ||
      settings.profile != options.profile)
    return "aircraft_identity_unready_or_changed";
  // A bridge attached into an already loaded flight reports epoch 0 until the
  // next session event; the companion mirrors it. Only disagreement is a mismatch.
  if (settings.aircraft_session_epoch != s.aircraft_session_epoch)
    return "session_mismatch";
  if (s.taxi_mask != options.mask)
    return "unexpected_taxi_mask";
  if (!options.mask && settings.enabled) {
    const auto* profile = taxi_camera::profiles::find(settings.profile);
    const bool follow = settings.follow_taxi && profile && profile->taxi_control != taxi_camera::profiles::TaxiControl::manual_only;
    if (follow && (!s.taxi_buttons_valid || !fresh(s.taxi_buttons_sample_ms, GetTickCount64())))
      return "off_button_state_unready";
    if (follow ? s.taxi_buttons_mask != 0 : settings.manual_mask != 0)
      return "off_foreground_render_demand_present";
  }
  return {};
}
std::string counter_issue(const ipc::Status& previous, const ipc::Status& current, unsigned mask) {
  if (current.captures < previous.captures || current.composed < previous.composed || current.stamps < previous.stamps)
    return "delivery_counter_reversal";
  if (!mask && (current.captures != previous.captures || current.composed != previous.composed || current.stamps != previous.stamps))
    return "off_background_or_unsettled_work";
  return {};
}
void write_line(std::ofstream& out, const std::string& line) {
  out << line << '\n';
  out.flush();
  if (!out)
    throw std::runtime_error("Cannot write capture evidence");
}
std::string strings_json(const std::vector<std::string>& values) {
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i)
      out << ',';
    out << json(values[i]);
  }
  return out.str() + ']';
}
int capture(const Options& options) {
  if (!std::filesystem::is_directory(std::filesystem::path(options.output)) ||
      std::filesystem::exists(std::filesystem::path(options.output + L"/samples.jsonl")) ||
      std::filesystem::exists(std::filesystem::path(options.output + L"/summary.json")))
    throw std::runtime_error("Use an existing output directory without prior sampler evidence");
  Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, options.pid));
  if (!process)
    throw std::runtime_error("Cannot open explicit target PID for read-only counters");
  const auto path = process_path(process.value);
  if (!named(path, L"FlightSimulator2024.exe"))
    throw std::runtime_error("Explicit PID is not FlightSimulator2024.exe; no capture performed");
  const auto module = bridge_module(options.pid, !options.allow_unloaded);
  ReadOnlyIpc mailbox(options.pid);
  const auto initial = mailbox.read();
  // Without a bridge nothing publishes readiness, mask or delivery counters:
  // the unloaded baseline records IPC when present and validates none of it.
  const auto initial_error = options.allow_unloaded ? std::string() : validate_capture(initial, options);
  if (!initial_error.empty())
    throw std::runtime_error("IPC preflight refused: " + initial_error);
  Handle companion(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, initial.value.owner_pid));
  if (!companion && !options.allow_unloaded)
    throw std::runtime_error("Companion owner PID unavailable");
  std::wstring companion_path;
  decltype(Cpu{}.created) companion_created{};
  std::string companion_hash = "absent";
  if (companion) {
    companion_path = process_path(companion.value);
    if (!named(companion_path, L"taxi-cam.exe"))
      throw std::runtime_error("IPC owner is not the Taxi Cam companion");
    companion_created = process_cpu(companion.value).created;
    companion_hash = sha256(companion_path);
  }
  // Hashing happens outside the CPU interval; these are backing files, not in-memory images.
  // Store/WindowsApps simulator images may deny CreateFile; bridge + companion remain required
  // whenever they are present.
  const auto image_hash = sha256(path, false);
  const std::string bridge_hash = module.base ? sha256(module.path) : "absent";
  std::ofstream samples(std::filesystem::path(options.output + L"/samples.jsonl"), std::ios::binary | std::ios::trunc);
  if (!samples)
    throw std::runtime_error("Cannot create samples.jsonl");
  const auto setting_identity = settings_json(initial.value.settings);
  std::ostringstream metadata;
  metadata << "{\"type\":\"metadata\",\"schema\":1,\"utc\":" << json(utc()) << ",\"phase\":" << json(options.phase)
           << ",\"pid\":" << options.pid << ",\"process_path\":" << json(path) << ",\"process_sha256\":" << json(image_hash)
           << ",\"unloaded\":" << (options.allow_unloaded ? "true" : "false") << ",\"bridge_path\":" << json(module.path)
           << ",\"bridge_base\":" << module.base << ",\"bridge_size\":" << module.size << ",\"bridge_sha256\":" << json(bridge_hash)
           << ",\"companion_pid\":" << initial.value.owner_pid << ",\"companion_path\":" << json(companion_path)
           << ",\"companion_sha256\":" << json(companion_hash) << ",\"companion_created_filetime\":" << companion_created
           << ",\"protocol\":" << ipc::ProtocolVersion << ",\"shared_bytes\":" << sizeof(ipc::Shared)
           << ",\"expected_profile\":" << options.profile << ",\"expected_mask\":" << options.mask
           << ",\"duration_ms\":" << options.duration_ms << ",\"interval_ms\":" << options.interval_ms
           << ",\"settings\":" << setting_identity << '}';
  write_line(samples, metadata.str());
  pump();  // Establish the message queue before registering the out-of-context event hook.
  Focus focus{GetForegroundWindow(), options.pid};
  focus.observe(focus.initial, Focus::pid(focus.initial));
  FocusHook hook(focus);
  std::vector<std::string> issues;
  const auto issue = [&](const std::string& value) {
    if (std::find(issues.begin(), issues.end(), value) == issues.end())
      issues.push_back(value);
  };
  std::map<ThreadKey, ThreadTotal> totals;
  const Cpu first = process_cpu(process.value);
  Cpu last = first;
  unsigned sample_count = 0, read_failures = 0, invalid_snapshots = 0;
  ipc::Status first_status{}, previous_status{};
  bool have_status = false;
  const auto started = first.at;
  try {
    for (unsigned index = 0;; ++index) {
      const auto deadline = started + std::min<std::uint64_t>(std::uint64_t(index) * options.interval_ms, options.duration_ms);
      while (clock_ms() < deadline) {
        pump();
        const auto remaining = std::max(1.0, std::ceil(deadline - clock_ms()));
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(std::min(remaining, 1000.0)), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
      }
      pump();
      const auto foreground = GetForegroundWindow();
      focus.observe(foreground, Focus::pid(foreground));
      const auto cpu = index ? process_cpu(process.value) : first;
      last = cpu;
      if (cpu.created != first.created)
        issue("process_identity_changed");
      const auto snapshot = mailbox.read();
      const auto error = options.allow_unloaded ? std::string() : validate_capture(snapshot, options);
      if (!error.empty()) {
        issue(error);
        ++invalid_snapshots;
      }
      if (snapshot.valid()) {
        if (settings_json(snapshot.value.settings) != setting_identity)
          issue("settings_changed");
        if (snapshot.value.owner_pid != initial.value.owner_pid ||
            snapshot.value.status.aircraft_session_epoch != initial.value.status.aircraft_session_epoch ||
            std::strcmp(snapshot.value.status.aircraft_type, initial.value.status.aircraft_type) ||
            std::strcmp(snapshot.value.status.aircraft_path, initial.value.status.aircraft_path))
          issue("session_or_owner_changed");
        if (snapshot.value.status.left_id != initial.value.status.left_id ||
            snapshot.value.status.right_id != initial.value.status.right_id)
          issue("display_target_identity_changed");
        if (have_status) {
          const auto counters = counter_issue(previous_status, snapshot.value.status, options.mask);
          if (!counters.empty())
            issue(counters);
        } else
          first_status = snapshot.value.status;
        previous_status = snapshot.value.status;
        have_status = true;
      }
      std::ostringstream row;
      row << std::setprecision(17);
      row << "{\"type\":\"sample\",\"utc\":" << json(utc()) << ",\"tick_ms\":" << GetTickCount64()
          << ",\"elapsed_ms\":" << cpu.at - first.at << ",\"pid\":" << options.pid << ",\"process_created_filetime\":" << cpu.created
          << ",\"process_user_100ns\":" << cpu.user << ",\"process_kernel_100ns\":" << cpu.kernel
          << ",\"foreground_pid\":" << Focus::pid(GetForegroundWindow()) << ",\"focus_invalid\":" << (focus.changed ? "true" : "false")
          << ",\"focus_events\":" << focus.events << ",\"ipc_valid\":" << (snapshot.valid() ? "true" : "false")
          << ",\"ipc_issue\":" << json(error) << ",\"capture_state_valid\":" << (error.empty() ? "true" : "false");
      if (snapshot.valid())
        row << ",\"owner_heartbeat\":" << snapshot.value.owner_heartbeat << ",\"settings\":" << settings_json(snapshot.value.settings)
            << ",\"status\":" << status_json(snapshot.value.status);
      row << ",\"threads\":[";
      bool comma = false;
      for (const auto& thread : threads(options.pid, read_failures)) {
        if (comma)
          row << ',';
        comma = true;
        row << "{\"id\":" << thread.id << ",\"created_filetime\":" << thread.cpu.created << ",\"name\":" << json(thread.name)
            << ",\"sample_elapsed_ms\":" << thread.cpu.at - first.at << ",\"user_100ns\":" << thread.cpu.user
            << ",\"kernel_100ns\":" << thread.cpu.kernel << '}';
        const ThreadKey key{thread.id, thread.cpu.created};
        auto found = totals.find(key);
        if (found != totals.end()) {
          auto& total = found->second;
          if (thread.cpu.user < total.last.cpu.user || thread.cpu.kernel < total.last.cpu.kernel)
            issue("thread_counter_reversal");
          else {
            total.user += thread.cpu.user - total.last.cpu.user;
            total.kernel += thread.cpu.kernel - total.last.cpu.kernel;
            ++total.intervals;
          }
          total.last = thread;
        } else
          totals.emplace(key, ThreadTotal{thread, 0, 0, 0});
      }
      row << "]}";
      write_line(samples, row.str());
      ++sample_count;
      if (cpu.at - started >= options.duration_ms)
        break;
    }
    pump();
    const auto foreground = GetForegroundWindow();
    focus.observe(foreground, Focus::pid(foreground));
    if (focus.changed)
      issue("foreground_transition_or_not_foreground");
    if (companion && process_cpu(companion.value).created != companion_created)
      issue("companion_identity_changed");
    // In the unloaded mode a bridge that appeared during the window throws here and invalidates the capture.
    const auto final_module = bridge_module(options.pid, !options.allow_unloaded);
    if (final_module.path != module.path || final_module.base != module.base || final_module.size != module.size)
      issue("bridge_module_changed");
    if (sha256(path, false) != image_hash || (module.base && sha256(module.path) != bridge_hash) ||
        (companion && sha256(companion_path) != companion_hash))
      issue("binary_backing_file_changed");
  } catch (const std::exception& error) {
    issue(error.what());
  }
  if (focus.changed)
    issue("foreground_transition_or_not_foreground");
  const double elapsed = sample_count > 1 ? (last.at - first.at) / 1000.0 : 0;
  if (elapsed <= 0 || elapsed * 1000 < options.duration_ms)
    issue("incomplete_capture");
  if (last.user < first.user || last.kernel < first.kernel)
    issue("process_counter_reversal");
  if (options.mask && have_status &&
      (previous_status.captures <= first_status.captures || previous_status.composed <= first_status.composed ||
       previous_status.stamps <= first_status.stamps))
    issue("on_delivery_progress_not_observed");
  std::ostringstream summary;
  summary << std::setprecision(17);
  summary << "{\"type\":\"summary\",\"schema\":1,\"valid\":" << (issues.empty() ? "true" : "false") << ",\"phase\":" << json(options.phase)
          << ",\"unloaded\":" << (options.allow_unloaded ? "true" : "false") << ",\"pid\":" << options.pid
          << ",\"process_created_filetime\":" << first.created << ",\"samples\":" << sample_count << ",\"elapsed_seconds\":" << elapsed
          << ",\"invalid_ipc_samples\":" << invalid_snapshots << ",\"thread_read_failures\":" << read_failures
          << ",\"focus_events\":" << focus.events << ",\"issues\":" << strings_json(issues);
  if (elapsed > 0 && last.user >= first.user && last.kernel >= first.kernel)
    summary << ",\"process_user_ms_per_second\":" << (last.user - first.user) / 10000.0 / elapsed
            << ",\"process_kernel_ms_per_second\":" << (last.kernel - first.kernel) / 10000.0 / elapsed;
  summary << ",\"threads\":[";
  bool comma = false;
  for (const auto& [key, total] : totals) {
    if (comma)
      summary << ',';
    comma = true;
    summary << "{\"id\":" << key.first << ",\"created_filetime\":" << key.second << ",\"name\":" << json(total.last.name)
            << ",\"observed_intervals\":" << total.intervals << ",\"user_ms\":" << total.user / 10000.0
            << ",\"kernel_ms\":" << total.kernel / 10000.0 << '}';
  }
  summary << "],\"thread_coverage\":\"Observed thread lifetimes only; process totals include threads missed between "
             "polls.\",\"scope\":\"CPU counters only; no FPS/GPU or scene-stability claim. IPC probe timings are last serviced elapsed "
             "callback times. Binary hashes identify backing files.\"}";
  write_line(samples, summary.str());
  std::ofstream summary_file(std::filesystem::path(options.output + L"/summary.json"), std::ios::binary | std::ios::trunc);
  write_line(summary_file, summary.str());
  std::puts(issues.empty() ? "Capture complete: valid CPU/IPC/focus interval. Review summary.json and scene comparability."
                           : "Capture invalid: see summary.json issues; do not use for before/after claims.");
  return issues.empty() ? 0 : 2;
}
}  // namespace taxi_performance

#ifndef TAXI_PERFORMANCE_TESTING
int wmain(int argc, wchar_t** argv) {
  try {
    return taxi_performance::capture(taxi_performance::arguments(argc, argv));
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Sampler refused: %s\n", error.what());
    return 1;
  }
}
#endif
