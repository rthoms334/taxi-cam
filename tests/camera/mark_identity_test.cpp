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
  require(key_of(a380, 1540).empty() && key_of(a380, 1600) == "@pfd",
          "a copy whose source is gone has no key, so it is never listed as its twin");

  // A recorded copy into a texture with marks of its own is another display:
  // routing stays. Of two copies, the newest is followed.
  Store copies;
  const panel_marks::Mark nd[] = {mark("VCockpit07", "nd", 0)};
  copies.read(1522, pfdl, 1);
  copies.read(1700, nd, 1);
  std::vector<std::uint64_t> shown{1522, 1528, 1530, 1700};
  const auto showing = [&](std::uint64_t id) { return std::find(shown.begin(), shown.end(), id) != shown.end(); };
  const std::pair<std::uint64_t, std::uint64_t> into_nd[] = {{1522, 1700}};
  require(copies.resolve("@pfd", into_nd, 1) == 1522, "no copy into a texture that carries other marks");
  const std::pair<std::uint64_t, std::uint64_t> two[] = {{1522, 1528}, {1522, 1530}};
  require(copies.inherit(two, 2, showing) == 2 && copies.resolve("@pfd", two, 2) == 1530, "the newest copy is followed");

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

  // Package-inactive verdict. The MSFS 2024 main menu (2026-11-05): one
  // display-shaped texture listed at a time, 10 pictures without a mark.
  require(!package_inactive(false, 10, 1), "the main menu's one texture is no verdict");
  require(!package_inactive(false, 400, MissingMarkTextures - 1), "however many pictures of too few textures");
  require(!package_inactive(false, 0, 0), "no pictures, no verdict");
  // A cockpit drawn without the package: many textures, none with a mark.
  require(package_inactive(false, MissingMarkPictures, MissingMarkTextures), "a cockpit's pictures without marks");
  require(!package_inactive(false, MissingMarkPictures - 1, 20), "too few pictures yet");
  require(!package_inactive(true, 50, 20), "one texture read means the package is loaded");

  // The iniBuilds A350 loading (2026-11-05): within 1.0 s of asking for the
  // marks, 8 pictures without one of 3 textures; the first mark 0.45 s later.
  PackageVerdict load;
  require(!load.observe(false, MissingMarkPictures, MissingMarkTextures, false), "pictures before the marks are painted");
  require(!load.observe(false, 23, 26, false) && !load.observe(true, 23, 26, true), "the marks read: no verdict");
  // Without the package the scan settles with nothing read.
  PackageVerdict missing;
  require(missing.observe(false, MissingMarkPictures, MissingMarkTextures, true), "a settled scan without marks");
  require(missing.observe(false, MissingMarkPictures + 1, MissingMarkTextures, false), "a texture created later keeps the verdict");
  require(!missing.observe(true, 30, 5, false), "a later read takes it back");
  missing.observe(false, 40, 5, true);
  missing.reset();
  require(!missing.inactive() && !missing.observe(false, 0, 0, false), "a new aircraft session starts without it");
  // The main menu settles too, but its one texture is still no verdict.
  PackageVerdict menu;
  require(!menu.observe(false, 10, 1, true), "the main menu's settled scan");

  // A read texture destroyed with the marks down: they go up at once, stay up
  // for its replacement, then may come down (Fenix A320, 2026-11-08).
  const auto hold = ReplacementWatch::HoldMs, gap = ReplacementWatch::GapMs;
  ReplacementWatch watch;
  require(!watch.pending(1000) && !watch.due(1000, false, 0), "nothing lost, nothing to show");
  watch.lost(100000, false);
  require(watch.pending(100000) && watch.due(100000, false, 50000), "a loss asks for the marks");
  watch.shown(100000);
  require(!watch.due(100001, true, 100000), "shown once");
  require(watch.pending(100000 + hold - 1), "held up for the replacement");
  require(!watch.pending(100000 + hold), "free to come down after the hold");
  watch.taken_down(100000 + hold);
  require(!watch.pending(100000 + hold + 1) && !watch.due(200000, false, 100000), "served: no further show");

  // Too soon after the last show: the loss waits for the gap, and stays
  // pending so "displays found" can't take the marks down meanwhile.
  ReplacementWatch soon;
  soon.lost(100000, false);
  require(!soon.due(100000, false, 95000) && soon.pending(100000 + hold), "waits out the gap");
  require(soon.due(95000 + gap, false, 95000), "shown once the gap has passed");
  require(!soon.due(95000 + gap, true, 95000), "not asked for again while they are already up");

  // A burst while the marks are up needs no second show; each loss extends the
  // hold.
  ReplacementWatch burst;
  burst.lost(10000, false);
  burst.shown(10000);
  burst.lost(13000, true);
  require(burst.pending(13000 + hold - 1) && !burst.due(40000, true, 10000), "a loss with the marks up extends the hold");
  require(!burst.pending(13000 + hold), "the hold ends after the last loss");
  burst.taken_down(13000 + hold);
  require(!burst.due(40000, false, 10000), "that burst was served by its one show");

  // The time limit inside the hold: the newest replacement still needs marks.
  ReplacementWatch limited;
  limited.lost(10000, false);
  limited.shown(10000);
  limited.lost(29000, true);
  limited.taken_down(30000);
  require(limited.pending(30000 + hold) && limited.due(30000, false, 10000), "cut off by the time limit: shown again");

  // A show for another reason (session start, setup opened) holds nothing up,
  // so the marks still come down as soon as the displays are found.
  ReplacementWatch plain;
  plain.shown(10000);
  require(!plain.pending(10000), "no loss, no hold");
  // But a loss waiting when such a show happens is served by it.
  ReplacementWatch served;
  served.lost(10000, false);
  served.shown(10002);
  require(!served.due(30000, false, 10002) && served.pending(10002 + hold - 1), "another show serves the loss, and holds for it");
  served.reset();
  require(!served.pending(10003), "a new session starts clean");

  // A replacement's show names its panel's page only: the marks of a panel
  // drawn on the screen must not come back with every replaced display
  // (FlyByWire A380X popup panel, issue 217).
  using panel_marks::kTargetedBase;
  using panel_marks::panel_bit;
  using panel_marks::targeted_show;
  ReplacementTargets targets;
  require(targets.mask() == 0 && targets.covered_by(0) && targets.covered_by(panel_bit(5)), "nothing lost: any show covers it");
  targets.lost(5);
  targets.lost(10);
  require(targets.mask() == (panel_bit(5) | panel_bit(10)), "the lost panels' pages are asked");
  require(targets.covered_by(0) && targets.covered_by(panel_bit(5) | panel_bit(10) | panel_bit(22)),
          "served by a show of every page, or of more panels");
  require(!targets.covered_by(panel_bit(5)), "marks up for another panel do not serve the loss");
  targets.lost(0);
  require(targets.mask() == 0 && !targets.covered_by(panel_bit(5) | panel_bit(10)) && targets.covered_by(0),
          "a texture with no panel number asks every page");
  targets.reset();
  targets.lost(49);
  require(targets.mask() == 0, "a panel beyond the mask asks every page");
  targets.reset();
  require(targets.mask() == 0 && targets.covered_by(panel_bit(1)), "reset");
  require(panel_bit(1) == 1 && panel_bit(48) == 1ull << 47 && !panel_bit(0) && !panel_bit(49), "panel bits");
  // The value: told apart from a show of every page (below 1e9 + 2), exact in
  // a double, and new each time for the same panels.
  require(targeted_show(panel_bit(5), 3) == kTargetedBase + 16 * 16 + 3, "the targeted value's layout");
  require(
      targeted_show(panel_bit(5), 3) != targeted_show(panel_bit(5), 4) && targeted_show(panel_bit(5), 16) == targeted_show(panel_bit(5), 0),
      "a new value for each show");
  const auto largest = targeted_show(~0ull, 15);
  require(largest < (1ull << 53) && static_cast<std::uint64_t>(static_cast<double>(largest)) == largest, "exact in the L:var's double");
  // A loss with marks up for other panels is owed its own show, at once.
  ReplacementWatch other;
  other.lost(10000, false);
  other.shown(10000);
  other.lost(11000, false);
  require(other.due(11000, false, 0), "not held back by the gap between shows");
  other.shown(11000);
  require(!other.due(11001, true, 0) && other.pending(11000 + hold - 1), "then served and held like any loss");

  std::printf("mark-identity: %u checks passed\n", checks);
  return 0;
}
