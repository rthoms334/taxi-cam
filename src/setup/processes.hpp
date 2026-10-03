#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Read-only process checks for Setup. No process is ever stopped or signalled.
namespace taxi_camera::setup {
struct ProcessInfo {
  DWORD pid{};
  std::wstring name;  // Image name without ".exe".
  std::wstring path;  // Empty when another account owns the process.
};
// Every running process except this one.
std::vector<ProcessInfo> running_processes();
bool simulator_running();
// The companion installed at this destination, under its current or former name.
bool companion_running_from(const std::wstring& destination);
// Any companion, from any installation; settings are shared per Windows user.
bool any_companion_running();
void assert_settings_closed();
// Waits up to 30 seconds for the companion that launched an update to exit.
void wait_for_updating_companion(DWORD pid, const std::wstring& destination);
}  // namespace taxi_camera::setup
