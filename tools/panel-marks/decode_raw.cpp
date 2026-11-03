#include <cstdio>
#include <cstdlib>
#include <vector>
#include "panel_marks.hpp"
using namespace taxi_camera::panel_marks;
int main(int argc, char** argv) {
  if (argc < 4)
    return 2;
  const unsigned w = std::atoi(argv[2]), h = std::atoi(argv[3]);
  std::vector<unsigned char> px(std::size_t(w) * h * 4);
  FILE* f = std::fopen(argv[1], "rb");
  if (!f || std::fread(px.data(), 1, px.size(), f) != px.size())
    return 3;
  std::fclose(f);
  Mark marks[32];
  const auto n = find_marks(px.data(), std::uint64_t(w) * 4, w, h, false, marks, 32);
  std::printf("found=%zu\n", n);
  for (std::size_t i = 0; i < n; ++i) {
    Rect r;
    texture_rect(marks[i], w, h, r);
    std::printf("  %s name=%s gauge=%u %s rect=%u,%u %ux%u pitch=%.2f cell=%u info=%.60s\n",
                marks[i].kind == Kind::panel ? "panel" : "gauge", marks[i].name.data(), marks[i].gauge, marks[i].gauge_name.data(), r.x,
                r.y, r.width, r.height, marks[i].pitch_x, marks[i].cell, marks[i].info.data());
  }
}
