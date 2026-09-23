#pragma once

#include "entry_pair.hpp"
#include "owned_view.hpp"

#include <cmath>

namespace taxi_camera::native_camera {

// Serialized by the caller in the manager observer phase. Retains only opaque
// owner/entry identities and update counters, never borrowed view pointers.
// Closed gates across CPU updates do NOT prove GPU or native output lifetime;
// the caller must establish that separately before acting on resize.
//
// Waiting for an output to return to its pane has no deadline: a user may stay
// in the graphics settings for any length of time, and expiry would leave the
// cameras off until a simulator restart. The caller keeps that wait cheap
// (issue 69: Frame Generation switched on in flight retried for 8 minutes).
class ViewResizeRecovery {
 public:
  // An ordinary AA/upscaler switch restores well within this many updates
  // (about five seconds at typical manager rates). A longer wait is reported
  // to the user as expected and self-resolving, not as a failure.
  static constexpr std::uint64_t SettlingNoticeUpdates = 250;
  enum class Action { wait, close_gates, resize, blocked };
  static const char* action_name(Action action) noexcept {
    switch (action) {
      case Action::wait:
        return "wait";
      case Action::close_gates:
        return "close_gates";
      case Action::resize:
        return "resize";
      case Action::blocked:
        return "blocked";
    }
    return "unknown";
  }
  using Ids = std::array<engine_camera::EntryId, 3>;
  using Views = std::array<engine_camera::OwnedViewSnapshot, 3>;

  bool begin(engine_camera::ManagerToken owner, const Ids& ids) noexcept {
    if (failed_)
      return false;
    if (!valid_identity(owner, ids) || (pending_ && !matches(owner, ids))) {
      mark_failed();
      return false;
    }
    if (pending_)
      return true;
    owner_ = owner;
    ids_ = ids;
    pending_ = true;
    return true;
  }

  Action observe(engine_camera::ManagerToken owner, const Ids& ids, std::uint64_t update, const Views& views) noexcept {
    if (failed_)
      return Action::blocked;
    if (!pending_)
      return Action::wait;
    if (!valid_identity(owner, ids) || !matches(owner, ids) || !update || update < last_update_) {
      mark_failed();
      return Action::blocked;
    }
    last_update_ = update;
    if (!first_update_)
      first_update_ = update;
    // Only the feeds this pair owns. A two-feed aircraft (A380) leaves the
    // third snapshot default, never inspected: judged here it read as a
    // temporarily unavailable view forever, and a graphics change mid-flight
    // (Frame Generation toggle, issue 69) never got past this wait.
    const unsigned feeds = ids[2] ? 3u : 2u;
    bool temporary = false;
    for (unsigned index = 0; index < feeds; ++index) {
      const auto& view = views[index];
      using Status = engine_camera::OwnedViewStatus;
      if ((view.status == Status::pending && view.complete && !view.ready) ||
          (!view.complete && !view.ready &&
           (view.status == Status::not_inspected || view.status == Status::read_failed || view.status == Status::changed ||
            view.status == Status::pool_changed))) {
        temporary = true;
      } else if (!ready_chain(view)) {
        mark_failed();
        return Action::blocked;
      }
    }
    if (temporary) {
      closed_seen_ = false;
      return Action::wait;
    }
    for (unsigned i = 0; i < feeds; ++i) {
      for (unsigned j = i + 1; j < feeds; ++j) {
        if (views[i].view_address == views[j].view_address || views[i].view_index == views[j].view_index) {
          mark_failed();
          return Action::blocked;
        }
      }
    }
    // A native attempt consumes this authorization. The caller must finish on
    // complete success or mark_failed on ANY refused/partial mutation.
    if (resize_issued_)
      return Action::wait;
    bool all_closed = true;
    for (unsigned i = 0; i < feeds; ++i)
      all_closed = all_closed && (views[i].flags[0] & 1u);
    if (!all_closed) {
      closed_seen_ = false;
      return Action::close_gates;
    }
    if (!closed_seen_) {
      closed_seen_ = true;
      closed_update_ = update;
      return Action::wait;
    }
    if (update == closed_update_)
      return Action::wait;
    resize_issued_ = true;
    return Action::resize;
  }

  bool finish(engine_camera::ManagerToken owner, const Ids& ids) noexcept {
    if (!pending_ || failed_)
      return false;
    if (!valid_identity(owner, ids) || !matches(owner, ids)) {
      mark_failed();
      return false;
    }
    if (!resize_issued_)
      return false;
    clear();
    return true;
  }
  // Caller inspected a closed ready pair but could not prove the existing Bitmap
  // still matches the pane. Release the one-shot authorization so a later update
  // may restore instead of sitting in wait after an unused resize token.
  void release_resize_authorization() noexcept {
    if (pending_ && !failed_) {
      resize_issued_ = false;
      ++released_;
    }
  }
  bool pending() const noexcept { return pending_; }
  bool failed() const noexcept { return failed_; }
  // Diagnostics only: updates observed since this recovery began, and resize
  // authorizations released because the outputs did not match their panes.
  std::uint64_t elapsed_updates() const noexcept { return first_update_ ? last_update_ - first_update_ : 0; }
  unsigned released() const noexcept { return released_; }
  bool settling() const noexcept { return pending_ && !failed_ && elapsed_updates() >= SettlingNoticeUpdates; }
  void clear() noexcept { *this = {}; }  // Explicit lifecycle reset, never automatic retry.
  void mark_failed() noexcept {
    failed_ = true;
    closed_seen_ = resize_issued_ = false;
  }

 private:
  static bool valid_identity(engine_camera::ManagerToken owner, const Ids& ids) noexcept {
    if (!owner.valid() || !ids[0] || !ids[1] || ids[0] == ids[1])
      return false;
    if (ids[2] && (ids[2] == ids[0] || ids[2] == ids[1]))
      return false;
    return true;
  }
  bool matches(engine_camera::ManagerToken owner, const Ids& ids) const noexcept { return owner == owner_ && ids == ids_; }
  static bool ready_chain(const engine_camera::OwnedViewSnapshot& view) noexcept {
    return view.complete && view.ready && view.status == engine_camera::OwnedViewStatus::ready && !view.read_failures &&
           (!view.error || !*view.error) && view.view_address && !(view.view_address & 7u) && view.node_address &&
           !(view.node_address & 7u) && view.camera_address && !(view.camera_address & 7u) && view.view_index >= 0 && view.view_index < 8 &&
           std::isfinite(view.fov) && view.fov > 0;
  }

  engine_camera::ManagerToken owner_{};
  Ids ids_{};
  std::uint64_t last_update_ = 0, closed_update_ = 0, first_update_ = 0;
  unsigned released_ = 0;
  bool pending_ = false, failed_ = false, closed_seen_ = false, resize_issued_ = false;
};

}  // namespace taxi_camera::native_camera
