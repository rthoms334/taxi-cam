#pragma once
#include <cstdint>

// Policy for closing the companion after the simulator it served has exited.
// No Windows dependency: the process-table lookup is injected so the rules run
// in ordinary host tests.
//
// Why this exists: MSFS starts the companion from its exe.xml entry, so the
// companion is a child of FlightSimulator2024.exe. Steam keeps a game "running"
// while any descendant of its process remains, so a companion left in the tray
// after Quit to Desktop blocks the next launch.
namespace taxi_camera::standalone {

struct ProcessRecord {
  std::uint32_t pid{};
  std::uint32_t parent_pid{};
  std::uint64_t created{};  // Any monotonic creation stamp; 0 means unknown.
  bool found{};
};

inline constexpr unsigned kMaxAncestorDepth = 8;

// True when ancestor_pid is on self's parent chain. A parent PID can name an
// unrelated process after the real parent exited, so each step must have been
// created no later than its child; an unknown stamp is rejected.
template <class Lookup>
bool descends_from(std::uint32_t self, std::uint32_t ancestor_pid, Lookup&& lookup) {
  if (!self || !ancestor_pid || self == ancestor_pid)
    return false;
  auto child = lookup(self);
  for (unsigned depth = 0; depth < kMaxAncestorDepth && child.found; ++depth) {
    if (!child.parent_pid)
      return false;
    const auto parent = lookup(child.parent_pid);
    if (!parent.found || !parent.created || !child.created || parent.created > child.created)
      return false;
    if (parent.pid == ancestor_pid)
      return true;
    child = parent;
  }
  return false;
}

// Waits a short grace after the simulator disappears, and cancels if a
// simulator is attached again (restart from the menu, or a rescan blip).
class SimulatorExitGate {
 public:
  static constexpr std::uint64_t GraceMs = 3000;
  void simulator_attached() noexcept { closed_at_ = 0; }
  void simulator_closed(std::uint64_t now_ms) noexcept { closed_at_ = now_ms ? now_ms : 1; }
  bool pending() const noexcept { return closed_at_ != 0; }
  // True once, after the grace has elapsed.
  bool due(std::uint64_t now_ms) noexcept {
    if (!closed_at_ || now_ms < closed_at_ || now_ms - closed_at_ < GraceMs)
      return false;
    closed_at_ = 0;
    return true;
  }

 private:
  std::uint64_t closed_at_{};
};

// Only a companion started by the simulator is a child that Steam waits for;
// one started from the Start menu keeps running in the tray as before.
inline constexpr bool exit_after_simulator(bool preference, bool started_by_simulator) noexcept {
  return preference && started_by_simulator;
}

// Unsaved edits in a visible settings window are never discarded silently.
inline constexpr bool may_exit_now(bool window_visible, bool unsaved_edits) noexcept {
  return !(window_visible && unsaved_edits);
}

}  // namespace taxi_camera::standalone
