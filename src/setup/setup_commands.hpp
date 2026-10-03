#pragma once

// Installer commands run by Setup and the uninstaller as
// "taxi-cam.exe --setup <command> ...". Results and errors are written to the
// --state directory: error.txt on failure, warning.txt and choices.ini as
// described per command. Returns the process exit code.
namespace taxi_camera::setup {
int run_setup_command(int argc, wchar_t** argv);
}  // namespace taxi_camera::setup
