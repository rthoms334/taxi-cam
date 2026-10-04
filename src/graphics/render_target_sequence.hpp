#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace taxi_camera {
// Render-target creations in order, for display identity research: the
// simulator creates one texture per textured VCockpit panel during cockpit
// load, and that order is compared with the panel table. Creation hooks write
// a slot and publish it with its sequence number; a reader copies only slots
// whose sequence matches before and after the copy. Bounded and lock-free; the
// oldest entries are overwritten.
class RenderTargetSequence {
 public:
  static constexpr std::size_t Capacity = 2048;
  static constexpr std::uint32_t MinimumEdge = 256;
  struct Entry {
    std::uint64_t sequence{}, id{}, tick{};
    std::uint32_t width{}, height{}, mips{}, format{};
  };
  void record(std::uint32_t width,
              std::uint32_t height,
              std::uint32_t mips,
              std::uint32_t format,
              std::uint64_t id,
              std::uint64_t tick) noexcept {
    if (width < MinimumEdge || height < MinimumEdge)
      return;
    const auto sequence = next_.fetch_add(1, std::memory_order_relaxed) + 1;
    auto& slot = slots_[(sequence - 1) % Capacity];
    slot.published.store(0, std::memory_order_release);
    slot.id.store(id, std::memory_order_relaxed);
    slot.tick.store(tick, std::memory_order_relaxed);
    slot.shape.store(std::uint64_t{width} << 40 | std::uint64_t{height} << 24 | std::uint64_t{mips & 0xff} << 16 | (format & 0xffff),
                     std::memory_order_relaxed);
    slot.published.store(sequence, std::memory_order_release);
  }
  // Entries with sequence > after, oldest first. Returns the number copied.
  template <std::size_t N>
  std::size_t snapshot(std::uint64_t after, std::array<Entry, N>& out) const noexcept {
    const auto newest = next_.load(std::memory_order_acquire);
    std::uint64_t first = newest > Capacity ? newest - Capacity + 1 : 1;
    if (first <= after)
      first = after + 1;
    std::size_t n = 0;
    for (auto sequence = first; sequence <= newest && n < N; ++sequence) {
      const auto& slot = slots_[(sequence - 1) % Capacity];
      if (slot.published.load(std::memory_order_acquire) != sequence)
        continue;
      Entry entry{sequence, slot.id.load(std::memory_order_relaxed), slot.tick.load(std::memory_order_relaxed)};
      const auto shape = slot.shape.load(std::memory_order_relaxed);
      if (slot.published.load(std::memory_order_acquire) != sequence)
        continue;
      entry.width = static_cast<std::uint32_t>(shape >> 40 & 0xffff);
      entry.height = static_cast<std::uint32_t>(shape >> 24 & 0xffff);
      entry.mips = static_cast<std::uint32_t>(shape >> 16 & 0xff);
      entry.format = static_cast<std::uint32_t>(shape & 0xffff);
      out[n++] = entry;
    }
    return n;
  }
  std::uint64_t newest() const noexcept { return next_.load(std::memory_order_acquire); }

 private:
  struct Slot {
    std::atomic<std::uint64_t> published{}, id{}, tick{}, shape{};
  };
  std::array<Slot, Capacity> slots_{};
  std::atomic<std::uint64_t> next_{};
};
}  // namespace taxi_camera
