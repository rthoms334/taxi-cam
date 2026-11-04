#include "../../src/shared/mark_identity.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
using namespace taxi_camera;
using namespace taxi_camera::mark_identity;
unsigned checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
panel_marks::Mark mark(const char* section, const char* gauge, std::uint8_t index) {
  panel_marks::Mark m;
  std::strncpy(m.name.data(), section, m.name.size() - 1);
  std::strncpy(m.gauge_name.data(), gauge, m.gauge_name.size() - 1);
  m.gauge = index;
  m.width = m.height = 100;
  return m;
}
}  // namespace

int main() {
  // Keys.
  require(std::strcmp(make_key("VCockpit02", "EFIS").data(), "VCockpit02/EFIS") == 0, "section/gauge key");
  require(std::strcmp(make_key("VCockpit01", "").data(), "VCockpit01") == 0, "section key");
  const auto long_key = make_key("VCockpit14", "a-very-long-gauge-name-from-an-html-page");
  require(std::strlen(long_key.data()) == KeySize - 1, "long key cut to fit");
  require(is_key("VCockpit02/EFIS") && is_key("VCockpit11") && !is_key("$EFIS_LEFT") && !is_key("DUS") && !is_key("VCockpitX"),
          "keys told from texture= names");

  // A350 (2026-11-04): two EFIS panels share the gauge name; the section tells them apart.
  Store store;
  const panel_marks::Mark left[] = {mark("VCockpit02", "EFIS", 0)}, right[] = {mark("VCockpit03", "EFIS", 0)},
                          ctr[] = {mark("VCockpit04", "CTR", 0)};
  require(store.read(259, left, 1) && store.read(258, right, 1) && store.read(257, ctr, 1), "marks stored");
  require(!store.read(259, left, 1), "same marks again are not new");
  require(store.resolve("VCockpit02/EFIS", nullptr, 0) == 259 && store.resolve("VCockpit03/EFIS", nullptr, 0) == 258 &&
              store.resolve("VCockpit04", nullptr, 0) == 257,
          "keys resolve to their textures");
  require(store.resolve("VCockpit02/CTR", nullptr, 0) == 0 && store.resolve("VCockpit05", nullptr, 0) == 0, "no false match");
  require(std::strcmp(store.find(257)->key().data(), "VCockpit04/CTR") == 0, "texture key is its first gauge");

  // PMDG 777: one texture, four DUs; any of them names it.
  const panel_marks::Mark dus[] = {mark("VCockpit01", "DU_LeftOutboard", 0), mark("VCockpit01", "DU_RightInboard", 2),
                                   mark("VCockpit01", "DU_LeftInboard", 1), mark("VCockpit01", "DU_RightOutboard", 3)};
  store.read(229, dus, 4);
  require(store.resolve("VCockpit01/DU_LeftInboard", nullptr, 0) == 229 && store.resolve("VCockpit01/DU_RightInboard", nullptr, 0) == 229,
          "777 DUs share their texture");
  require(std::strcmp(store.find(229)->key().data(), "VCockpit01/DU_LeftOutboard") == 0, "first gauge by index");

  // FlyByWire A380X: the simulator copies PFDL into the texture it shows,
  // and later into a replacement; routing follows to the end of the chain.
  Store a380;
  const panel_marks::Mark pfdl[] = {mark("VCockpit05", "pfd", 0)};
  a380.read(1522, pfdl, 1);
  std::vector<std::uint64_t> alive{1522, 1528, 1540};
  const auto live = [&](std::uint64_t id) { return std::find(alive.begin(), alive.end(), id) != alive.end(); };
  const std::pair<std::uint64_t, std::uint64_t> feeds[] = {{1528, 1540}, {1522, 1528}};
  require(a380.inherit(feeds, 2, live) == 2, "copies take the marks along the chain");
  require(a380.find(1540) && a380.find(1540)->copied, "copy marked as inherited");
  require(a380.resolve("VCockpit05/pfd", feeds, 2) == 1540, "end of the copy chain wins");
  alive = {1540};
  a380.retain(live);
  require(a380.textures().size() == 1 && a380.resolve("VCockpit05/pfd", feeds, 2) == 1540, "dead textures dropped");
  require(a380.inherit(feeds, 2, live) == 0, "no source, nothing inherited");

  // A recreated display with no copy (777, Fenix): the newer read wins.
  Store fenix;
  const panel_marks::Mark pfd[] = {mark("VCockpit01", "PFD_Captain", 2)};
  fenix.read(1113, pfd, 1);
  fenix.read(1220, pfd, 1);
  require(fenix.resolve("VCockpit01/PFD_Captain", nullptr, 0) == 1220, "newest texture wins");

  // A key cut short still matches the gauge it was made from.
  Store html;
  const panel_marks::Mark efb[] = {mark("VCockpit14", "a-very-long-gauge-name-from-an-html-page", 0)};
  html.read(5, efb, 1);
  require(html.resolve(long_key.data(), nullptr, 0) == 5, "cut key matches");
  require(html.resolve("VCockpit14/a-very-long", nullptr, 0) == 0, "a different prefix does not");

  std::printf("mark-identity: %u checks passed\n", checks);
  return 0;
}
