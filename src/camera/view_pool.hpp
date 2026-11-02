#pragma once

#include "memory_reader.hpp"

#include <array>
#include <cstdint>

namespace taxi_camera::engine_camera {

enum class ViewPoolStatus {
  not_inspected,
  complete,
  invalid_renderer,
  invalid_array,
  invalid_view,
  duplicate_view,
  invalid_release_queue,
  invalid_control,
  read_failed,
  changed,
  read_budget_exhausted
};

enum class ViewAssociation { unobserved, null_control, stale_generation, null_payload, occupied };

struct ViewSlot {
  std::uint32_t index = 0;
  // Internal addresses only. Do not serialize these into UI/log/JSON output.
  std::uint64_t view_address = 0;
  ViewAssociation association = ViewAssociation::unobserved;
  bool free = false;
  bool association_valid = false;
  bool release_pending = false;
  bool release_queued = false;
};

struct ViewPoolSnapshot {
  bool valid = false;
  ViewPoolStatus status = ViewPoolStatus::not_inspected;
  // Internal pointer; never a lifetime token or a reservation.
  std::uint64_t array_address = 0;
  std::uint32_t slots_examined = 0;
  std::int32_t failure_slot = -1;
  // Attempted bytes, including failed reads, capped at 8192. The current
  // eight-slot walk plus complete trace reread uses at most 592 bytes.
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
  std::array<ViewSlot, 8> slots{};
  // Published only after all slots and their consistency rechecks succeed.
  std::uint32_t free_count = 0;
  // Track enough leading free slots for a whole camera set (four views).
  // Smaller requests still require only their first slots.
  std::array<std::int32_t, 4> first_free_indices{-1, -1, -1, -1};
  bool release_checked = false;
  std::uint32_t release_count = 0;
  std::array<std::uint32_t, 2> release_queue_counts{};
  std::array<std::uint64_t, 128> release_views{};

  // The engine selects the first native-free slot, not the first unqueued one.
  bool creation_available(unsigned required) const noexcept {
    if (!valid || !release_checked || required == 0 || required > first_free_indices.size() || free_count < required)
      return false;
    for (unsigned i = 0; i < required; ++i) {
      const auto index = first_free_indices[i];
      if (index < 0 || index >= static_cast<std::int32_t>(slots.size()) || slots[index].release_pending || slots[index].release_queued)
        return false;
    }
    return true;
  }
};

// Reproduce the captured 66864720 availability predicate over eight EXISTING
// views. renderer+2752 contains a pointer to an array, not an inline array.
// Read only its eight view pointers, each P+72 sixteen-byte association, and
// nonnull control+28 generation / matching control+0 payload. Null/stale/null-
// payload handles are free; a null or duplicate P is an error. Reread every
// observed field at its exact address before publishing complete capacity.
// Control records may be byte-aligned; renderer, array and
// view pointers still require eight-byte alignment. No pointer bits are masked.
// Never call an engine function, reserve
// a slot, mutate memory, query the cached global or follow an association payload.
// The adapter owns readable-region checks and cached-renderer/build validation.
// Call in the same engine phase as prospective creation: this detects observed
// changes but is not atomic, a lifetime guarantee or a capacity reservation.
// The reader must not throw; report unavailable memory by returning false.
// This helper is not an SEH fault barrier.
ViewPoolSnapshot inspect_view_pool(MemoryReader& reader, std::uint64_t renderer) noexcept;

// Creation-only extension. The caller must first verify the renderer release
// method contracts. Captures P+23688 and both renderer+2768/+2784 release queues,
// then rechecks the entire association/marker/queue trace together. A cleared
// association alone is never evidence that native deferred release has ended.
ViewPoolSnapshot inspect_view_creation_pool(MemoryReader& reader, std::uint64_t renderer) noexcept;

// These addresses are comparison tokens, never dereferenced after retirement.
// Keep them beyond native ID disappearance until the current pool and release
// queue prove the old views are no longer pending. Only native code clears the
// marker/replaces the slot. A different renderer cannot prove old retirement.
class RetiredViewPool {
 public:
  bool retain(std::uint64_t renderer, std::uint64_t view) noexcept {
    if (!renderer || !view || (renderer_ && renderer_ != renderer))
      return false;
    for (auto old : views_)
      if (old == view)
        return true;
    for (auto& old : views_)
      if (!old) {
        renderer_ = renderer;
        old = view;
        return true;
      }
    return false;
  }
  bool pending() const noexcept {
    for (auto view : views_)
      if (view)
        return true;
    return false;
  }
  bool observe(std::uint64_t renderer, const ViewPoolSnapshot& pool) noexcept {
    if (!pending())
      return true;
    if (renderer != renderer_ || !pool.valid || !pool.release_checked)
      return false;
    for (auto& old : views_) {
      if (!old)
        continue;
      bool pending = false;
      for (unsigned i = 0; i < pool.release_count; ++i)
        pending |= pool.release_views[i] == old;
      for (const auto& slot : pool.slots)
        pending |= slot.view_address == old && slot.release_pending;
      if (!pending)
        old = 0;
    }
    if (!pending())
      renderer_ = 0;
    return !pending();
  }

 private:
  std::uint64_t renderer_{};
  // Capacity matches the four owned camera views so a full set can retire
  // without dropping the last identity and blocking later creation.
  std::array<std::uint64_t, 4> views_{};
};

}  // namespace taxi_camera::engine_camera
