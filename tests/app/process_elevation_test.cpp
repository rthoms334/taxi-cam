#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "../../src/app/process_elevation.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
#include "../../src/app/launcher.hpp"

namespace {
unsigned checks{};
void require(bool ok, const char* message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}
}  // namespace

int main() {
  using namespace taxi_camera::standalone;
  try {
    // Only an elevated simulator refused to a standard companion names elevation.
    require(elevation_blocks_attach(ERROR_ACCESS_DENIED, Elevation::elevated, Elevation::standard),
            "Elevated MSFS refused to a standard companion is an elevation mismatch");
    require(!elevation_blocks_attach(ERROR_ACCESS_DENIED, Elevation::standard, Elevation::standard),
            "Access denied between two standard processes is not an elevation mismatch");
    require(!elevation_blocks_attach(ERROR_ACCESS_DENIED, Elevation::elevated, Elevation::elevated),
            "Two elevated processes are not an elevation mismatch");
    require(!elevation_blocks_attach(ERROR_ACCESS_DENIED, Elevation::standard, Elevation::elevated),
            "An elevated companion can open a standard simulator");
    require(!elevation_blocks_attach(ERROR_ACCESS_DENIED, Elevation::unknown, Elevation::standard),
            "An unreadable simulator token is not reported as elevation");
    require(!elevation_blocks_attach(ERROR_ACCESS_DENIED, Elevation::elevated, Elevation::unknown),
            "An unreadable own token is not reported as elevation");
    require(!elevation_blocks_attach(ERROR_INVALID_PARAMETER, Elevation::elevated, Elevation::standard),
            "Other OpenProcess errors keep the generic diagnosis");
    require(!elevation_blocks_attach(ERROR_SUCCESS, Elevation::elevated, Elevation::standard), "Success is never a mismatch");

    // The launch result carries the flag so the companion can offer the restart.
    const LaunchResult generic{false, ERROR_ACCESS_DENIED, L"Cannot open MSFS in this Windows session."};
    require(!generic.elevation_required && !generic.retry_before_load, "Default launch result has no elevation flag");
    const LaunchResult elevated{false, ERROR_ACCESS_DENIED, ElevatedSimulatorMessage, false, true};
    require(elevated.elevation_required && !elevated.retry_before_load, "Elevation result is terminal for this session");
    require(std::wstring(ElevatedSimulatorMessage).find(L"administrator") != std::wstring::npos, "Message names administrator rights");

    // Own-token queries succeed for the running test process.
    const auto own = own_elevation();
    require(own == Elevation::standard || own == Elevation::elevated, "Own elevation is readable");
    require(simulator_elevation(GetCurrentProcessId()) == own, "Limited query of own PID agrees with the token query");
    require(simulator_elevation(0) == Elevation::unknown, "PID 0 cannot be queried");
    require(token_elevation(nullptr) == Elevation::unknown, "Null handle is unknown");

    // Relaunch argument round trip.
    require(relaunch_arguments(32864) == L"--wait-for-exit 32864", "Relaunch argument carries the previous PID");
    require(parse_wait_for_exit(L"32864") == 32864, "Parse a PID");
    require(parse_wait_for_exit(L"4294967295") == 4294967295u, "Parse the largest PID");
    require(parse_wait_for_exit(L"4294967296") == 0, "Reject PID overflow");
    require(parse_wait_for_exit(L"") == 0 && parse_wait_for_exit(nullptr) == 0, "Reject empty PID");
    require(parse_wait_for_exit(L"12a") == 0 && parse_wait_for_exit(L"-1") == 0 && parse_wait_for_exit(L" 1") == 0,
            "Reject non-numeric PID");

    // Waiting on a missing or own predecessor never blocks the elevated start.
    require(wait_for_previous_instance(0, 0), "No predecessor");
    require(wait_for_previous_instance(GetCurrentProcessId(), 0), "Own PID is not waited on");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    wchar_t command[] = L"cmd.exe /c exit 0";
    require(CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child),
            "Start a short-lived predecessor");
    CloseHandle(child.hThread);
    require(wait_for_previous_instance(child.dwProcessId, 10000), "A predecessor that exits is waited for");
    CloseHandle(child.hProcess);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "process elevation test failed: %s\n", error.what());
    return 1;
  }
  std::printf("process elevation test passed (%u checks)\n", checks);
  return 0;
}
