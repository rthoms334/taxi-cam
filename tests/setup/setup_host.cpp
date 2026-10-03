#include <cwchar>
#include "../../src/setup/setup_commands.hpp"

// Test-only host for the companion's setup commands. It is built with
// TAXI_SETUP_TEST, so TAXI_SETUP_TEST_PROCESSES replaces process enumeration
// and isolated fixtures never depend on the running simulator or companion.
// Installed as taxi-cam.exe in isolated installer fixtures, it accepts the
// same "--setup <command>" form that Setup and the uninstaller use.
int wmain(int argc, wchar_t** argv) {
  const int skip = argc >= 2 && !std::wcscmp(argv[1], L"--setup") ? 2 : 1;
  return taxi_camera::setup::run_setup_command(argc - skip, argv + skip);
}
