#pragma once

#include "entry_pair.hpp"
#include "owned_view.hpp"

#include <cmath>

namespace taxi_camera::native_camera {

// Serialized by the caller in the manager observer phase. Retains only opaque
// owner/entry identities and update counters, never borrowed view pointers.
// Closed gates across CPU updates do NOT prove GPU or native output lifetime;
// the caller must establish that separately before acting on resize.
class ViewResizeRecovery {
 public:
  enum class Action { wait, close_gates, resize, blocked };
  using Ids = std::array<engine_camera::EntryId, 2>;
  using Views = std::array<engine_camera::OwnedViewSnapshot, 2>;

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
    bool temporary = false;
    for (const auto& view : views) {
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
    if (views[0].view_address == views[1].view_address || views[0].view_index == views[1].view_index) {
      mark_failed();
      return Action::blocked;
    }
    // A native attempt consumes this authorization. The caller must finish on
    // complete success or mark_failed on ANY refused/partial mutation.
    if (resize_issued_)
      return Action::wait;
    if (!(views[0].flags[0] & 1u) || !(views[1].flags[0] & 1u)) {
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
  bool pending() const noexcept { return pending_; }
  bool failed() const noexcept { return failed_; }
  void clear() noexcept { *this = {}; }  // Explicit lifecycle reset, never automatic retry.
  void mark_failed() noexcept {
    failed_ = true;
    closed_seen_ = resize_issued_ = false;
  }

 private:
  static bool valid_identity(engine_camera::ManagerToken owner, const Ids& ids) noexcept {
    return owner.valid() && ids[0] && ids[1] && ids[0] != ids[1];
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
  std::uint64_t last_update_ = 0, closed_update_ = 0;
  bool pending_ = false, failed_ = false, closed_seen_ = false, resize_issued_ = false;
};

}  // namespace taxi_camera::native_camera
