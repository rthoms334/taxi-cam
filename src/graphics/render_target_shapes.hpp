#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace taxi_camera {
// Distinct 2D render-target shapes created by the simulator, for finding a new
// aircraft's display textures before its profile exists. Creation hooks record
// the description only: no resource is retained, drawn or tracked for activity.
// Lock-free and bounded; shapes beyond Capacity are counted as overflow.
class RenderTargetShapes {
 public:
  static constexpr std::size_t Capacity = 128;
  // Smaller targets are glyph, icon and reduction buffers, not displays.
  static constexpr std::uint32_t MinimumEdge = 256;
  static constexpr std::uint32_t MaximumEdge = 16384;
  struct Shape {
    std::uint32_t width{}, height{}, mips{}, format{};
    std::uint64_t created{}, first_ms{}, last_ms{};
  };
  bool record(std::uint32_t width, std::uint32_t height, std::uint32_t mips, std::uint32_t format, std::uint64_t now) noexcept {
    if (width < MinimumEdge || height < MinimumEdge || width > MaximumEdge || height > MaximumEdge || mips > 0xff || format > 0xffff)
      return false;
    const std::uint64_t key = Occupied | std::uint64_t{width} << 40 | std::uint64_t{height} << 24 | std::uint64_t{mips} << 16 | format;
    for (auto& slot : slots_) {
      auto current = slot.key.load(std::memory_order_acquire);
      if (!current && slot.key.compare_exchange_strong(current, key, std::memory_order_acq_rel)) {
        slot.first_ms.store(now, std::memory_order_relaxed);
        distinct_.fetch_add(1, std::memory_order_relaxed);
        current = key;
      }
      if (current == key) {
        // Concurrent creators can arrive out of order; keep the latest tick.
        auto last = slot.last_ms.load(std::memory_order_relaxed);
        while (last < now && !slot.last_ms.compare_exchange_weak(last, now, std::memory_order_relaxed)) {
        }
        slot.created.fetch_add(1, std::memory_order_relaxed);
        total_.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    }
    overflow_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // Oldest first; a shape whose first creation is still being published is skipped.
  std::size_t snapshot(std::array<Shape, Capacity>& out) const noexcept {
    std::size_t n = 0;
    for (const auto& slot : slots_) {
      const auto key = slot.key.load(std::memory_order_acquire);
      const auto first = slot.first_ms.load(std::memory_order_relaxed);
      if (!key || !first)
        continue;
      out[n++] = {static_cast<std::uint32_t>(key >> 40 & 0xffff), static_cast<std::uint32_t>(key >> 24 & 0xffff),
                  static_cast<std::uint32_t>(key >> 16 & 0xff),   static_cast<std::uint32_t>(key & 0xffff),
                  slot.created.load(std::memory_order_relaxed), first,
                  slot.last_ms.load(std::memory_order_relaxed)};
    }
    std::sort(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n),
              [](const Shape& a, const Shape& b) { return a.first_ms < b.first_ms; });
    return n;
  }
  std::uint64_t distinct() const noexcept { return distinct_.load(std::memory_order_relaxed); }
  std::uint64_t total() const noexcept { return total_.load(std::memory_order_relaxed); }
  std::uint64_t overflow() const noexcept { return overflow_.load(std::memory_order_relaxed); }

 private:
  static constexpr std::uint64_t Occupied = 1ull << 63;
  struct Slot {
    std::atomic<std::uint64_t> key{}, created{}, first_ms{}, last_ms{};
  };
  std::array<Slot, Capacity> slots_{};
  std::atomic<std::uint64_t> distinct_{}, total_{}, overflow_{};
};
}  // namespace taxi_camera
