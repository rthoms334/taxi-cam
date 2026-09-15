#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace taxi_camera::standalone {
// Recording-local admission for extra graphics work, not validation of the
// application's heap contents. Heap identities are compared, never dereferenced.
// The owner serializes access and must observe the complete recording, including
// query setters on alternate native tables. Late registration starts unknown.
class QueryScope {
 public:
  static constexpr std::size_t capacity = 64;

  // Only an observed successful native Reset establishes an empty recording.
  // Failure, late attachment, and any observer gap must leave it unknown.
  void reset(bool observed_success) noexcept {
    count_ = 0;
    known_ = observed_success;
  }
  void invalidate() noexcept { known_ = false; }
  // ClearState resets graphics bindings, not an outstanding Begin/End query.
  void clear_state() noexcept {}

  bool begin(std::uint64_t heap, std::uint32_t type, std::uint32_t index) noexcept {
    if (!known_)
      return false;
    if (!heap || !paired_type(type) || count_ == capacity)
      return poison();
    const Key key{heap, type, index};
    for (std::size_t i = 0; i < count_; ++i) {
      // A second Begin on one heap element is invalid even when its type differs.
      if (entries_[i].heap == heap && entries_[i].index == index)
        return poison();
    }
    entries_[count_++] = key;
    return true;
  }

  // Call AFTER the original EndQuery has returned. Until then the query must
  // remain open so nested admission cannot insert work inside that query.
  bool end(std::uint64_t heap, std::uint32_t type, std::uint32_t index) noexcept {
    if (!known_)
      return false;
    if (!heap)
      return poison();
    if (type == 2)  // D3D12_QUERY_TYPE_TIMESTAMP has no BeginQuery.
      return true;
    if (!paired_type(type))
      return poison();
    const Key key{heap, type, index};
    for (std::size_t i = 0; i < count_; ++i) {
      if (entries_[i] == key) {
        entries_[i] = entries_[--count_];
        return true;
      }
    }
    return poison();
  }

  // A destroyed heap must not let a replacement at the same address close an
  // old query. Retirement of a heap with no open query leaves scope unchanged.
  void retire(std::uint64_t heap) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (entries_[i].heap == heap) {
        invalidate();
        return;
      }
    }
  }

  bool known() const noexcept { return known_; }
  bool known_empty() const noexcept { return known_ && count_ == 0; }
  bool can_stamp() const noexcept { return known_empty(); }
  std::size_t active_count() const noexcept { return count_; }

 private:
  // Public D3D12_QUERY_TYPE numeric ABI; the test pins these to d3d12.h.
  // VIDEO_DECODE_STATISTICS (8) is End-only on a VIDEO_DECODE list, so is not
  // admitted by this graphics-list tracker. Unknown future values fail closed.
  // https://learn.microsoft.com/windows/win32/api/d3d12/ne-d3d12-d3d12_query_type
  static constexpr bool paired_type(std::uint32_t type) noexcept {
    return type == 0 || type == 1 || (type >= 3 && type <= 7) || type == 10;
  }
  struct Key {
    std::uint64_t heap;
    std::uint32_t type;
    std::uint32_t index;
    bool operator==(const Key&) const noexcept = default;
  };
  bool poison() noexcept {
    invalidate();
    return false;
  }
  std::array<Key, capacity> entries_{};
  std::size_t count_ = 0;
  bool known_ = false;
};
}  // namespace taxi_camera::standalone
