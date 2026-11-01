#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include "../shared/profile_selection.hpp"
#include "../shared/ui_text.hpp"

namespace taxi_camera::standalone {
// Companion-only attach preference. Not part of the bridge IPC Settings layout.
inline constexpr UINT kDefaultAutoConnect = 1;

inline bool load_auto_connect(const std::wstring& directory) {
  if (directory.empty())
    return kDefaultAutoConnect != 0;
  const auto path = directory + L"\\settings.ini";
  const UINT value = GetPrivateProfileIntW(L"companion", L"auto_connect", kDefaultAutoConnect, path.c_str());
  return value != 0;
}

inline bool save_auto_connect(const std::wstring& directory, bool enabled) {
  if (directory.empty())
    return false;
  const auto path = directory + L"\\settings.ini";
  return WritePrivateProfileStringW(L"companion", L"auto_connect", enabled ? L"1" : L"0", path.c_str()) != FALSE;
}

enum class ConnectCommand : std::uint32_t { none = 0, connect = 1, reset = 2, disconnect = 3 };

inline const wchar_t* connection_button_label(bool requested) noexcept {
  return requested ? ui_text(L"Disconnect") : ui_text(L"Connect");
}

inline void apply_connection_command(Settings& settings, ConnectCommand command) noexcept {
  if (command == ConnectCommand::connect || command == ConnectCommand::reset)
    settings.enabled = 1;
  else if (command == ConnectCommand::disconnect) {
    settings.enabled = 0;
    settings.manual_mask = settings.calibration_mask = settings.scene_test = 0;
    settings.taxi_selected_mask = settings.taxi_desired_mask = 0;
  }
}

inline bool begin_connection(Settings& settings) noexcept {
  if (settings.profile_request == std::numeric_limits<std::uint64_t>::max())
    return false;
  ++settings.profile_request;
  reset_aircraft_session(settings, settings.aircraft_session_epoch);
  apply_connection_command(settings, ConnectCommand::connect);
  return true;
}

class ConnectCommandQueue {
 public:
  void request(ConnectCommand command) noexcept {
    if (command == ConnectCommand::none)
      return;
    // Reset supersedes a pending Connect; a later Connect does not clear Reset.
    auto previous = pending_.load(std::memory_order_relaxed);
    do {
      if (previous == static_cast<std::uint32_t>(ConnectCommand::reset) && command == ConnectCommand::connect)
        return;
    } while (!pending_.compare_exchange_weak(previous, static_cast<std::uint32_t>(command), std::memory_order_release,
                                             std::memory_order_relaxed));
  }
  ConnectCommand take() noexcept {
    return static_cast<ConnectCommand>(pending_.exchange(static_cast<std::uint32_t>(ConnectCommand::none), std::memory_order_acq_rel));
  }
  bool pending() const noexcept { return pending_.load(std::memory_order_acquire) != 0; }

 private:
  std::atomic<std::uint32_t> pending_{};
};

// Decide whether the worker should attempt bridge load/start this tick.
// manual_session_armed latches an explicit Connect/Reset across scheduled
// preflight retries until success, exhaustion, or a new simulator session.
inline bool should_attempt_connect(bool auto_connect,
                                   bool already_attempted,
                                   ConnectCommand command,
                                   bool manual_session_armed = false,
                                   bool disconnected = false) noexcept {
  if (command == ConnectCommand::disconnect || disconnected)
    return false;
  if (command == ConnectCommand::reset || command == ConnectCommand::connect)
    return true;
  if (already_attempted)
    return false;
  return auto_connect || manual_session_armed;
}

// After stale-heartbeat recovery, ignore the same mapped beat until a newer one arrives.
inline bool heartbeat_confirms_bridge(std::uint64_t sample_heartbeat, std::uint64_t ignore_through) noexcept {
  return sample_heartbeat && sample_heartbeat > ignore_through;
}

inline bool fresh_load_allowed(bool load_started_this_session) noexcept {
  return !load_started_this_session;
}

// Issue 137: a companion the simulator started from exe.xml (--background)
// leaves with it, like the other add-ons it launches; Steam counts that child
// process as the game still running. A manual start stays in the tray. The
// grace lets a simulator that comes straight back keep this companion.
class ExitWithSimulator {
 public:
  static constexpr std::uint64_t GraceMs = 5000;
  explicit ExitWithSimulator(bool enabled) noexcept : enabled_(enabled) {}
  // Once per worker loop. True while a simulator seen in this run has been
  // gone for GraceMs, so a quit the window defers (an open prompt) comes again.
  bool observe(bool simulator_running, std::uint64_t now_ms) noexcept {
    if (!enabled_)
      return false;
    if (simulator_running) {
      seen_ = true;
      gone_since_ = 0;
      return false;
    }
    if (!seen_)
      return false;
    if (!gone_since_) {
      gone_since_ = now_ms ? now_ms : 1;
      return false;
    }
    return now_ms >= gone_since_ && now_ms - gone_since_ >= GraceMs;
  }

 private:
  bool enabled_ = false, seen_ = false;
  std::uint64_t gone_since_ = 0;
};
}  // namespace taxi_camera::standalone
