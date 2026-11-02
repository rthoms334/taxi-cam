#pragma once

#include <algorithm>
#include "entry_pair.hpp"
#include "owned_view.hpp"

namespace taxi_camera::native_camera {

// Established views may briefly be pending or fail a consistency read during
// allocation. Waiting grants no permission to use the failed snapshot. The
// caller must classify the inspection as transient; no borrowed pointers survive.
class ViewReadinessWait {
 public:
  bool observe(std::uint64_t now,
               const engine_camera::Snapshot& pair,
               bool established,
               const std::array<engine_camera::OwnedViewSnapshot, engine_camera::kMaxOwnedViews>& views) noexcept {
    bool pending = false;
    bool valid = established && pair.state == engine_camera::State::active && pair.owner.valid() &&
                 engine_camera::distinct_owned(pair.owned_ids) && !pair.request_pending && !pair.creation_pending &&
                 pair.failure == engine_camera::Failure::none && pair.blocked == engine_camera::Blocked::none;
    const unsigned feeds = std::max(2u, engine_camera::owned_count(pair.owned_ids));
    for (unsigned vi = 0; vi < feeds; ++vi) {
      const auto& view = views[vi];
      const bool stable_pending = view.complete && !view.ready && view.status == engine_camera::OwnedViewStatus::pending;
      const bool transient_read =
          !view.complete && !view.ready &&
          (view.status == engine_camera::OwnedViewStatus::read_failed || view.status == engine_camera::OwnedViewStatus::changed ||
           view.status == engine_camera::OwnedViewStatus::pool_changed || view.status == engine_camera::OwnedViewStatus::not_inspected);
      valid &= stable_pending || transient_read || (view.complete && view.ready && view.status == engine_camera::OwnedViewStatus::ready);
      pending |= stable_pending || transient_read;
    }
    if (!valid || !pending) {
      clear();
      return false;
    }
    if (!waiting_) {
      waiting_ = true;
      owner_ = pair.owner;
      ids_ = pair.owned_ids;
      since_ = now;
      ++episodes_;
    }
    // Time passing cannot authorize destruction of an unavailable view. A different pair or
    // manager cannot inherit the previous wait, even if native addresses recur.
    return pair.owner == owner_ && pair.owned_ids == ids_ && now >= since_;
  }
  void clear() noexcept {
    waiting_ = false;
    owner_ = {};
    ids_ = {};
    since_ = 0;
  }
  std::uint64_t episodes() const noexcept { return episodes_; }

 private:
  bool waiting_ = false;
  engine_camera::ManagerToken owner_{};
  engine_camera::OwnedIds ids_{};
  std::uint64_t since_ = 0, episodes_ = 0;
};

}  // namespace taxi_camera::native_camera
