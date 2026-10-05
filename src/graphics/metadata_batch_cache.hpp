#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace taxi_camera::standalone {
// Thread-confined metadata only. Each scope borrows its record: the caller
// keeps it alive from begin until the matching end, and end clears the slot,
// so no record pointer outlives its scope. No application COM reference or
// mutex survives begin. Overflow uses ordinary lookup, and leaving a nested
// scope restores the exact outer context.
template <class Record, class Pointer, std::size_t Capacity = 8>
class MetadataBatchCache {
 public:
  void begin(Pointer native, std::uint64_t generation, Record* record) noexcept {
    if (depth_ < Capacity) {
      auto& slot = scopes_[depth_];
      slot.native = native;
      slot.generation = generation;
      slot.recording = record ? static_cast<std::uint64_t>(record->recording) : 0;
      slot.record = record;
    }
    ++depth_;
  }
  void end() noexcept {
    if (depth_ && --depth_ < Capacity)
      scopes_[depth_] = {};
  }
  Record* current(Pointer native, std::uint64_t generation) const noexcept {
    if (!depth_ || depth_ > Capacity)
      return nullptr;
    const auto& slot = scopes_[depth_ - 1];
    auto* const item = slot.record;
    return slot.native == native && slot.generation == generation && item && item->alive && item->id == generation &&
                   item->recording == slot.recording
               ? item
               : nullptr;
  }

 private:
  struct Scope {
    Pointer native{};
    std::uint64_t generation{}, recording{};
    Record* record{};
  };
  std::array<Scope, Capacity> scopes_{};
  std::size_t depth_{};
};
}  // namespace taxi_camera::standalone
