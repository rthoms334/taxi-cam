#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "module_inventory.hpp"

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
inline DWORD find_simulator(const std::wstring& expected) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return 0;
  DWORD found = 0, session{};
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
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
      wchar_t path[32768];
      DWORD length = 32768;
      const bool match =
          QueryFullProcessImageNameW(process, 0, path, &length) && same_user(process) && (expected.empty() || same_path(expected, path));
      CloseHandle(process);
      if (match) {
        if (found) {
          found = 0;
          break;
        }
        found = entry.th32ProcessID;
      }
    } while (Process32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return found;
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
};
struct LaunchDiagnostics {
  const wchar_t* phase = L"preflight";
  DWORD module_error = 0, loader_thread_result = 0, loader_result_error = 0;
  unsigned module_attempts = 0;
  bool load_started = false;
};
class LaunchRetry {
 public:
  bool ready(std::uint64_t now) const noexcept { return now >= next_; }
  bool schedule(const LaunchResult& result, std::uint64_t now) noexcept {
    if (result.ok || !result.retry_before_load || retries_ >= 59 || now > UINT64_MAX - 1000)
      return false;
    ++retries_;
    next_ = now + 1000;
    return true;
  }

 private:
  unsigned retries_{};
  std::uint64_t next_{};
};
inline DWORD wait_for_loader(HANDLE thread, const std::atomic<bool>* running) {
  const auto deadline = GetTickCount64() + 30000;
  while ((!running || running->load()) && GetTickCount64() < deadline) {
    const auto result = WaitForSingleObject(thread, 100);
    if (result != WAIT_TIMEOUT)
      return result;
  }
  return WAIT_TIMEOUT;
}
inline LaunchResult load_bridge(DWORD pid,
                                const std::wstring& expected_exe,
                                const std::wstring& dll,
                                const std::atomic<bool>* running = nullptr,
                                LaunchDiagnostics* diagnostics = nullptr) {
  LaunchDiagnostics local_diagnostics;
  auto& trace = diagnostics ? *diagnostics : local_diagnostics;
  trace = {};
  constexpr DWORD rights =
      PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE;
  HANDLE process = OpenProcess(rights, FALSE, pid);
  if (!process)
    return {false, GetLastError(), L"Cannot open MSFS in this Windows session."};
  struct Close {
    HANDLE p;
    ~Close() { CloseHandle(p); }
  } close{process};
  if (!same_user(process))
    return {false, ERROR_ACCESS_DENIED, L"MSFS must run under the same Windows account."};
  wchar_t path[32768]{};
  DWORD n = 32768;
  if (!QueryFullProcessImageNameW(process, 0, path, &n) || (!expected_exe.empty() && !same_path(expected_exe, path)))
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
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(loader), memory, 0, nullptr);
    if (!thread) {
      const DWORD e = GetLastError();
      VirtualFreeEx(process, memory, 0, MEM_RELEASE);
      return {false, e, L"Windows refused to load the camera bridge."};
    }
    trace.load_started = true;
    trace.phase = L"windows_loader";
    const DWORD wait = wait_for_loader(thread, running);
    if (wait == WAIT_OBJECT_0 && !GetExitCodeThread(thread, &trace.loader_thread_result))
      trace.loader_result_error = GetLastError();
    CloseHandle(thread);
    // The loader may still own this argument on timeout. Keep it until process exit.
    if (wait != WAIT_OBJECT_0)
      return {false, WAIT_TIMEOUT, L"Bridge loading is still pending; no second load will be attempted this session."};
    VirtualFreeEx(process, memory, 0, MEM_RELEASE);
    trace.phase = L"after_load_module_scan";
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
  trace.phase = L"bridge_export";
  HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (!local)
    return {false, GetLastError(), L"Cannot read the bridge's exported entry point."};
  const auto start = GetProcAddress(local, "TaxiCameraStart");
  const auto offset = start ? reinterpret_cast<std::uintptr_t>(start) - reinterpret_cast<std::uintptr_t>(local) : UINTPTR_MAX;
  FreeLibrary(local);
  if (offset >= bridge.bytes || !amd64_image(process, bridge))
    return {false, ERROR_INVALID_ADDRESS, L"Invalid native bridge entry point."};
  HANDLE thread =
      CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(bridge.base + offset), nullptr, 0, nullptr);
  if (!thread)
    return {false, GetLastError(), L"Cannot start the loaded bridge."};
  trace.phase = L"bridge_start";
  const DWORD wait = wait_for_loader(thread, running);
  DWORD result = ERROR_GEN_FAILURE;
  if (wait == WAIT_OBJECT_0)
    GetExitCodeThread(thread, &result);
  CloseHandle(thread);
  if (wait != WAIT_OBJECT_0)
    return {false, WAIT_TIMEOUT, L"Native bridge startup is pending."};
  if (result)
    return {false, result, L"The native bridge refused startup."};
  trace.phase = L"complete";
  return {true, 0, L"Native bridge loaded. Waiting for graphics and aircraft."};
}
}  // namespace taxi_camera::standalone
