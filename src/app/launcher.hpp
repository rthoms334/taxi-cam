#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <winreg.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>
#include "module_inventory.hpp"
#include "process_elevation.hpp"

namespace taxi_camera::standalone {
inline bool same_path(const std::wstring& a, const std::wstring& b) {
  if (a.empty() || b.empty())
    return false;
  if (_wcsicmp(a.c_str(), b.c_str()) == 0)
    return true;
  // XboxGames and WindowsApps can expose the same MSFS executable under
  // different paths. Accept aliases only when both handles identify one file.
  // https://learn.microsoft.com/windows/win32/api/winbase/ns-winbase-file_id_info
  constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  HANDLE first = CreateFileW(a.c_str(), FILE_READ_ATTRIBUTES, sharing, nullptr, OPEN_EXISTING, 0, nullptr);
  if (first == INVALID_HANDLE_VALUE)
    return false;
  HANDLE second = CreateFileW(b.c_str(), FILE_READ_ATTRIBUTES, sharing, nullptr, OPEN_EXISTING, 0, nullptr);
  FILE_ID_INFO first_id{}, second_id{};
  const bool same =
      second != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(first, FileIdInfo, &first_id, sizeof(first_id)) &&
      GetFileInformationByHandleEx(second, FileIdInfo, &second_id, sizeof(second_id)) &&
      first_id.VolumeSerialNumber == second_id.VolumeSerialNumber &&
      std::equal(std::begin(first_id.FileId.Identifier), std::end(first_id.FileId.Identifier), std::begin(second_id.FileId.Identifier));
  if (second != INVALID_HANDLE_VALUE)
    CloseHandle(second);
  CloseHandle(first);
  return same;
}
inline bool same_user(HANDLE process) {
  HANDLE own{}, other{};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own))
    return false;
  if (!OpenProcessToken(process, TOKEN_QUERY, &other)) {
    CloseHandle(own);
    return false;
  }
  DWORD na{}, nb{};
  GetTokenInformation(own, TokenUser, nullptr, 0, &na);
  GetTokenInformation(other, TokenUser, nullptr, 0, &nb);
  std::vector<unsigned char> a(na), b(nb);
  const bool ok = na && nb && GetTokenInformation(own, TokenUser, a.data(), na, &na) &&
                  GetTokenInformation(other, TokenUser, b.data(), nb, &nb) &&
                  EqualSid(reinterpret_cast<TOKEN_USER*>(a.data())->User.Sid, reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid);
  CloseHandle(own);
  CloseHandle(other);
  return ok;
}
inline std::wstring normalize_path_separators(std::wstring path) {
  for (auto& c : path)
    if (c == L'/')
      c = L'\\';
  while (path.size() > 3 && path.back() == L'\\')
    path.pop_back();
  return path;
}
inline bool simulator_exe_name(const std::wstring& path) {
  const auto slash = path.find_last_of(L"\\/");
  const auto* leaf = slash == std::wstring::npos ? path.c_str() : path.c_str() + slash + 1;
  return _wcsicmp(leaf, L"FlightSimulator2024.exe") == 0;
}
inline bool contains_ci(const std::wstring& haystack, const wchar_t* needle) {
  const size_t n = std::wcslen(needle);
  if (!n || haystack.size() < n)
    return false;
  return std::search(haystack.begin(), haystack.end(), needle, needle + n, [](wchar_t a, wchar_t b) {
           return towlower(static_cast<wint_t>(a)) == towlower(static_cast<wint_t>(b));
         }) != haystack.end();
}
inline bool known_msfs2024_layout(const std::wstring& path) {
  if (!simulator_exe_name(path))
    return false;
  const auto normalized = normalize_path_separators(path);
  return contains_ci(normalized, L"\\XboxGames\\Microsoft Flight Simulator 2024\\Content\\FlightSimulator2024.exe") ||
         contains_ci(normalized, L"\\steamapps\\common\\MSFS2024\\FlightSimulator2024.exe") ||
         contains_ci(normalized, L"\\steamapps\\common\\Limitless\\FlightSimulator2024.exe") ||
         (contains_ci(normalized, L"\\WindowsApps\\Microsoft.Limitless_") && contains_ci(normalized, L"\\FlightSimulator2024.exe"));
}
inline bool file_exists(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
inline void add_unique_path(std::vector<std::wstring>& out, const std::wstring& path) {
  const auto normalized = normalize_path_separators(path);
  if (normalized.empty())
    return;
  for (const auto& existing : out)
    if (_wcsicmp(existing.c_str(), normalized.c_str()) == 0)
      return;
  out.push_back(normalized);
}
inline void add_existing_simulator(std::vector<std::wstring>& out, const std::wstring& path) {
  if (file_exists(path))
    add_unique_path(out, path);
}
inline void add_steam_library_simulators(std::vector<std::wstring>& out, const std::wstring& library_root) {
  const auto root = normalize_path_separators(library_root);
  if (root.empty())
    return;
  add_existing_simulator(out, root + L"\\steamapps\\common\\MSFS2024\\FlightSimulator2024.exe");
  add_existing_simulator(out, root + L"\\steamapps\\common\\Limitless\\FlightSimulator2024.exe");
}
inline bool is_vdf_space(wchar_t c) {
  return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}
inline std::vector<std::wstring> steam_library_roots_from_vdf(const std::wstring& text) {
  std::vector<std::wstring> roots;
  for (size_t i = 0; i + 6 <= text.size(); ++i) {
    if (_wcsnicmp(text.c_str() + i, L"\"path\"", 6) != 0)
      continue;
    if (i > 0 && !is_vdf_space(text[i - 1]) && text[i - 1] != L'{' && text[i - 1] != L'}')
      continue;
    size_t pos = i + 6;
    while (pos < text.size() && is_vdf_space(text[pos]))
      ++pos;
    if (pos >= text.size() || text[pos] != L'"')
      continue;
    ++pos;
    std::wstring raw;
    while (pos < text.size() && text[pos] != L'"') {
      if (text[pos] == L'\\' && pos + 1 < text.size()) {
        raw.push_back(text[pos + 1]);
        pos += 2;
        continue;
      }
      raw.push_back(text[pos++]);
    }
    add_unique_path(roots, raw);
  }
  return roots;
}
inline std::wstring registry_string(HKEY root, const wchar_t* subkey, const wchar_t* value) {
  wchar_t buffer[32768]{};
  DWORD size = sizeof(buffer);
  if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
    return {};
  return buffer;
}
inline std::wstring steam_install_root() {
  auto path = registry_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
  if (path.empty())
    path = registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
  if (path.empty())
    path = L"C:\\Program Files (x86)\\Steam";
  return normalize_path_separators(path);
}
inline std::wstring read_small_text_file(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return {};
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
    CloseHandle(file);
    return {};
  }
  std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  const bool ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
  CloseHandle(file);
  if (!ok)
    return {};
  size_t start = 0;
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
      static_cast<unsigned char>(bytes[2]) == 0xBF)
    start = 3;
  const int n =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + start, static_cast<int>(bytes.size() - start), nullptr, 0);
  if (n <= 0)
    return {};
  std::wstring text(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + start, static_cast<int>(bytes.size() - start), text.data(), n);
  return text;
}
inline void add_xboxgames_simulators(std::vector<std::wstring>& out) {
  wchar_t candidate[] = L"A:\\XboxGames\\Microsoft Flight Simulator 2024\\Content\\FlightSimulator2024.exe";
  const DWORD drives = GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if ((drives & (1u << i)) == 0)
      continue;
    candidate[0] = static_cast<wchar_t>(L'A' + i);
    add_existing_simulator(out, candidate);
  }
}
inline void add_windowsapps_limitless(std::vector<std::wstring>& out) {
  WIN32_FIND_DATAW data{};
  HANDLE find = FindFirstFileW(L"C:\\Program Files\\WindowsApps\\Microsoft.Limitless_*", &data);
  if (find == INVALID_HANDLE_VALUE)
    return;
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || data.cFileName[0] == L'.')
      continue;
    add_existing_simulator(out, std::wstring(L"C:\\Program Files\\WindowsApps\\") + data.cFileName + L"\\FlightSimulator2024.exe");
  } while (FindNextFileW(find, &data));
  FindClose(find);
}
inline std::vector<std::wstring> discover_msfs2024_executables(const std::wstring& configured) {
  std::vector<std::wstring> out;
  add_existing_simulator(out, configured);
  add_xboxgames_simulators(out);
  add_windowsapps_limitless(out);
  const auto steam = steam_install_root();
  add_steam_library_simulators(out, steam);
  for (const auto& library : steam_library_roots_from_vdf(read_small_text_file(steam + L"\\steamapps\\libraryfolders.vdf")))
    add_steam_library_simulators(out, library);
  return out;
}
inline bool accepted_simulator_image(const std::wstring& live, const std::wstring& configured, const std::vector<std::wstring>& known) {
  if (!simulator_exe_name(live))
    return false;
  if (!configured.empty() && same_path(configured, live))
    return true;
  for (const auto& candidate : known)
    if (same_path(candidate, live))
      return true;
  if (known_msfs2024_layout(live))
    return true;
  return configured.empty();
}
struct SimulatorMatch {
  DWORD pid{};
  std::wstring path;
};
inline DWORD select_simulator(const std::vector<SimulatorMatch>& matches, const std::wstring& configured) {
  if (matches.empty())
    return 0;
  std::vector<const SimulatorMatch*> preferred;
  if (!configured.empty()) {
    for (const auto& match : matches)
      if (same_path(configured, match.path))
        preferred.push_back(&match);
    if (preferred.size() > 1)
      return 0;
    if (preferred.size() == 1)
      return preferred[0]->pid;
  }
  if (matches.size() == 1)
    return matches[0].pid;
  bool all_same = true;
  for (size_t i = 1; i < matches.size(); ++i)
    if (!same_path(matches[0].path, matches[i].path)) {
      all_same = false;
      break;
    }
  if (all_same)
    return 0;
  DWORD pid = matches[0].pid;
  for (const auto& match : matches)
    if (match.pid && match.pid < pid)
      pid = match.pid;
  return pid;
}
struct SimulatorAttach {
  DWORD pid{};
  std::wstring path;
  unsigned accepted{};
  bool used_configured{};
};
inline SimulatorAttach find_simulator_attach(const std::wstring& expected) {
  SimulatorAttach result;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return result;
  DWORD session{};
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  const auto known = discover_msfs2024_executables(expected);
  std::vector<SimulatorMatch> matches;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry))
    do {
      DWORD other_session{};
      if (_wcsicmp(entry.szExeFile, L"FlightSimulator2024.exe") || !ProcessIdToSessionId(entry.th32ProcessID, &other_session) ||
          session != other_session)
        continue;
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
      if (!process)
        continue;
      wchar_t path[32768]{};
      DWORD length = 32768;
      const bool ok =
          QueryFullProcessImageNameW(process, 0, path, &length) && same_user(process) && accepted_simulator_image(path, expected, known);
      CloseHandle(process);
      if (ok)
        matches.push_back({entry.th32ProcessID, path});
    } while (Process32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  result.accepted = static_cast<unsigned>(matches.size());
  result.pid = select_simulator(matches, expected);
  for (const auto& match : matches)
    if (match.pid == result.pid) {
      result.path = match.path;
      result.used_configured = !expected.empty() && same_path(expected, match.path);
      break;
    }
  return result;
}
inline DWORD find_simulator(const std::wstring& expected) {
  return find_simulator_attach(expected).pid;
}
inline bool read_exact(HANDLE process, std::uintptr_t address, void* data, SIZE_T count) {
  SIZE_T read{};
  return address && address <= UINTPTR_MAX - count &&
         ReadProcessMemory(process, reinterpret_cast<const void*>(address), data, count, &read) && read == count;
}
inline bool amd64_image(HANDLE process, const Module& module, DWORD* timestamp = nullptr) {
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 nt{};
  if (!read_exact(process, module.base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 1024 * 1024 ||
      !read_exact(process, module.base + dos.e_lfanew, &nt, sizeof(nt)))
    return false;
  if (timestamp)
    *timestamp = nt.FileHeader.TimeDateStamp;
  return nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
         nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && nt.OptionalHeader.SizeOfImage == module.bytes;
}
inline std::uintptr_t remote_export(HANDLE process, DWORD pid, FARPROC function) {
  HMODULE owner{};
  if (!function || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(function), &owner))
    return 0;
  wchar_t path[32768]{};
  if (!GetModuleFileNameW(owner, path, 32768))
    return 0;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(owner);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const unsigned char*>(owner) + dos->e_lfanew);
  const auto rva = reinterpret_cast<std::uintptr_t>(function) - reinterpret_cast<std::uintptr_t>(owner);
  for (const auto& m : modules(pid)) {
    DWORD timestamp{};
    if (same_path(m.path, path) && amd64_image(process, m, &timestamp) && timestamp == nt->FileHeader.TimeDateStamp && rva < m.bytes)
      return m.base + rva;
  }
  return 0;
}
struct LaunchResult {
  bool ok{};
  DWORD error{};
  std::wstring message;
  // Only a preflight read can be retried. Once a remote load/start may have
  // begun, every result remains terminal for this companion session.
  bool retry_before_load = false;
  // MSFS holds administrator rights this companion lacks. No retry can
  // succeed until Taxi Cam itself restarts with the same rights.
  bool elevation_required = false;
};
struct LoaderWaitResult {
  DWORD result = WAIT_TIMEOUT, error = ERROR_SUCCESS;
  bool observed = false, cancelled = false;
};
struct LaunchDiagnostics {
  const wchar_t* phase = L"preflight";
  DWORD module_error = 0, loader_thread_result = 0, loader_result_error = 0;
  DWORD loader_thread_id = 0, start_thread_id = 0, start_thread_result = 0, start_result_error = 0;
  LoaderWaitResult loader_wait{}, start_wait{};
  std::uint64_t started_ms{}, elapsed_ms{}, preflight_ms{}, load_create_ms{}, load_wait_ms{}, post_load_ms{}, export_ms{},
      start_create_ms{}, start_wait_ms{};
  unsigned module_attempts = 0;
  bool load_started = false;
};
class LaunchTiming {
 public:
  explicit LaunchTiming(LaunchDiagnostics& trace) noexcept : trace_(trace), elapsed_(&trace.preflight_ms) {
    phase_started_ = trace_.started_ms = GetTickCount64();
  }
  ~LaunchTiming() {
    const auto now = GetTickCount64();
    *elapsed_ += now - phase_started_;
    trace_.elapsed_ms = now - trace_.started_ms;
  }
  void phase(const wchar_t* name, std::uint64_t& elapsed) noexcept {
    const auto now = GetTickCount64();
    *elapsed_ += now - phase_started_;
    phase_started_ = now;
    elapsed_ = &elapsed;
    trace_.phase = name;
  }

 private:
  LaunchDiagnostics& trace_;
  std::uint64_t* elapsed_;
  std::uint64_t phase_started_{};
};
class LaunchRetry {
 public:
  static constexpr unsigned MaxPreflightRetries = 59;
  static constexpr unsigned MaxRecoveryWaves = 20;
  static constexpr std::uint64_t PreflightDelayMs = 1000;
  static constexpr std::uint64_t RecoveryDelayMs = 15000;

  bool ready(std::uint64_t now) const noexcept { return now >= next_; }
  bool schedule(const LaunchResult& result, std::uint64_t now) noexcept {
    if (result.ok || !result.retry_before_load || retries_ >= MaxPreflightRetries || now > UINT64_MAX - PreflightDelayMs)
      return false;
    ++retries_;
    next_ = now + PreflightDelayMs;
    return true;
  }
  // After a preflight wave is exhausted, wait before starting another wave so a
  // stuck early MSFS session can recover without restarting the companion.
  bool schedule_recovery(std::uint64_t now) noexcept {
    if (recoveries_ >= MaxRecoveryWaves || now > UINT64_MAX - RecoveryDelayMs)
      return false;
    ++recoveries_;
    retries_ = 0;
    next_ = now + RecoveryDelayMs;
    return true;
  }
  void reset() noexcept {
    retries_ = 0;
    recoveries_ = 0;
    next_ = 0;
  }
  unsigned retries() const noexcept { return retries_; }
  unsigned recoveries() const noexcept { return recoveries_; }

 private:
  unsigned retries_{};
  unsigned recoveries_{};
  std::uint64_t next_{};
};
inline LoaderWaitResult wait_for_loader(HANDLE thread, const std::atomic<bool>* running, DWORD timeout_ms = 30000) {
  LoaderWaitResult wait;
  const auto started = GetTickCount64();
  for (;;) {
    if (running && !running->load()) {
      wait.cancelled = true;
      return wait;
    }
    if (GetTickCount64() - started >= timeout_ms)
      return wait;
    wait.result = WaitForSingleObject(thread, 100);
    wait.observed = true;
    if (wait.result == WAIT_FAILED)
      wait.error = GetLastError();
    if (wait.result != WAIT_TIMEOUT)
      return wait;
  }
}
inline LaunchResult loader_wait_failure(const LoaderWaitResult& wait, bool starting_bridge) {
  if (wait.cancelled)
    return {false, ERROR_CANCELLED,
            starting_bridge ? L"Stopped waiting for native bridge startup; startup may still finish."
                            : L"Stopped waiting for bridge loading; the Windows load may still finish."};
  if (wait.result == WAIT_TIMEOUT)
    return {false, WAIT_TIMEOUT,
            starting_bridge ? L"Native bridge startup is pending."
                            : L"Bridge loading is still pending; no second load will be attempted this session."};
  return {false, wait.error ? wait.error : ERROR_GEN_FAILURE,
          starting_bridge ? L"Windows could not wait for native bridge startup. See launcher.log."
                          : L"Windows could not wait for bridge loading. See launcher.log."};
}
inline LaunchResult load_bridge(DWORD pid,
                                const std::wstring& expected_exe,
                                const std::wstring& dll,
                                const std::atomic<bool>* running = nullptr,
                                LaunchDiagnostics* diagnostics = nullptr,
                                bool allow_fresh_load = true) {
  LaunchDiagnostics local_diagnostics;
  auto& trace = diagnostics ? *diagnostics : local_diagnostics;
  trace = {};
  LaunchTiming timing(trace);
  constexpr DWORD rights =
      PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE;
  HANDLE process = OpenProcess(rights, FALSE, pid);
  if (!process) {
    const DWORD error = GetLastError();
    if (elevation_blocks_attach(error, simulator_elevation(pid), own_elevation()))
      return {false, error, ElevatedSimulatorMessage, false, true};
    return {false, error, L"Cannot open MSFS in this Windows session."};
  }
  struct Close {
    HANDLE p;
    ~Close() { CloseHandle(p); }
  } close{process};
  if (!same_user(process))
    return {false, ERROR_ACCESS_DENIED, L"MSFS must run under the same Windows account."};
  wchar_t path[32768]{};
  DWORD n = 32768;
  if (!QueryFullProcessImageNameW(process, 0, path, &n) ||
      !accepted_simulator_image(path, expected_exe, discover_msfs2024_executables(expected_exe)))
    return {false, ERROR_BAD_ENVIRONMENT, L"Simulator executable path changed; attach refused."};
  auto inventory = modules(pid, &trace.module_error, &trace.module_attempts);
  if (trace.module_error)
    return {false, trace.module_error, L"Cannot read the simulator module list during startup.",
            trace.module_error == ERROR_BAD_LENGTH || trace.module_error == ERROR_PARTIAL_COPY};
  Module main{}, bridge{};
  for (const auto& m : inventory) {
    if (!_wcsicmp(m.name.c_str(), L"FlightSimulator2024.exe") && same_path(m.path, path))
      main = m;
    if (!_wcsicmp(m.name.c_str(), L"taxi-camera-native.addon64"))
      return {false, ERROR_ALREADY_EXISTS, L"The legacy taxi add-on is loaded. Run the native installer, then restart MSFS."};
    if (same_path(m.path, dll))
      bridge = m;
  }
  if (!main.base || !amd64_image(process, main))
    return {false, ERROR_BAD_EXE_FORMAT, L"Waiting for readable, valid MSFS executable headers.", true};
  if (!bridge.base) {
    if (!allow_fresh_load)
      return {false, ERROR_MOD_NOT_FOUND, L"Waiting for the previous bridge load to finish; no second Windows load will be started.", true};
    const auto loader = remote_export(process, pid, GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!loader)
      return {false, ERROR_INVALID_ADDRESS, L"Waiting for the verified Windows DLL loader in MSFS.", true};
    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* memory = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory)
      return {false, GetLastError(), L"Could not allocate the bridge path."};
    SIZE_T wrote{};
    if (!WriteProcessMemory(process, memory, dll.c_str(), bytes, &wrote) || wrote != bytes) {
      const DWORD e = GetLastError();
      VirtualFreeEx(process, memory, 0, MEM_RELEASE);
      return {false, e, L"Could not write the bridge path."};
    }
    timing.phase(L"windows_loader_create", trace.load_create_ms);
    HANDLE thread =
        CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(loader), memory, 0, &trace.loader_thread_id);
    if (!thread) {
      const DWORD e = GetLastError();
      VirtualFreeEx(process, memory, 0, MEM_RELEASE);
      return {false, e, L"Windows refused to load the camera bridge."};
    }
    trace.load_started = true;
    timing.phase(L"windows_loader", trace.load_wait_ms);
    trace.loader_wait = wait_for_loader(thread, running);
    if (trace.loader_wait.observed && trace.loader_wait.result == WAIT_OBJECT_0 && !GetExitCodeThread(thread, &trace.loader_thread_result))
      trace.loader_result_error = GetLastError();
    CloseHandle(thread);
    // A pending, cancelled or failed wait cannot prove the loader released its
    // argument. Keep it until process exit, without issuing another load.
    if (trace.loader_wait.cancelled || !trace.loader_wait.observed || trace.loader_wait.result != WAIT_OBJECT_0)
      return loader_wait_failure(trace.loader_wait, false);
    VirtualFreeEx(process, memory, 0, MEM_RELEASE);
    timing.phase(L"after_load_module_scan", trace.post_load_ms);
    inventory = modules(pid, &trace.module_error, &trace.module_attempts);
    if (trace.module_error)
      return {false, trace.module_error, L"Windows finished the load attempt, but the bridge module check failed. See launcher.log."};
    for (const auto& m : inventory)
      if (same_path(m.path, dll))
        bridge = m;
    if (!bridge.base)
      return {false, ERROR_MOD_NOT_FOUND, L"The bridge was not found after Windows loading completed. See launcher.log for details."};
  }
  // LoadLibraryW returns an HMODULE; its thread exit code retains only the low
  // 32 bits. Record it as a diagnostic, never use it as a module address or a
  // remote GetLastError value. Full module identity remains the authority.
  timing.phase(L"bridge_export", trace.export_ms);
  HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (!local)
    return {false, GetLastError(), L"Cannot read the bridge's exported entry point."};
  const auto start = GetProcAddress(local, "TaxiCameraStart");
  const auto offset = start ? reinterpret_cast<std::uintptr_t>(start) - reinterpret_cast<std::uintptr_t>(local) : UINTPTR_MAX;
  FreeLibrary(local);
  if (offset >= bridge.bytes || !amd64_image(process, bridge))
    return {false, ERROR_INVALID_ADDRESS, L"Invalid native bridge entry point."};
  timing.phase(L"bridge_start_create", trace.start_create_ms);
  HANDLE thread = CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(bridge.base + offset), nullptr, 0,
                                     &trace.start_thread_id);
  if (!thread)
    return {false, GetLastError(), L"Cannot start the loaded bridge."};
  timing.phase(L"bridge_start", trace.start_wait_ms);
  trace.start_wait = wait_for_loader(thread, running);
  DWORD result = ERROR_GEN_FAILURE;
  if (trace.start_wait.observed && trace.start_wait.result == WAIT_OBJECT_0) {
    if (!GetExitCodeThread(thread, &result))
      trace.start_result_error = GetLastError();
    trace.start_thread_result = result;
  }
  CloseHandle(thread);
  if (trace.start_wait.cancelled || !trace.start_wait.observed || trace.start_wait.result != WAIT_OBJECT_0)
    return loader_wait_failure(trace.start_wait, true);
  if (trace.start_result_error)
    return {false, trace.start_result_error, L"Cannot read the native bridge startup result. See launcher.log."};
  if (result)
    return {false, result, L"The native bridge refused startup."};
  trace.phase = L"complete";
  return {true, 0, L"Native bridge loaded. Waiting for graphics and aircraft."};
}
}  // namespace taxi_camera::standalone
