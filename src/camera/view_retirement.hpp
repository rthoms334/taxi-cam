#pragma once

#include "entry_pair.hpp"
#include "owned_view.hpp"

namespace taxi_camera::native_camera {

// No borrowed pointers are retained. A complete ready chain and a closed gate
// must be observed on distinct manager updates before calling native removal.
// This is a CPU lifecycle precondition, not a GPU fence: the engine's normal
// deferred view release remains responsible for renderer retirement.
class ViewRetirement {
 public:
  enum class Action { wait, close_gate, erase };

  Action observe(engine_camera::ManagerToken owner,
                 engine_camera::EntryId id,
                 std::uint64_t update,
                 const engine_camera::OwnedViewSnapshot& view) noexcept {
    if (!owner.valid() || !id)
      return Action::wait;
    Slot* slot = nullptr;
    for (auto& candidate : slots_)
      if (candidate.owner == owner && candidate.id == id) {
        slot = &candidate;
        break;
      }
    if (!view.complete || !view.ready || view.status != engine_camera::OwnedViewStatus::ready || !view.view_address || !view.node_address ||
        !view.camera_address || view.view_index < 0 || view.view_index >= 8) {
      if (slot)
        *slot = {};
      return Action::wait;
    }
    if ((view.flags[0] & 1u) == 0) {
      if (slot)
        *slot = {};
      return Action::close_gate;
    }
    if (!slot) {
      for (auto& candidate : slots_)
        if (!candidate.id) {
          candidate = {owner, id, update};
          return Action::wait;
        }
      return Action::wait;
    }
    if (update < slot->closed_update) {
      *slot = {};
      return Action::wait;
    }
    return update > slot->closed_update ? Action::erase : Action::wait;
  }

  void forget(engine_camera::ManagerToken owner, engine_camera::EntryId id) noexcept {
    for (auto& slot : slots_)
      if (slot.owner == owner && slot.id == id)
        slot = {};
  }
  void clear() noexcept { slots_ = {}; }

 private:
  struct Slot {
    engine_camera::ManagerToken owner{};
    engine_camera::EntryId id = 0;
    std::uint64_t closed_update = 0;
  };
  std::array<Slot, 2> slots_{};
};

}  // namespace taxi_camera::native_camera
