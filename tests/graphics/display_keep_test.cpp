#include "../../src/graphics/display_keep.hpp"

#include <cstdio>
#include <stdexcept>

namespace {
namespace keep = taxi_camera::standalone::display_keep;
unsigned checks{};
void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}
bool same(const D3D12_RECT& r, LONG left, LONG top, LONG right, LONG bottom) {
  return r.left == left && r.top == top && r.right == right && r.bottom == bottom;
}
// Pieces never overlap a mark or each other, and cover the rest of the area.
void covers(const D3D12_RECT& area, const keep::Pieces& split, const keep::Point* points, unsigned count) {
  for (LONG y = area.top; y < area.bottom; ++y)
    for (LONG x = area.left; x < area.right; ++x) {
      bool marked = false;
      for (unsigned m = 0; m < count; ++m)
        marked = marked || (x >= static_cast<LONG>(points[m].x) && x < static_cast<LONG>(points[m].x + keep::MarkWidth) &&
                            y >= static_cast<LONG>(points[m].y) && y < static_cast<LONG>(points[m].y + keep::MarkHeight));
      unsigned hits = 0;
      for (unsigned i = 0; i < split.count; ++i)
        hits += x >= split.rects[i].left && x < split.rects[i].right && y >= split.rects[i].top && y < split.rects[i].bottom;
      require(hits == (marked ? 0u : 1u), "Every unmarked pixel written once, no marked pixel written");
    }
}
void pieces() {
  keep::set_marks(nullptr, 0);
  const D3D12_RECT du{1058, 33, 2016, 1004};
  auto split = keep::pieces(du);
  require(split.count == 1 && same(split.rects[0], 1058, 33, 2016, 1004), "No marks: the area whole");
  // The live PMDG 777 case: GlassOut's mark at the gauge corner (1058,33).
  const keep::Point gauge[]{{1058, 33}};
  keep::set_marks(gauge, 1);
  split = keep::pieces(du);
  require(split.count == 2 && same(split.rects[0], 1060, 33, 2016, 34) && same(split.rects[1], 1058, 34, 2016, 1004),
          "A mark at the area's corner");
  // The user moved the area's top to 0: the gauge corner is inside it.
  const D3D12_RECT moved{1058, 0, 2016, 1004};
  split = keep::pieces(moved);
  require(split.count == 3 && same(split.rects[0], 1058, 0, 2016, 33), "Rows above the mark first");
  covers({1050, 25, 1070, 40}, keep::pieces({1050, 25, 1070, 40}), gauge, 1);
  // A mark outside the area, or on another texture's coordinates, changes nothing.
  split = keep::pieces({0, 0, 958, 971});
  require(split.count == 1, "A mark outside the area");
  // Several marks, overlapping and on one row, clipped at the area's edge.
  const keep::Point many[]{{10, 5}, {11, 5}, {30, 5}, {39, 9}, {0, 0}, {50, 50}};
  keep::set_marks(many, 6);
  const D3D12_RECT small{0, 0, 40, 12};
  split = keep::pieces(small);
  covers(small, split, many, 6);
  require(split.count <= keep::Pieces::Capacity, "Within capacity");
  // Restores leave out only gauge marks: an area mark goes back whole.
  const keep::Point mixed[]{{10, 5, true}, {30, 5, false}};
  keep::set_marks(mixed, 2);
  covers(small, keep::pieces(small, true), mixed, 1);
  require(keep::pieces(small).count > keep::pieces(small, true).count, "An area mark is left out of camera writes only");
  keep::set_marks(nullptr, 0);
  require(keep::pieces(small).count == 1, "An empty list writes the area whole");
}
// A reader takes the published list without waiting, even while the worker
// is writing the next one.
void publication() {
  const keep::Point first[]{{1, 2}, {3, 4}}, second[]{{5, 6}};
  std::array<std::uint32_t, keep::MaxMarks> out{};
  keep::set_marks(first, 2);
  require(keep::read_marks(out) == 2 && out[0] == (1u << 16 | 2) && out[1] == (3u << 16 | 4), "The published list");
  const keep::Point gauge[]{{0x7fff, 0xffff, true}};
  keep::set_marks(gauge, 1);
  require(keep::read_marks(out) == 1 && out[0] == (1u << 31 | 0x7fffu << 16 | 0xffff), "A gauge mark keeps its flag and position");
  keep::set_marks(first, 2);
  const auto stable = keep::version.load();
  keep::version.store(stable + 1);  // The worker is writing the other buffer.
  require(keep::read_marks(out) == 2 && out[0] == (1u << 16 | 2), "The published list while the next is written");
  keep::version.store(stable);
  keep::set_marks(second, 1);
  require(keep::read_marks(out) == 1 && out[0] == (5u << 16 | 6), "The next list once published");
  keep::set_marks(nullptr, 0);
  require(keep::read_marks(out) == 0, "An empty list");
}
}  // namespace

int main() {
  try {
    pieces();
    publication();
    std::printf("Display keep checks: %u passed\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Display keep test failed: %s\n", error.what());
    return 1;
  }
}
