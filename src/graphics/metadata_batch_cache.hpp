#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace taxi_camera::standalone {
// Thread-confined metadata only. Each scope keeps its own shared record alive;
// no application COM reference or mutex survives begin. Overflow uses ordinary
// lookup, and leaving a nested scope restores the exact outer context.
template <class Record, class Pointer, std::size_t Capacity = 8>
class MetadataBatchCache {
 public:
  void begin(Pointer native, std::uint64_t generation, std::shared_ptr<Record> record) noexcept {
    if (depth_ < Capacity) {
      auto& slot = scopes_[depth_];
      slot.native = native;
      slot.generation = generation;
      slot.recording = record ? record->recording : 0;
      slot.record = std::move(record);
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
    const auto& item = slot.record;
    return slot.native == native && slot.generation == generation && item && item->alive && item->id == generation &&
                   item->recording == slot.recording
               ? item.get()
               : nullptr;
  }

 private:
  struct Scope {
    Pointer native{};
    std::uint64_t generation{}, recording{};
    std::shared_ptr<Record> record;
  };
  std::array<Scope, Capacity> scopes_{};
  std::size_t depth_{};
};
}  // namespace taxi_camera::standalone