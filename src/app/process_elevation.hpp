#pragma once
#include <windows.h>
#include <shellapi.h>
#include <cstdint>
#include <cwchar>
#include <string>

namespace taxi_camera::standalone {
// Whether a process token carries the full administrator rights granted by
// UAC. "unknown" covers a token Windows would not let this caller query.
enum class Elevation : std::uint8_t { unknown, standard, elevated };

inline Elevation token_elevation(HANDLE process) noexcept {
  HANDLE token{};
  if (!process || !OpenProcessToken(process, TOKEN_QUERY, &token))
    return Elevation::unknown;
  TOKEN_ELEVATION elevation{};
  DWORD returned{};
  const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) && returned == sizeof(elevation);
  CloseHandle(token);
  if (!ok)
    return Elevation::unknown;
  return elevation.TokenIsElevated ? Elevation::elevated : Elevation::standard;
}

inline Elevation own_elevation() noexcept {
  return token_elevation(GetCurrentProcess());
}

// PROCESS_QUERY_LIMITED_INFORMATION and TOKEN_QUERY remain available on an
// elevated process of the same user, so the mismatch can be diagnosed from a
// standard-rights companion after the wider OpenProcess was refused.
inline Elevation simulator_elevation(DWORD pid) noexcept {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process)
    return Elevation::unknown;
  const auto elevation = token_elevation(process);
  CloseHandle(process);
  return elevation;
}

// Windows refuses VM_WRITE/CREATE_THREAD on an elevated process from a
// standard-rights caller with ERROR_ACCESS_DENIED. Every other combination,
// including an elevated companion attaching to a standard simulator, is
// permitted for the same account, so only this pairing names elevation.
inline constexpr bool elevation_blocks_attach(DWORD open_error, Elevation simulator, Elevation companion) noexcept {
  return open_error == ERROR_ACCESS_DENIED && simulator == Elevation::elevated && companion == Elevation::standard;
}

inline constexpr const wchar_t* ElevatedSimulatorMessage =
    L"MSFS is running as administrator. Taxi Cam needs the same rights; choose Restart as administrator.";

// The relaunched companion waits for this process to release the single-
// instance mutex before it starts, so the argument carries the old PID.
inline constexpr wchar_t WaitForExitArgument[] = L"--wait-for-exit";

inline std::wstring relaunch_arguments(DWORD previous_pid) {
  return std::wstring(WaitForExitArgument) + L" " + std::to_wstring(previous_pid);
}

// Parses the PID that follows --wait-for-exit. Zero means absent or invalid.
inline DWORD parse_wait_for_exit(const wchar_t* value) noexcept {
  if (!value || !*value)
    return 0;
  std::uint64_t pid{};
  for (const wchar_t* c = value; *c; ++c) {
    if (*c < L'0' || *c > L'9')
      return 0;
    pid = pid * 10 + static_cast<std::uint64_t>(*c - L'0');
    if (pid > 0xFFFFFFFFull)
      return 0;
  }
  return static_cast<DWORD>(pid);
}

// Waits for a previous companion to exit. A PID that cannot be opened is
// treated as already gone; a still-running one is only waited on briefly so
// a stuck predecessor cannot block the elevated start forever.
inline bool wait_for_previous_instance(DWORD pid, DWORD timeout_ms) noexcept {
  if (!pid || pid == GetCurrentProcessId())
    return true;
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!process)
    return true;
  const DWORD wait = WaitForSingleObject(process, timeout_ms);
  CloseHandle(process);
  return wait == WAIT_OBJECT_0;
}

// Starts this executable again through the UAC consent prompt. Returns false
// with the Windows error when consent was refused or the start failed; the
// caller keeps running in that case.
inline bool relaunch_elevated(const std::wstring& executable, const std::wstring& arguments, DWORD& error) noexcept {
  error = ERROR_SUCCESS;
  SHELLEXECUTEINFOW execute{};
  execute.cbSize = sizeof(execute);
  execute.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  execute.lpVerb = L"runas";
  execute.lpFile = executable.c_str();
  execute.lpParameters = arguments.c_str();
  execute.nShow = SW_SHOWNORMAL;
  if (ShellExecuteExW(&execute))
    return true;
  error = GetLastError();
  if (!error)
    error = ERROR_GEN_FAILURE;
  return false;
}
}  // namespace taxi_camera::standalone
