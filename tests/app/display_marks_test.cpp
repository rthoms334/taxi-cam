#include "../../src/bridge/display_marks.hpp"

#include <cstdio>
#include <stdexcept>

namespace {
namespace keep = taxi_camera::standalone::display_keep;
namespace profiles = taxi_camera::profiles;
unsigned checks{};
void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}
struct Marks {
  std::array<keep::Point, keep::MaxMarks> points{};
  unsigned count = 0;
  // 1 gauge mark, 0 area mark, -1 not listed.
  int at(unsigned x, unsigned y) const {
    for (unsigned i = 0; i < count; ++i)
      if (points[i].x == x && points[i].y == y)
        return points[i].gauge ? 1 : 0;
    return -1;
  }
};
Marks marks(const profiles::AircraftProfile* current, const profiles::AircraftProfile* base) {
  Marks out;
  out.count = keep::profile_marks(current, base, out.points);
  return out;
}
void built_in() {
  // The PMDG 777 left inboard DU: GlassOut marks its gauge at 1058,33.
  const auto& p = profiles::Pmdg777300ER;
  const auto plain = marks(&p, &p);
  require(plain.at(1058, 33) == 1, "The 777 left inboard gauge corner is a gauge mark");
  const auto content = profiles::display_content_rect(p, 0);
  require(plain.at(content.left, content.top) == 1, "Its camera area corner is a gauge mark too");
  for (unsigned i = 0; i < plain.count; ++i)
    require(plain.points[i].gauge, "Every mark of an unchanged built-in profile is a gauge mark");
  // The user moved the left inboard area to start at row 0.
  auto moved = p;
  moved.display_regions[0] = {1058, 0, 2016, 1004};
  const auto custom = marks(&moved, &p);
  require(custom.at(1058, 33) == 1, "The gauge corner stays a gauge mark after the area moves");
  require(custom.at(1058, 0) == 0, "The moved area's corner is an area mark");
  require(marks(nullptr, &p).count == 0, "No profile, no marks");
}
void set_up() {
  // A set-up aircraft: its gauge is unknown, so the area's corner stands in.
  auto p = profiles::Pmdg777300ER;
  p.fit_texture = true;
  p.areas[0] = {40, 25, 900, 700};
  const auto listed = marks(&p, nullptr);
  require(listed.at(40, 25) == 1, "A set-up aircraft's area corner is a gauge mark");
  require(listed.at(0, 0) == 1, "The texture's own corner is a gauge mark");
  require(listed.count <= keep::MaxMarks, "Within the list's size");
}
}  // namespace

int main() {
  try {
    built_in();
    set_up();
    std::printf("Display marks checks: %u passed\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Display marks test failed: %s\n", error.what());
    return 1;
  }
}
