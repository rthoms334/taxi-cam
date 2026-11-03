#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace taxi_camera::standalone::display_keep {
// Panel marks. GlassOut identifies each cockpit display by a two-pixel mark
// its page helper paints once at the gauge's top-left corner, and checks the
// mark in every frame it streams. A single camera write over those pixels
// hides that display from GlassOut until it identifies panels again (live,
// PMDG 777 DUS, 2026-11-03). Every camera write into a display therefore
// leaves each mark that falls inside the written area alone.
//
// Marks are texture coordinates, not tied to one texture: the worker lists
// the gauge corners and camera-area corners of the routed displays. A mark
// that lands on another texture costs that texture two unwritten pixels.
inline constexpr unsigned MarkWidth = 2, MarkHeight = 1, MaxMarks = 16;
// One writer (the bridge worker) and readers on simulator threads, which
// never wait for it. List k of the published generations lives in buffer
// k & 1; version is 2g after g publications and 2g + 1 while generation g + 1
// is written into the other buffer.
struct MarkList {
  std::array<std::atomic<std::uint32_t>, MaxMarks> marks{};  // x << 16 | y
  std::atomic<unsigned> count{};
};
inline std::array<MarkList, 2> lists{};
inline std::atomic<std::uint32_t> version{};
struct Point {
  unsigned x = 0, y = 0;
};
// Worker only. An empty list writes every area whole.
inline void set_marks(const Point* points, unsigned count) noexcept {
  count = std::min(count, MaxMarks);
  const auto published = version.load(std::memory_order_relaxed) >> 1;
  auto& list = lists[(published + 1) & 1];
  version.store(2 * published + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  for (unsigned i = 0; i < count; ++i)
    list.marks[i].store((std::min(points[i].x, 0xffffu) << 16) | std::min(points[i].y, 0xffffu), std::memory_order_relaxed);
  list.count.store(count, std::memory_order_relaxed);
  version.store(2 * published + 2, std::memory_order_release);
}
// The latest published list. The worker writes the other buffer, so a reader
// never waits; only a second rewrite during one read, at most once a second,
// can change the buffer under it. Then it tries again, and after a few tries
// uses the last list this thread read whole.
inline unsigned read_marks(std::array<std::uint32_t, MaxMarks>& out) noexcept {
  thread_local std::array<std::uint32_t, MaxMarks> last{};
  thread_local unsigned last_count = 0;
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    const auto published = version.load(std::memory_order_acquire) >> 1;
    const auto& list = lists[published & 1];
    const auto count = std::min(list.count.load(std::memory_order_relaxed), MaxMarks);
    for (unsigned i = 0; i < count; ++i)
      out[i] = list.marks[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    // Writing generation published + 2, into this buffer, starts at 2 * published + 3.
    if (version.load(std::memory_order_relaxed) < 2 * published + 3) {
      last = out;
      last_count = count;
      return count;
    }
  }
  out = last;
  return last_count;
}
// The pieces of an area that leave out every mark inside it, in rows from
// top to bottom. Without a mark inside, the area itself is the only piece.
struct Pieces {
  static constexpr unsigned Capacity = 2 * MaxMarks + 1 + MaxMarks;
  std::array<D3D12_RECT, Capacity> rects{};
  unsigned count = 0;
};
inline Pieces pieces(const D3D12_RECT& area) noexcept {
  Pieces out;
  std::array<D3D12_RECT, MaxMarks> inside{};
  unsigned n = 0;
  std::array<std::uint32_t, MaxMarks> listed{};
  const auto total = read_marks(listed);
  for (unsigned i = 0; i < total; ++i) {
    const auto value = listed[i];
    const LONG x = static_cast<LONG>(value >> 16), y = static_cast<LONG>(value & 0xffff);
    const D3D12_RECT mark{std::max(x, area.left), std::max(y, area.top), std::min<LONG>(x + MarkWidth, area.right),
                          std::min<LONG>(y + MarkHeight, area.bottom)};
    if (mark.left < mark.right && mark.top < mark.bottom)
      inside[n++] = mark;
  }
  if (!n || area.left >= area.right || area.top >= area.bottom) {
    out.rects[out.count++] = area;
    return out;
  }
  std::sort(inside.begin(), inside.begin() + n,
            [](const D3D12_RECT& a, const D3D12_RECT& b) { return a.top != b.top ? a.top < b.top : a.left < b.left; });
  const auto add = [&](LONG left, LONG top, LONG right, LONG bottom) noexcept {
    if (left < right && top < bottom && out.count < out.rects.size())
      out.rects[out.count++] = {left, top, right, bottom};
  };
  LONG row = area.top;
  for (unsigned i = 0; i < n;) {
    // Marks are one row high: every mark starting on this row ends on it.
    const LONG top = inside[i].top;
    add(area.left, row, area.right, top);
    LONG x = area.left;
    for (; i < n && inside[i].top == top; ++i) {
      add(x, top, inside[i].left, top + MarkHeight);
      x = std::max(x, inside[i].right);
    }
    add(x, top, area.right, top + MarkHeight);
    row = top + MarkHeight;
  }
  add(area.left, row, area.right, area.bottom);
  return out;
}
// CopyTextureRegion of source box {0, 0, width, height} to (x, y), split
// around the marks inside the destination area.
inline void copy_region(ID3D12GraphicsCommandList* list,
                        const D3D12_TEXTURE_COPY_LOCATION* destination,
                        UINT x,
                        UINT y,
                        const D3D12_TEXTURE_COPY_LOCATION* source,
                        UINT width,
                        UINT height) noexcept {
  const D3D12_RECT area{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + width), static_cast<LONG>(y + height)};
  const auto split = pieces(area);
  for (unsigned i = 0; i < split.count; ++i) {
    const auto& r = split.rects[i];
    const D3D12_BOX box{static_cast<UINT>(r.left) - x,  static_cast<UINT>(r.top) - y,    0,
                        static_cast<UINT>(r.right) - x, static_cast<UINT>(r.bottom) - y, 1};
    list->CopyTextureRegion(destination, static_cast<UINT>(r.left), static_cast<UINT>(r.top), 0, source, &box);
  }
}
}  // namespace taxi_camera::standalone::display_keep
