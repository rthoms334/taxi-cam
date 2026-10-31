#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "../../src/app/connection_recoverability.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

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
    wchar_t cwd[32768]{};
    require(GetCurrentDirectoryW(32768, cwd), "Read isolated fixture root");
    const auto directory = std::wstring(cwd) + L"\\build\\connection-recoverability-" + std::to_wstring(GetCurrentProcessId());
    require(CreateDirectoryW(directory.c_str(), nullptr), "Create isolated preference directory");

    require(load_auto_connect(directory), "Missing preference defaults to auto-connect on");
    require(save_auto_connect(directory, false) && !load_auto_connect(directory), "Persist auto-connect off");
    require(save_auto_connect(directory, true) && load_auto_connect(directory), "Persist auto-connect on");
    require(load_auto_connect(L""), "Empty directory fails open to the default");
    require(!save_auto_connect(L"", false), "Empty directory cannot write preference");

    require(load_exit_with_simulator(directory), "Missing preference defaults to exit-with-simulator on");
    require(save_exit_with_simulator(directory, false) && !load_exit_with_simulator(directory), "Persist exit-with-simulator off");
    require(load_auto_connect(directory), "Exit preference left Auto-connect untouched");
    require(save_exit_with_simulator(directory, true) && load_exit_with_simulator(directory), "Persist exit-with-simulator on");
    require(!save_exit_with_simulator(L"", false) && load_exit_with_simulator(L""), "Empty directory uses the default");

    require(should_attempt_connect(true, false, ConnectCommand::none), "Auto-connect attempts on first sighting");
    require(!should_attempt_connect(true, true, ConnectCommand::none), "Auto-connect does not hammer after an attempt");
    require(!should_attempt_connect(false, false, ConnectCommand::none), "Manual mode waits for Connect");
    require(should_attempt_connect(false, false, ConnectCommand::connect), "Connect command authorizes a manual attempt");
    require(should_attempt_connect(false, true, ConnectCommand::reset), "Reset authorizes another attempt");
    require(should_attempt_connect(true, true, ConnectCommand::connect), "Connect can retry after a stuck attempt");
    require(should_attempt_connect(false, false, ConnectCommand::none, true),
            "Latched manual authorization continues after Connect is consumed");
    require(!should_attempt_connect(false, true, ConnectCommand::none, true), "Latched manual mode still respects an in-flight attempt");
    require(!should_attempt_connect(true, false, ConnectCommand::disconnect, true), "Disconnect cancels an armed automatic attempt");
    require(!should_attempt_connect(true, false, ConnectCommand::none, true, true),
            "Explicit disconnect suppresses automatic and latched retries");
    require(should_attempt_connect(true, false, ConnectCommand::connect, false, false), "Explicit reconnect restores attempts");
    Settings settings;
    settings.enabled = 0;  // A profile saved by the former Service: Off switch.
    const auto saved_mounts = settings.mounts;
    apply_connection_command(settings, ConnectCommand::connect);
    require(settings.enabled && std::wstring(connection_button_label(true)) == L"Disconnect", "Connect enables an old disabled profile");
    settings.manual_mask = settings.calibration_mask = 3;
    settings.scene_test = 1;
    settings.taxi_request = 12;
    settings.taxi_selected_mask = 3;
    settings.taxi_desired_mask = 2;
    apply_connection_command(settings, ConnectCommand::disconnect);
    require(!settings.enabled && !settings.manual_mask && !settings.calibration_mask && !settings.scene_test &&
                !settings.taxi_selected_mask && !settings.taxi_desired_mask && settings.taxi_request == 12 &&
                settings.mounts == saved_mounts && valid_settings(settings),
            "Disconnect stops output and temporary requests while retaining calibration and request identity");
    require(std::wstring(connection_button_label(false)) == L"Connect", "Disconnected control invites Connect");
    apply_connection_command(settings, ConnectCommand::connect);
    require(settings.enabled && !settings.manual_mask && !settings.calibration_mask && !settings.scene_test,
            "Reconnect enables again without reviving discarded temporary requests");
    settings.profile_request = 23;
    settings.route_request = 19;
    settings.left_id = 12;
    settings.right_id = 13;
    const auto selected_profile = settings.profile, auto_profile = settings.auto_profile;
    require(begin_connection(settings) && settings.profile_request == 24 && !settings.route_request && !settings.left_id &&
                !settings.right_id && settings.mounts == saved_mounts && settings.profile == selected_profile &&
                settings.auto_profile == auto_profile,
            "A new Connect requests bridge reset and rescan while preserving aircraft and calibration preferences");
    apply_connection_command(settings, ConnectCommand::disconnect);
    require(settings.profile_request == 24 && begin_connection(settings) && settings.profile_request == 25,
            "Disconnect preserves the serial and every subsequent Connect advances it");
    settings.profile_request = std::numeric_limits<std::uint64_t>::max();
    require(!begin_connection(settings) && settings.profile_request == std::numeric_limits<std::uint64_t>::max(),
            "Connection session serial never wraps");
    require(fresh_load_allowed(false) && !fresh_load_allowed(true), "Fresh LoadLibrary only before a session load starts");
    require(!heartbeat_confirms_bridge(1000, 1000) && !heartbeat_confirms_bridge(999, 1000),
            "Stale or equal heartbeat cannot re-arm recovery");
    require(heartbeat_confirms_bridge(1001, 1000), "Newer heartbeat confirms bridge health after recovery");
    require(!heartbeat_confirms_bridge(0, 0), "Missing heartbeat does not confirm the bridge");

    ConnectCommandQueue queue;
    require(!queue.pending(), "Queue starts empty");
    queue.request(ConnectCommand::connect);
    require(queue.pending() && queue.take() == ConnectCommand::connect && !queue.pending(), "Connect request is consumed once");
    queue.request(ConnectCommand::connect);
    queue.request(ConnectCommand::reset);
    require(queue.take() == ConnectCommand::reset, "Reset supersedes a pending Connect");
    queue.request(ConnectCommand::reset);
    queue.request(ConnectCommand::connect);
    require(queue.take() == ConnectCommand::reset, "Connect cannot clear a pending Reset");
    queue.request(ConnectCommand::reset);
    queue.request(ConnectCommand::disconnect);
    require(queue.take() == ConnectCommand::disconnect, "Disconnect cancels a pending reset");
    queue.request(ConnectCommand::connect);
    queue.request(ConnectCommand::disconnect);
    require(queue.take() == ConnectCommand::disconnect, "Disconnect cancels a pending connect");
    queue.request(ConnectCommand::disconnect);
    queue.request(ConnectCommand::connect);
    require(queue.take() == ConnectCommand::connect, "A later explicit Connect supersedes Disconnect");

    require(DeleteFileW((directory + L"\\settings.ini").c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND, "Remove preference file");
    require(RemoveDirectoryW(directory.c_str()), "Remove isolated fixture");
    std::printf("PASS connection recoverability: %u preference, gate and command checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL connection recoverability: %s\n", error.what());
    return 1;
  }
}
