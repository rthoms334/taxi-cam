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
  bool has(unsigned x, unsigned y) const {
    for (unsigned i = 0; i < count; ++i)
      if (points[i].x == x && points[i].y == y)
        return true;
    return false;
  }
};
Marks marks(const profiles::AircraftProfile* current, const profiles::AircraftProfile* base) {
  Marks out;
  out.count = keep::profile_marks(current, base, out.points);
  return out;
}
void built_in() {
  // The PMDG 777 left inboard DU: its gauge corner is at 1058,33.
  const auto& p = profiles::Pmdg777300ER;
  const auto plain = marks(&p, &p);
  require(plain.has(1058, 33), "The 777 left inboard gauge corner is marked");
  const auto content = profiles::display_content_rect(p, 0);
  require(plain.has(content.left, content.top), "Its camera area corner is marked too");
  // The user moved the left inboard area to start at row 0.
  auto moved = p;
  moved.display_regions[0] = {1058, 0, 2016, 1004};
  const auto custom = marks(&moved, &p);
  require(custom.has(1058, 33), "The gauge corner stays marked after the area moves");
  require(custom.has(1058, 0), "The moved area's corner is marked");
  require(marks(nullptr, &p).count == 0, "No profile, no marks");
}
void set_up() {
  // A set-up aircraft: its gauge is unknown, so the area's corner stands in.
  auto p = profiles::Pmdg777300ER;
  p.fit_texture = true;
  p.areas[0] = {40, 25, 900, 700};
  const auto listed = marks(&p, nullptr);
  require(listed.has(40, 25), "A set-up aircraft's area corner is marked");
  require(listed.has(0, 0), "The texture's own corner is marked");
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
