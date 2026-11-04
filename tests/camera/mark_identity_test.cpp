#include "../../src/shared/mark_identity.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
std::string key_of(const Store& store, std::uint64_t id) {
  return store.key_of(*store.find(id)).data();
}
}  // namespace

int main() {
  // Keys.
  require(std::string(make_key("PFD_Captain").data()) == "@PFD_Captain", "gauge key");
  require(std::string(make_key("EFIS", 1, 2).data()) == "@EFIS#1/2", "twin key");
  const auto long_key = make_key("a-very-long-gauge-name-from-an-html-page", 2, 3);
  require(std::strlen(long_key.data()) == KeySize - 1 && std::string(long_key.data()).ends_with("#2/3"), "long key cut, rank kept");
  require(is_key("@EFIS#0/2") && !is_key("$EFIS_LEFT") && !is_key("DUS") && !is_key("VCockpit02/EFIS") && !is_key("@"),
          "keys told from texture= names and test-build keys");
  ParsedKey parsed;
  require(parse_key("@EFIS#1/2", parsed) && parsed.gauge == "EFIS" && parsed.rank == 1 && parsed.count == 2, "twin key parsed");
  require(parse_key("@Gauge#Tail", parsed) && parsed.gauge == "Gauge#Tail" && parsed.count == 1, "a # in a gauge name is kept");

  // Fenix A320, two loads: the panel numbers shift by one, the keys do not.
  for (const unsigned shift : {0u, 1u}) {
    Store store;
    char pfd[16], nd[16];
    std::snprintf(pfd, sizeof(pfd), "VCockpit%02u", 1 + shift);
    std::snprintf(nd, sizeof(nd), "VCockpit%02u", 5 + shift);
    const panel_marks::Mark captain[] = {mark(pfd, "PFD_Captain", 2)}, fo[] = {mark(nd, "PFD_FO", 0)};
    store.read(1113, captain, 1);
    store.read(1107, fo, 1);
    require(key_of(store, 1113) == "@PFD_Captain" && store.resolve("@PFD_Captain", nullptr, 0) == 1113 &&
                store.resolve("@PFD_FO", nullptr, 0) == 1107,
            "gauge keys survive renumbered panels");
  }

  // A350: two EFIS panels carry the same gauge; panel order tells them apart,
  // also when every panel number moved.
  for (const unsigned shift : {0u, 7u}) {
    Store store;
    char left[16], right[16], centre[16];
    std::snprintf(left, sizeof(left), "VCockpit%02u", 2 + shift);
    std::snprintf(right, sizeof(right), "VCockpit%02u", 3 + shift);
    std::snprintf(centre, sizeof(centre), "VCockpit%02u", 4 + shift);
    const panel_marks::Mark l[] = {mark(left, "EFIS", 0)}, r[] = {mark(right, "EFIS", 0)}, c[] = {mark(centre, "CTR", 0)};
    store.read(258, r, 1);  // Read in any order.
    store.read(259, l, 1);
    store.read(257, c, 1);
    require(store.resolve("@EFIS#0/2", nullptr, 0) == 259 && store.resolve("@EFIS#1/2", nullptr, 0) == 258 &&
                store.resolve("@CTR", nullptr, 0) == 257,
            "twins by panel order");
    require(key_of(store, 259) == "@EFIS#0/2" && key_of(store, 258) == "@EFIS#1/2" && key_of(store, 257) == "@CTR", "twin keys");
    require(store.resolve("@EFIS", nullptr, 0) == 0, "an unranked key never picks one of twins");
  }
  // Half a pair read: the twin key waits rather than taking the wrong one.
  Store half;
  const panel_marks::Mark right_only[] = {mark("VCockpit03", "EFIS", 0)};
  half.read(258, right_only, 1);
  require(half.resolve("@EFIS#0/2", nullptr, 0) == 0 && half.resolve("@EFIS#1/2", nullptr, 0) == 0, "half a pair resolves nothing");

  // PMDG 777: one texture, four DUs; any of them names it.
  Store dus;
  const panel_marks::Mark four[] = {mark("VCockpit01", "DU_LeftOutboard", 0), mark("VCockpit01", "DU_RightInboard", 2),
                                    mark("VCockpit01", "DU_LeftInboard", 1), mark("VCockpit01", "DU_RightOutboard", 3)};
  dus.read(229, four, 4);
  require(dus.resolve("@DU_LeftInboard", nullptr, 0) == 229 && dus.resolve("@DU_RightInboard", nullptr, 0) == 229,
          "777 DUs share their texture");
  require(key_of(dus, 229) == "@DU_LeftOutboard", "a texture is named by its first gauge");

  // FlyByWire A380X: the simulator copies PFDL into the texture it shows,
  // and later into a replacement; routing follows to the end of the chain.
  Store a380;
  const panel_marks::Mark pfdl[] = {mark("VCockpit05", "pfd", 0)}, pfdr[] = {mark("VCockpit06", "pfd", 0)};
  a380.read(1522, pfdl, 1);
  a380.read(1600, pfdr, 1);
  std::vector<std::uint64_t> alive{1522, 1528, 1540, 1600};
  const auto live = [&](std::uint64_t id) { return std::find(alive.begin(), alive.end(), id) != alive.end(); };
  const std::pair<std::uint64_t, std::uint64_t> feeds[] = {{1528, 1540}, {1522, 1528}};
  require(a380.inherit(feeds, 2, live) == 2 && a380.find(1540) && a380.find(1540)->copied, "copies take the marks along the chain");
  require(a380.resolve("@pfd#0/2", feeds, 2) == 1540 && a380.resolve("@pfd#1/2", feeds, 2) == 1600,
          "end of the copy chain; copies are no twin");
  require(key_of(a380, 1540) == "@pfd#0/2", "a copy has its source's key");
  alive = {1540, 1600};
  a380.retain(live);
  require(a380.resolve("@pfd#0/2", feeds, 2) == 0, "a copy alone has no owner to rank");

  // A recreated display (777, Fenix): the newer texture of the same panel wins
  // and the old one does not count as a twin.
  Store fenix;
  const panel_marks::Mark pfd[] = {mark("VCockpit02", "PFD_Captain", 2)};
  fenix.read(1113, pfd, 1);
  fenix.read(1220, pfd, 1);
  require(fenix.resolve("@PFD_Captain", nullptr, 0) == 1220 && key_of(fenix, 1220) == "@PFD_Captain", "newest texture of a panel wins");

  // A key cut short still names the gauge it was made from.
  Store html;
  const panel_marks::Mark efb[] = {mark("VCockpit14", "a-very-long-gauge-name-from-an-html-page", 0)};
  html.read(5, efb, 1);
  const auto cut = key_of(html, 5);
  require(cut.size() == KeySize - 1 && html.resolve(cut.c_str(), nullptr, 0) == 5, "a cut key names its gauge");
  require(std::string(make_key("PFD", 0, 1).data()) == "@PFD" && html.resolve("@a-very-long", nullptr, 0) == 0, "a prefix is no match");

  std::printf("mark-identity: %u checks passed\n", checks);
  return 0;
}
