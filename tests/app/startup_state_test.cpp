#include "../../src/app/startup_state.hpp"
#include <cstdio>
#include <stdexcept>

namespace {
unsigned checks{};
void require(bool ok, const char* message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}
bool exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
}  // namespace

int main() {
  using taxi_camera::standalone::StartupSettings;
  try {
    wchar_t cwd[32768]{};
    require(GetCurrentDirectoryW(32768, cwd), "Read isolated artifact root");
    const auto directory = std::wstring(cwd) + L"\\build\\startup-state-test-" + std::to_wstring(GetCurrentProcessId());
    require(CreateDirectoryW(directory.c_str(), nullptr), "Create isolated first-use fixture");
    const auto marker = directory + L"\\startup-state";

    const StartupSettings preview(directory, false, true);
    require(preview.should_show(), "Explicit preview still shows Settings");
    require(preview.record_shown(true, true) && !exists(marker), "Preview never records first-use state");
    require(!StartupSettings(directory, true, true).should_show(), "Background preview ignores missing real first-use state");

    const StartupSettings first(directory, true, false);
    require(first.should_show(), "First background launch shows Settings");
    require(!first.record_shown(false, true) && !exists(marker), "Failed or hidden window cannot consume first launch");
    require(!first.record_shown(true, false) && !exists(marker), "Failed service startup cannot consume first launch");
    require(StartupSettings(directory, true, false).should_show(), "Failed startup retries first-use visibility");
    require(first.record_shown(true, true) && exists(marker), "Visible successful startup records first use");
    require(!StartupSettings(directory, true, false).should_show(), "Later background launch remains in tray");
    require(StartupSettings(directory, false, false).should_show(), "Explicit manual launch always opens Settings");
    require(StartupSettings(directory, false, true).record_shown(true, true) && !StartupSettings(directory, true, false).should_show(),
            "Preview leaves existing first-use state intact");
    const auto held = CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(held != INVALID_HANDLE_VALUE, "Hold existing marker against writes");
    require(StartupSettings(directory, false, false).record_shown(true, true), "Later manual launch does not rewrite first-use state");
    CloseHandle(held);
    require(DeleteFileW(marker.c_str()), "Reset only the isolated first-use marker");

    require(CreateDirectoryW(marker.c_str(), nullptr), "Block atomic marker replacement with an isolated directory");
    const StartupSettings unwritable(directory, true, false);
    require(unwritable.should_show() && !unwritable.record_shown(true, true), "State write failure is reported without hiding Settings");
    require(StartupSettings(directory, true, false).should_show(), "Failed state write retries on the next launch");
    require(!exists(marker + L".tmp." + std::to_wstring(GetCurrentProcessId())), "Failed marker write leaves no temporary file");
    require(RemoveDirectoryW(marker.c_str()), "Remove isolated write obstacle");
    const StartupSettings manual_first(directory, false, false);
    require(manual_first.should_show() && manual_first.record_shown(true, true), "First manual launch consumes first use after showing");
    require(!StartupSettings(directory, true, false).should_show(), "A later background launch after manual first use stays hidden");
    require(StartupSettings(L"", true, false).should_show() && !StartupSettings(L"", true, false).record_shown(true, true),
            "Missing per-user directory fails open without writing a relative marker");
    require(DeleteFileW(marker.c_str()) && RemoveDirectoryW(directory.c_str()), "Clean up only the isolated fixture");
    std::printf("PASS first-launch Settings: %u visibility, persistence, failure and preview-isolation checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL first-launch Settings: %s\n", error.what());
    return 1;
  }
}
