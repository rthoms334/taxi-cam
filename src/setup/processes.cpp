#include "processes.hpp"
#include <tlhelp32.h>
#include "setup_common.hpp"

namespace taxi_camera::setup {
namespace {
std::wstring image_path(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process)
    return {};
  std::wstring path(32768, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process, 0, path.data(), &size))
    size = 0;
  CloseHandle(process);
  path.resize(size);
  return path;
}
bool companion_name(const std::wstring& name) {
  return equal_insensitive(name, L"taxi-cam") || equal_insensitive(name, L"380-taxi-cam");
}
bool companion_path(const std::wstring& path, const std::wstring& destination) {
  return !path.empty() &&
         (equal_insensitive(path, join(destination, L"taxi-cam.exe")) || equal_insensitive(path, join(destination, L"380-taxi-cam.exe")));
}
}  // namespace
std::vector<ProcessInfo> running_processes() {
  std::vector<ProcessInfo> processes;
#ifdef TAXI_SETUP_TEST
  // Isolated tests describe running processes as name|path entries separated by
  // '*' ("-" for none), so guards run without enumerating real processes.
  if (const auto fixture = environment(L"TAXI_SETUP_TEST_PROCESSES"); !fixture.empty()) {
    std::size_t at = 0;
    for (;;) {
      const auto end = fixture.find(L'*', at);
      const auto item = fixture.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
      if (const auto split = item.find(L'|'); split != std::wstring::npos)
        processes.push_back({1, item.substr(0, split), item.substr(split + 1)});
      if (end == std::wstring::npos)
        break;
      at = end + 1;
    }
    return processes;
  }
#endif
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    fail_win32(L"Cannot list running programs to check that MSFS and Taxi Cam are closed.");
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
    // Setup commands run from taxi-cam.exe itself; never count this process.
    if (entry.th32ProcessID == GetCurrentProcessId())
      continue;
    std::wstring name = entry.szExeFile;
    if (name.size() > 4 && equal_insensitive(std::wstring_view(name).substr(name.size() - 4), L".exe"))
      name.resize(name.size() - 4);
    processes.push_back({entry.th32ProcessID, std::move(name), {}});
  }
  CloseHandle(snapshot);
  for (auto& process : processes)
    if (companion_name(process.name))
      process.path = image_path(process.pid);
  return processes;
}
bool simulator_running() {
  for (const auto& process : running_processes())
    if (equal_insensitive(process.name, L"FlightSimulator2024"))
      return true;
  return false;
}
bool companion_running_from(const std::wstring& destination) {
  for (const auto& process : running_processes())
    if (companion_name(process.name) && companion_path(process.path, destination))
      return true;
  return false;
}
bool any_companion_running() {
  for (const auto& process : running_processes())
    if (companion_name(process.name))
      return true;
  return false;
}
void assert_settings_closed() {
  if (any_companion_running())
    fail(L"Exit every Taxi Cam companion before resetting or removing shared settings.");
}
void wait_for_updating_companion(DWORD pid, const std::wstring& destination) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
  if (!process)
    return;  // Already exited.
  std::wstring path(32768, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  const bool named = QueryFullProcessImageNameW(process, 0, path.data(), &size);
  path.resize(named ? size : 0);
  DWORD exit_code = STILL_ACTIVE;
  const bool running = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
  if (running && !companion_path(path, destination)) {
    CloseHandle(process);
    fail(L"UPDATEFROMPID does not identify the installed companion.");
  }
  const DWORD wait = running ? WaitForSingleObject(process, 30000) : WAIT_OBJECT_0;
  CloseHandle(process);
  if (wait != WAIT_OBJECT_0)
    fail(L"The companion is still exiting. Close it and retry Setup.");
}
}  // namespace taxi_camera::setup
