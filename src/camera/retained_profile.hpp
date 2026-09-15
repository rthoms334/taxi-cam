#pragma once
#include "entry_pair.hpp"
#include "owned_view.hpp"

namespace taxi_camera::native_camera {
// A profile transition changes the aircraft adapter, never the native pair.
// Only numeric identity and dimension expectations survive an inspection.
class RetainedProfileTransition {
 public:
  using Pair = engine_camera::Snapshot;
  using Views = std::array<engine_camera::OwnedViewSnapshot, 2>;
  using Dimensions = std::array<decltype(engine_camera::OwnedViewSnapshot::dimensions), 2>;
  enum class Decision { wait, close, ready, refused };
  static bool session_changed(const Pair& pair, std::uint64_t started, std::uint64_t current) noexcept {
    return (pair.owned_ids[0] || pair.owned_ids[1]) && started != current;
  }
  void begin(std::uint32_t id, const Pair& pair, const Dimensions& dimensions) noexcept {
    id_ = id;
    owner_ = pair.owner;
    ids_ = pair.owned_ids;
    dimensions_ = dimensions;
    state_ = Decision::wait;
    if (!id || pair.request_pending || pair.creation_pending)
      state_ = Decision::refused;
    else if (!ids_[0] && !ids_[1])
      state_ = Decision::ready;
    else if (!valid_pair(pair))
      state_ = Decision::refused;
  }
  Decision inspect(engine_camera::ManagerToken manager, const Pair& pair, const Views& views) noexcept {
    if (state_ == Decision::refused)
      return state_;
    if (!matches(pair) || manager != owner_)
      return state_ = Decision::refused;
    bool open = false;
    for (unsigned i = 0; i < 2; ++i) {
      const auto& view = views[i];
      if (!view.complete || !view.ready || view.status == engine_camera::OwnedViewStatus::pending)
        return state_ = Decision::wait;
      if (view.status != engine_camera::OwnedViewStatus::ready || view.mode != 2 || !view.resource_present || !view.resource_address ||
          !view.view_address || !view.node_address || !view.camera_address || view.view_index < 0 || view.view_index >= 8 ||
          view.dimensions != dimensions_[i] || view.output_dimensions != dimensions_[i][0])
        return state_ = Decision::refused;
      open |= (view.flags[0] & 1u) == 0;
    }
    if (views[0].view_address == views[1].view_address || views[0].view_index == views[1].view_index ||
        views[0].resource_address == views[1].resource_address)
      return state_ = Decision::refused;
    return state_ = open ? Decision::close : Decision::ready;
  }
  bool matches(const Pair& pair) const noexcept { return valid_pair(pair) && pair.owner == owner_ && pair.owned_ids == ids_; }
  bool can_resume(const Pair& pair) const noexcept { return ready() && matches(pair); }
  void refuse() noexcept { state_ = Decision::refused; }
  void consume() noexcept { id_ = 0; }
  bool holding() const noexcept { return id_ != 0; }
  bool pending() const noexcept { return holding() && (state_ == Decision::wait || state_ == Decision::close); }
  bool ready() const noexcept { return holding() && state_ == Decision::ready; }
  bool failed() const noexcept { return holding() && state_ == Decision::refused; }
  std::uint32_t id() const noexcept { return id_; }

 private:
  static bool valid_pair(const Pair& pair) noexcept {
    return pair.state == engine_camera::State::active && pair.owner.valid() && pair.owned_ids[0] && pair.owned_ids[1] &&
           pair.owned_ids[0] != pair.owned_ids[1] && !pair.request_pending && !pair.creation_pending &&
           pair.failure == engine_camera::Failure::none && pair.blocked == engine_camera::Blocked::none;
  }
  std::uint32_t id_{};
  engine_camera::ManagerToken owner_{};
  std::array<engine_camera::EntryId, 2> ids_{};
  Dimensions dimensions_{};
  Decision state_ = Decision::wait;
};
}  // namespace taxi_camera::native_camera