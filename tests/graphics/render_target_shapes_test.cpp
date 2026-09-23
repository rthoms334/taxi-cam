#include <cassert>
#include <cstdio>
#include "../../src/graphics/render_target_shapes.hpp"

int main() {
  using taxi_camera::RenderTargetShapes;
  static RenderTargetShapes shapes;
  static std::array<RenderTargetShapes::Shape, RenderTargetShapes::Capacity> out;
  assert(!shapes.snapshot(out) && !shapes.distinct() && !shapes.total());

  // Repeated creations of one shape share a slot; mips and format distinguish shapes.
  assert(shapes.record(4096, 4096, 1, 27, 200));
  assert(shapes.record(768, 1024, 1, 27, 100));
  assert(shapes.record(4096, 4096, 1, 27, 300));
  assert(shapes.record(4096, 4096, 12, 27, 400));
  assert(shapes.record(4096, 4096, 1, 28, 500));
  assert(shapes.distinct() == 4 && shapes.total() == 5);
  auto n = shapes.snapshot(out);
  assert(n == 4);
  // Oldest first, with creation count and first/last creation times.
  assert(out[0].width == 768 && out[0].height == 1024 && out[0].created == 1 && out[0].first_ms == 100);
  assert(out[1].width == 4096 && out[1].mips == 1 && out[1].format == 27 && out[1].created == 2);
  assert(out[1].first_ms == 200 && out[1].last_ms == 300);
  assert(out[2].mips == 12 && out[3].format == 28);
  // A creator reporting an older tick late does not move the last creation back.
  assert(shapes.record(4096, 4096, 1, 27, 250));
  n = shapes.snapshot(out);
  assert(out[1].created == 3 && out[1].first_ms == 200 && out[1].last_ms == 300);

  // Small and oversized targets are not display candidates.
  assert(!shapes.record(255, 1024, 1, 27, 600) && !shapes.record(1024, 128, 1, 27, 600));
  assert(!shapes.record(16385, 1024, 1, 27, 600) && !shapes.record(1024, 1024, 256, 27, 600));
  assert(shapes.record(16384, 256, 1, 27, 600));
  assert(shapes.distinct() == 5 && shapes.overflow() == 0);

  // The table is bounded: distinct shapes beyond capacity count as overflow.
  for (std::uint32_t i = 0; shapes.distinct() < RenderTargetShapes::Capacity; ++i)
    assert(shapes.record(1000 + i, 1000, 1, 27, 700 + i));
  assert(!shapes.record(9000, 9000, 1, 27, 900) && shapes.overflow() == 1);
  assert(shapes.record(768, 1024, 1, 27, 1000));
  n = shapes.snapshot(out);
  assert(n == RenderTargetShapes::Capacity && out[0].created == 2 && out[0].last_ms == 1000);
  std::puts("render target shapes: ok");
}
