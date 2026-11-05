#pragma once
#include <cstdint>
#include "protocol.hpp"

namespace taxi_camera::standalone {
// When the door camera renders. Live keeps it rendering while it is on. A
// snapshot renders from each turn-on (or each switch from live) until
// SnapshotImages door pages have been composed from it, then stops; the last
// page stays on the display until the door camera is turned off. Taking a few
// images lets the view's temporal anti-aliasing and the exposure settle. A
// taken snapshot whose page is gone (a feed or session reset drops it) is
// taken again.
class DoorSnapshot {
 public:
  static constexpr std::uint64_t SnapshotImages = 3;
  // on: turned on and admitted this control tick; pages: door pages composed
  // so far (scene_runtime::Snapshot::door_pages); ready: a door page is there
  // to show (scene_runtime::DoorPage::ready). True while it renders.
  bool observe(bool on, bool live, std::uint64_t pages, bool ready = true) noexcept {
    if (!on) {
      on_ = false;
      return false;
    }
    if (!on_ || live || pages < base_ || (!ready && pages - base_ >= SnapshotImages)) {
      on_ = true;
      base_ = pages;
    }
    return live || pages - base_ < SnapshotImages;
  }

 private:
  bool on_ = false;
  std::uint64_t base_ = 0;
};
// The door camera's display leaves its PLEASE WAIT page as soon as a door
// page composed since this turn-on is ready (pages past the count at turn-on),
// not after the taxi cameras' WaitingPageMinimumMs: its first picture takes
// about 0.1-0.2 s, and the minimum held it back to 0.75 s (A350, 2026-11-05).
// An older page is never shown in its place. Returns the door side bits whose
// waiting page may end.
class DoorWaitRelease {
 public:
  unsigned observe(unsigned door_mask, std::uint64_t pages, bool ready) noexcept {
    // A restarted page count (a feed or session reset) counts from there.
    if ((door_mask && !on_) || pages < base_)
      base_ = pages;
    on_ = door_mask != 0;
    return door_mask && ready && pages > base_ ? door_mask : 0u;
  }

 private:
  bool on_ = false;
  std::uint64_t base_ = 0;
};
inline DoorState door_state(bool on, bool live, bool page) noexcept {
  return !on ? DoorState::off : !page ? DoorState::waiting : live ? DoorState::live : DoorState::snapshot;
}
}  // namespace taxi_camera::standalone
