#include "../../src/app/area_edit.hpp"
#include <cstdio>
#include <cstdlib>

namespace {
using namespace taxi_camera;
using namespace taxi_camera::standalone;
unsigned checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
bool is(const profiles::DisplayRect& r, unsigned l, unsigned t, unsigned rr, unsigned b) {
  return r.left == l && r.top == t && r.right == rr && r.bottom == b;
}
}  // namespace

int main() {
  // Limits: a set-up aircraft 16 px; a catalog profile 64 px or its padding.
  profiles::AircraftProfile fit{};
  fit.fit_texture = true;
  fit.camera_padding = {0, 0, 0, 0};
  require(area_edit::limits(fit).min_width == 16 && area_edit::limits(fit).min_height == 16, "set-up aircraft minimum");
  const auto a380 = area_edit::limits(profiles::A380);
  require(a380.min_width == 64 && a380.min_height == 64, "catalog minimum");

  // A 2048 texture shown 512 px wide: handles and hits.
  const RECT picture{100, 50, 612, 562};
  const profiles::DisplayRect area{1058, 33, 2016, 1004};  // The 777's left inboard DU.
  const auto points = area_edit::handles(picture, 2048, 2048, area);
  require(points[0].x == 100 + 265 && points[0].y == 50 + 8, "top-left handle (MulDiv rounds)");
  require(points[2].x == 100 + 504 && points[2].y == 50 + 251, "bottom-right handle");
  require(area_edit::hit(picture, 2048, 2048, area, points[5], 9, 6) == 5, "right edge handle hit");
  require(area_edit::hit(picture, 2048, 2048, area, {500, 200}, 9, 6) == 8, "inside moves the frame");
  require(area_edit::hit(picture, 2048, 2048, area, {120, 500}, 9, 6) == -1, "outside hits nothing");

  // Drags keep the area on the texture and above the minimum.
  const area_edit::Limits least{64, 64};
  require(is(area_edit::drag(area, 8, 100, 0, 2048, 2048, least), 1090, 33, 2048, 1004), "move stops at the right edge");
  require(is(area_edit::drag(area, 8, -2000, -100, 2048, 2048, least), 0, 0, 958, 971), "move stops at the top-left corner");
  require(is(area_edit::drag(area, 5, -2000, 0, 2048, 2048, least), 1058, 33, 1122, 1004), "resize stops at the minimum width");
  require(is(area_edit::drag(area, 0, -100, -100, 2048, 2048, least), 958, 0, 2016, 1004), "corner resize clamps to the texture");
  require(is(area_edit::drag(area, 6, 0, 2000, 2048, 2048, least), 1058, 33, 2016, 2048), "bottom edge stops at the texture");

  // A small gauge is grown to the minimum about its centre, on the texture.
  require(is(area_edit::fit({0, 0, 10, 10}, 768, 768, least), 0, 0, 64, 64), "small gauge in a corner grows inward");
  require(is(area_edit::fit({300, 300, 310, 310}, 768, 768, least), 273, 273, 337, 337), "small gauge grows about its centre");
  require(is(area_edit::fit({0, 1441, 683, 2050}, 2048, 2048, least), 0, 1439, 683, 2048), "gauge past the texture edge kept on it");
  require(area_edit::cursor(8) == MAKEINTRESOURCEW(32646) && area_edit::cursor(-1) == MAKEINTRESOURCEW(32512), "cursors");
  std::printf("area-edit: %u checks passed\n", checks);
  return 0;
}
