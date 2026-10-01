#pragma once
#include "entry_pair.hpp"
#include "owned_view.hpp"

namespace taxi_camera::native_camera {
// A profile transition changes the aircraft adapter, never the native pair.
// Only numeric identity and dimension expectations survive an inspection.
class RetainedProfileTransition {
 public:
  using Pair = engine_camera::Snapshot;
  using Views = std::array<engine_camera::OwnedViewSnapshot, 3>;
  using Dimensions = std::array<decltype(engine_camera::OwnedViewSnapshot::dimensions), 3>;
  struct AllocationEvidence {
    engine_camera::ManagerToken owner{};
    std::array<engine_camera::EntryId, 3> ids{};
    Dimensions dimensions{};
    bool matches(const Pair& pair) const noexcept { return owner == pair.owner && ids == pair.owned_ids; }
  };
  enum class Decision { wait, close, ready, refused };
  static bool session_changed(const Pair& pair, std::uint64_t started, std::uint64_t current) noexcept {
    return (pair.owned_ids[0] || pair.owned_ids[1] || pair.owned_ids[2]) && started != current;
  }
  void begin(std::uint32_t id, const Pair& pair, const Dimensions& dimensions) noexcept {
    awaiting_pair_ = false;
    id_ = id;
    owner_ = pair.owner;
    ids_ = pair.owned_ids;
    dimensions_ = dimensions;
    resizing_ = false;
    state_ = Decision::wait;
    if (!id || pair.request_pending || pair.creation_pending)
      state_ = Decision::refused;
    else if (!ids_[0] && !ids_[1] && !ids_[2])
      state_ = Decision::ready;
    else if (!valid_pair(pair))
      state_ = Decision::refused;
  }
  // The controller is still processing. Its published empty snapshot cannot
  // prove absence until cancel_uncreated_request or a fresh owned pair settles.
  void defer_pair(std::uint32_t id) noexcept {
    id_ = id;
    owner_ = {};
    ids_ = {};
    dimensions_ = {};
    awaiting_pair_ = id != 0;
    state_ = id ? Decision::wait : Decision::refused;
  }
  // PairController publishes ownership before the probe can publish allocation
  // expectations. Never bind a new pair to an earlier pair's dimensions during
  // that gap; the observer will publish matching evidence before retrying.
  void begin_published(std::uint32_t id, const Pair& pair, const AllocationEvidence& evidence) noexcept {
    if (id && valid_pair(pair) && !evidence.matches(pair))
      defer_pair(id);
    else
      begin(id, pair, evidence.dimensions);
  }
  Decision inspect(engine_camera::ManagerToken manager, const Pair& pair, const Views& views) noexcept {
    if (state_ == Decision::refused)
      return state_;
    if (awaiting_pair_)
      return Decision::wait;
    if (!matches(pair) || manager != owner_)
      return state_ = Decision::refused;
    bool open = false;
    const unsigned feeds = ids_[2] ? 3u : 2u;
    for (unsigned i = 0; i < feeds; ++i) {
      const auto& view = views[i];
      if (!view.complete || !view.ready || view.status == engine_camera::OwnedViewStatus::pending)
        return state_ = Decision::wait;
      if (view.status != engine_camera::OwnedViewStatus::ready || view.mode != 2 || !view.resource_present || !view.resource_address ||
          !view.view_address || !view.node_address || !view.camera_address || view.view_index < 0 || view.view_index >= 8 ||
          view.dimensions != dimensions_[i] || (!resizing_ && view.output_dimensions != dimensions_[i][0]))
        return state_ = Decision::refused;
      open |= (view.flags[0] & 1u) == 0;
    }
    for (unsigned i = 0; i < feeds; ++i)
      for (unsigned j = i + 1; j < feeds; ++j)
        if (views[i].view_address == views[j].view_address || views[i].view_index == views[j].view_index ||
            views[i].resource_address == views[j].resource_address)
          return state_ = Decision::refused;
    return state_ = open ? Decision::close : Decision::ready;
  }
  bool matches(const Pair& pair) const noexcept { return valid_pair(pair) && pair.owner == owner_ && pair.owned_ids == ids_; }
  // The kept pair is being resized in place for another aircraft's panes: the
  // views must carry the new dimensions, while their new output may still be
  // pending until resized() (resize_kept_panes).
  void rebase(const Dimensions& dimensions) noexcept {
    dimensions_ = dimensions;
    resizing_ = true;
  }
  void resized() noexcept { resizing_ = false; }
  bool can_resume(const Pair& pair) const noexcept { return ready() && matches(pair); }
  void refuse() noexcept {
    awaiting_pair_ = false;
    state_ = Decision::refused;
  }
  void consume() noexcept {
    id_ = 0;
    awaiting_pair_ = false;
  }
  bool holding() const noexcept { return id_ != 0; }
  bool awaiting_pair() const noexcept { return holding() && awaiting_pair_; }
  bool pending() const noexcept { return holding() && (state_ == Decision::wait || state_ == Decision::close); }
  bool ready() const noexcept { return holding() && state_ == Decision::ready; }
  bool failed() const noexcept { return holding() && state_ == Decision::refused; }
  std::uint32_t id() const noexcept { return id_; }

 private:
  static bool valid_pair(const Pair& pair) noexcept {
    if (pair.state != engine_camera::State::active || !pair.owner.valid() || !pair.owned_ids[0] || !pair.owned_ids[1] ||
        pair.owned_ids[0] == pair.owned_ids[1] || pair.request_pending || pair.creation_pending ||
        pair.failure != engine_camera::Failure::none || pair.blocked != engine_camera::Blocked::none)
      return false;
    if (pair.owned_ids[2] && (pair.owned_ids[2] == pair.owned_ids[0] || pair.owned_ids[2] == pair.owned_ids[1]))
      return false;
    return true;
  }
  std::uint32_t id_{};
  engine_camera::ManagerToken owner_{};
  std::array<engine_camera::EntryId, 3> ids_{};
  Dimensions dimensions_{};
  bool awaiting_pair_ = false, resizing_ = false;
  Decision state_ = Decision::wait;
};
}  // namespace taxi_camera::native_camera
