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
// What a door camera's display shows: PLEASE WAIT from the moment it turns on
// until its waiting time is over (waiting: the same minimum as a taxi camera,
// WaitingPageTimer) and it has a current door page; then the door page. With
// no PLEASE WAIT page ready, a current door page shows at once, else nothing.
enum class DoorSidePage { none, waiting, door };
inline DoorSidePage door_side_page(bool available, bool page_current, bool waiting, bool waiting_ready) noexcept {
  if (!available)
    return DoorSidePage::none;
  if (page_current && !waiting)
    return DoorSidePage::door;
  if (waiting_ready)
    return DoorSidePage::waiting;
  return page_current ? DoorSidePage::door : DoorSidePage::none;
}
inline DoorState door_state(bool on, bool live, bool page) noexcept {
  return !on ? DoorState::off : !page ? DoorState::waiting : live ? DoorState::live : DoorState::snapshot;
}
}  // namespace taxi_camera::standalone
