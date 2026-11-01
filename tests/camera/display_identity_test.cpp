#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../../src/camera/display_panels.hpp"
#include "../../src/graphics/taxi_button_routes.hpp"

namespace {
using namespace taxi_camera::display_identity;
unsigned checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
void add(Panels& panels, std::uint32_t index, const char* section, const char* texture, std::uint32_t kind, std::uint32_t canvas) {
  auto& p = panels.panels[panels.count++];
  p.index = index;
  p.kind = kind;
  p.canvas = canvas != 0;
  p.canvas_width = p.canvas_height = canvas;
  std::strncpy(p.section.data(), section, p.section.size() - 1);
  std::strncpy(p.texture.data(), texture, p.texture.size() - 1);
}
}  // namespace

bool any(const char*, std::uint32_t&, std::uint32_t&) {
  return false;
}
int main() {
  // PMDG 777-300ER panel table as read live on 2026-10-04 (registry order is
  // not index order), and that session's multi-mip cockpit-load creations.
  Panels panels;
  add(panels, 1, "VCockpit02", "EICASCDU", 4, 1024);
  add(panels, 3, "VCockpit04", "VCISFDNC", 4, 512);
  add(panels, 2, "VCockpit03", "VCISFD", 4, 512);
  add(panels, 0, "VCockpit01", "DUS", 4, 1024);
  add(panels, 12, "VPainting01", "RegistrationNumber", 8, 0);
  add(panels, 8, "VCockpit09", "VCTABLETCA", 4, 1024);
  add(panels, 10, "VCockpit11", "EICASUP", 4, 1024);
  add(panels, 11, "VCockpit12", "NO_TEXTURE", 4, 256);
  add(panels, 9, "VCockpit10", "VCTABLETFO", 4, 1024);
  add(panels, 4, "VCockpit05", "VCSTBY", 4, 1024);
  add(panels, 7, "VCockpit08", "VCChrono", 4, 1024);
  add(panels, 5, "VCockpit06", "VCSTBYNC", 4, 1024);
  add(panels, 6, "VCockpit07", "VCRadio", 4, 1024);
  require(textured(panels.panels[0]) && !textured(panels.panels[4]) && !textured(panels.panels[7]),
          "Textured panels exclude VPainting and NO_TEXTURE");
  const std::vector<Creation> burst{{229, 1, 2048, 2048, 5, 28}, {0, 2, 368, 434, 1, 26},   {230, 3, 2048, 2048, 5, 28},
                                    {231, 4, 2048, 2048, 5, 28}, {0, 5, 1024, 512, 5, 28},  {0, 6, 1024, 1024, 5, 28},
                                    {0, 7, 1024, 1024, 5, 28},   {0, 8, 1024, 1024, 5, 28}, {0, 9, 512, 512, 5, 28},
                                    {0, 10, 512, 512, 5, 28},    {0, 11, 917, 943, 1, 27},  {232, 12, 2048, 2048, 5, 28},
                                    {233, 13, 2048, 2048, 5, 28}};
  std::array<Assignment, kMaxPanels> out{};
  bool complete = false;
  const auto n = propose(panels, burst.data(), burst.size(), out, complete);
  require(n == 11 && complete, "All eleven textured panels paired");
  const auto find = [&](const char* name) -> const Creation* {
    for (std::size_t i = 0; i < n; ++i)
      if (std::strcmp(out[i].panel->texture.data(), name) == 0)
        return &out[i].creation;
    return nullptr;
  };
  require(find("DUS") && find("DUS")->id == 233, "DUS is the last multi-mip creation");
  require(find("EICASCDU") && find("EICASCDU")->id == 232, "EICASCDU is second last");
  require(find("EICASUP") && find("EICASUP")->id == 229, "Highest textured index is created first");
  require(find("VCChrono") && find("VCChrono")->width == 1024 && find("VCChrono")->height == 512, "Chrono gets the 1024x512 texture");
  require(find("VCISFD") && find("VCISFD")->width == 512, "ISFD gets a 512 texture");
  require(!find("NO_TEXTURE") && !find("RegistrationNumber"), "Untextured panels are not paired");

  const auto short_n = propose(panels, burst.data(), 5, out, complete);
  require(short_n == 4 && !complete, "A short burst is reported incomplete");
  Panels empty;
  require(propose(empty, burst.data(), burst.size(), out, complete) == 0 && !complete, "No panels, no proposal");

  // iniBuilds A350, 2026-10-04: two extra 2048 x 2048 textures arrived
  // between $EFIS_LEFT's and $INI_FAP's. Names must land on the 1644 x 1024
  // display shape, which a window moved back by two creations gives.
  {
    Panels a350;
    add(a350, 0, "VCockpit01", "$INI_FAP", 4, 1024);
    add(a350, 2, "VCockpit02", "$EFIS_LEFT", 4, 1024);
    add(a350, 3, "VCockpit03", "$EFIS_RIGHT", 4, 1024);
    add(a350, 4, "VCockpit04", "$SD", 4, 1024);
    const auto fit = [](const Creation& c) { return c.width == 1644 && c.height == 1024; };
    const char* names[]{"$EFIS_LEFT", "$EFIS_RIGHT", ""};
    std::uint64_t ids[3]{};
    std::size_t pairs = 0;
    const std::vector<Creation> usual{
        {976, 1, 1644, 1024, 5, 28}, {977, 2, 1644, 1024, 5, 28}, {978, 3, 1644, 1024, 5, 28}, {981, 4, 2048, 2048, 5, 28}};
    require(resolve_names(a350, usual.data(), usual.size(), names, 3, fit, any, ids, out, pairs) == 0 && ids[0] == 978 && ids[1] == 977 &&
                !ids[2],
            "Usual A350 burst names without a shift");
    const std::vector<Creation> extras{{975, 0, 2048, 2048, 5, 28}, {976, 1, 1644, 1024, 5, 28}, {977, 2, 1644, 1024, 5, 28},
                                       {978, 3, 1644, 1024, 5, 28}, {979, 4, 2048, 2048, 5, 28}, {980, 5, 2048, 2048, 5, 28},
                                       {981, 6, 2048, 2048, 5, 28}};
    require(resolve_names(a350, extras.data(), extras.size(), names, 3, fit, any, ids, out, pairs) == 2 && ids[0] == 978 && ids[1] == 977,
            "Two trailing extras shift the A350 names back by two");
    const auto never = [](const Creation&) { return false; };
    require(resolve_names(a350, extras.data(), extras.size(), names, 3, never, any, ids, out, pairs) == -1 && !ids[0] && !ids[1],
            "No display-shaped pairing leaves the names unresolved");
    const char* none[]{"", "", ""};
    require(resolve_names(a350, extras.data(), extras.size(), none, 3, fit, any, ids, out, pairs) == -1,
            "A profile without names resolves nothing");
  }
  // 777 names resolve unshifted from the recorded burst (DUS 233, EICASCDU 232).
  {
    const char* names[]{"DUS", "DUS", "EICASCDU"};
    const auto fit = [](const Creation& c) { return c.width == 2048; };
    std::uint64_t ids[3]{};
    std::size_t pairs = 0;
    std::vector<Creation> multimip;  // The bridge passes multi-mip format 28 creations only.
    for (const auto& c : burst)
      if (c.mips > 1 && c.format == 28)
        multimip.push_back(c);
    require(resolve_names(panels, multimip.data(), multimip.size(), names, 3, fit, any, ids, out, pairs) == 0 && ids[0] == 233 &&
                ids[1] == 233 && ids[2] == 232,
            "777 names resolve without a shift");
  }

  // FlyByWire A380 after an aircraft switch, 2026-10-04: six 768 x 1024
  // textures (NDR, NDL, PFDR, PFDL, SD, EWD), MFD at 1646 x 1024, then an
  // extra 1646 x 1024 and four 768 x 1024 copies. The display shape alone
  // accepts a shift of 3 (EWD's texture as PFDL); the MFD/EWD shapes force 5.
  {
    Panels fbw;
    add(fbw, 0, "VCockpit01", "SCREEN_DU_MFD", 4, 1024);
    add(fbw, 2, "VCockpit03", "SCREEN_DU_EWD", 4, 1024);
    add(fbw, 3, "VCockpit04", "SCREEN_DU_SD", 4, 1024);
    add(fbw, 4, "VCockpit05", "SCREEN_DU_PFDL", 4, 1024);
    add(fbw, 5, "VCockpit06", "SCREEN_DU_PFDR", 4, 1024);
    add(fbw, 6, "VCockpit07", "SCREEN_DU_NDL", 4, 1024);
    add(fbw, 7, "VCockpit08", "SCREEN_DU_NDR", 4, 1024);
    add(fbw, 11, "VCockpit12", "Clock", 4, 256);
    const auto fit = [](const Creation& c) { return c.width == 768 && c.height == 1024; };
    const auto shapes = [](const char* panel, std::uint32_t& width, std::uint32_t& height) {
      width = std::strcmp(panel, "SCREEN_DU_MFD") == 0 ? 1646 : 768;
      height = 1024;
      return std::strcmp(panel, "SCREEN_DU_MFD") == 0 || std::strcmp(panel, "SCREEN_DU_EWD") == 0 ||
             std::strcmp(panel, "SCREEN_DU_SD") == 0;
    };
    const std::vector<Creation> load{{0, 0, 2560, 1280, 5, 28},   {1519, 1, 768, 1024, 5, 28},  {1520, 2, 768, 1024, 5, 28},
                                     {1521, 3, 768, 1024, 5, 28}, {1522, 4, 768, 1024, 5, 28},  {1523, 5, 768, 1024, 5, 28},
                                     {1524, 6, 768, 1024, 5, 28}, {0, 7, 1646, 1024, 5, 28},    {0, 8, 1646, 1024, 5, 28},
                                     {1527, 9, 768, 1024, 5, 28}, {1528, 10, 768, 1024, 5, 28}, {1529, 11, 768, 1024, 5, 28},
                                     {1530, 12, 768, 1024, 5, 28}};
    const char* names[]{"SCREEN_DU_PFDL", "SCREEN_DU_PFDR", ""};
    std::uint64_t ids[3]{};
    std::size_t pairs = 0;
    require(resolve_names(fbw, load.data(), load.size(), names, 3, fit, any, ids, out, pairs) == 3 && ids[0] == 1524,
            "Display shape alone takes EWD's texture as PFDL");
    require(resolve_names(fbw, load.data(), load.size(), names, 3, fit, shapes, ids, out, pairs) == 5 && ids[0] == 1522 && ids[1] == 1521,
            "MFD, EWD and SD shapes put PFDL and PFDR on their own textures");
    const std::vector<Creation> fresh(load.begin(), load.begin() + 8);
    require(resolve_names(fbw, fresh.data(), fresh.size(), names, 3, fit, shapes, ids, out, pairs) == 0 && ids[0] == 1522 && ids[1] == 1521,
            "A fresh FlyByWire load names without a shift");
  }
  {
    // Fenix A320 load of 2026-11-01 (bridge.log): the table lists one
    // VCockpit17 panel first and the other last, while the textures were
    // created in section order. The setup page's pictures showed #247-#252
    // are ND FO, PFD FO, ECAM lower, ECAM upper, ND and PFD captain.
    Panels fenix;
    const char* layout[][2]{
        {"VCockpit17", "Panel_Refueling"},    {"VCockpit01", "$A320_PFD_Captain"},  {"VCockpit02", "$A320_ND_Captain"},
        {"VCockpit03", "$A320_ECAM_Upper"},   {"VCockpit04", "$A320_ECAM_Lower"},   {"VCockpit05", "$A320_PFD_FO"},
        {"VCockpit06", "$A320_ND_FO"},        {"VCockpit07", "$A320_Standby"},      {"VCockpit08", "$A320_Readouts_Legacy"},
        {"VCockpit09", "$A320_Clock_Legacy"}, {"VCockpit10", "$A320_DCDU"},         {"VCockpit11", "$A320_MCDU_Captain"},
        {"VCockpit12", "$A320_MCDU_FO"},      {"VCockpit13", "$EFB_Captain"},       {"VCockpit14", "$EFB_FO"},
        {"VCockpit15", "NO_TEXTURE"},         {"VCockpit16", "DisplaysReflection"}, {"VCockpit17", "$FAP"}};
    for (std::uint32_t i = 0; i < 18; ++i)
      add(fenix, i, layout[i][0], layout[i][1], 4, 1024);
    const std::vector<Creation> load{{232, 0, 768, 768, 10, 28}, {233, 1, 384, 264, 9, 28},   {234, 2, 1024, 768, 5, 28},
                                     {235, 3, 1024, 512, 5, 28}, {236, 4, 2048, 1536, 5, 28}, {237, 5, 2048, 1536, 5, 28},
                                     {238, 6, 512, 432, 5, 28},  {239, 7, 512, 432, 5, 28},   {240, 8, 512, 680, 5, 28},
                                     {242, 9, 512, 512, 5, 28},  {246, 10, 256, 256, 5, 28},  {247, 11, 768, 768, 5, 28},
                                     {248, 12, 768, 768, 5, 28}, {249, 13, 768, 768, 5, 28},  {250, 14, 768, 768, 5, 28},
                                     {251, 15, 768, 768, 5, 28}, {252, 16, 768, 768, 5, 28}};
    const auto fit = [](const Creation& c) { return c.width == 768 && c.height == 768 && c.mips == 5; };
    // The saved setup's anchors: four displays and the standby instrument.
    const auto anchors = [](const char* panel, std::uint32_t& width, std::uint32_t& height) {
      const bool display = std::strcmp(panel, "$A320_ECAM_Upper") == 0 || std::strcmp(panel, "$A320_ND_Captain") == 0 ||
                           std::strcmp(panel, "$A320_ND_FO") == 0 || std::strcmp(panel, "$A320_ECAM_Lower") == 0;
      width = height = display ? 768 : 256;
      return display || std::strcmp(panel, "$A320_Standby") == 0;
    };
    const char* names[]{"$A320_PFD_Captain", "$A320_PFD_FO", ""};
    std::uint64_t ids[3]{};
    std::size_t pairs = 0;
    std::array<Assignment, kMaxPanels> out{};
    require(resolve_names(fenix, load.data(), load.size(), names, 3, fit, anchors, ids, out, pairs) == -1,
            "Index order fits no shift on a table listing a section's panels apart");
    require(resolve_names(fenix, load.data(), load.size(), names, 3, fit, anchors, ids, out, pairs, 8, PanelOrder::section) == 0 &&
                ids[0] == 252 && ids[1] == 248,
            "Section order names the captain and FO PFDs");
    bool complete = false;
    const auto n = propose(fenix, load.data(), load.size(), out, complete, PanelOrder::section);
    const auto named = [&](const char* texture) {
      for (std::size_t i = 0; i < n; ++i)
        if (std::strcmp(out[i].panel->texture.data(), texture) == 0)
          return out[i].creation.id;
      return std::uint64_t{};
    };
    require(complete && named("$A320_ND_Captain") == 251 && named("$A320_ECAM_Upper") == 250 && named("$A320_ECAM_Lower") == 249 &&
                named("$A320_ND_FO") == 247 && named("$A320_Standby") == 246,
            "Every display gets the picture it shows");
    // A table already in section order pairs the same either way.
    Panels ordered;
    for (std::uint32_t i = 1; i < 18; ++i)
      add(ordered, i - 1, layout[i][0], layout[i][1], 4, 1024);
    add(ordered, 17, "VCockpit17", "Panel_Refueling", 4, 1024);
    std::uint64_t by_index[3]{}, by_section[3]{};
    require(
        resolve_names(ordered, load.data(), load.size(), names, 3, fit, anchors, by_index, out, pairs) ==
                resolve_names(ordered, load.data(), load.size(), names, 3, fit, anchors, by_section, out, pairs, 8, PanelOrder::section) &&
            by_index[0] == by_section[0] && by_index[1] == by_section[1] && by_index[0] == 252,
        "A table in section order names the same in either order");
  }

  std::array<char, 8> name{};
  const unsigned char raw[] = {'E', 'I', 'C', 'A', 'S', 'C', 'D', 'U', 'X'};
  copy_name(raw, sizeof(raw), name);
  require(std::strcmp(name.data(), "EICASCD") == 0, "Names are truncated and terminated");
  const unsigned char stop[] = {'D', 'U', 'S', 0, 'Z'};
  copy_name(stop, sizeof(stop), name);
  require(std::strcmp(name.data(), "DUS") == 0, "Names stop at NUL");
  // Named routing replaces automatic guesses, never an explicit choice, and a
  // destroyed named texture leaves the side to detection again.
  {
    using taxi_camera::TaxiButtonRoutes;
    TaxiButtonRoutes routes;
    require(routes.adopt_detected({5, 6}), "Automatic pair adopted");
    require(routes.adopt_named({229, 228, 0}, 2) && routes.targets[0] == 229 && routes.targets[1] == 228 && routes.named(0),
            "Names replace an automatic pair");
    require(!routes.adopt_detected({5, 6}) && routes.targets[0] == 229, "Detection does not withdraw a named side");
    routes.forget(229);
    require(!routes.targets[0] && !routes.named(0) && routes.targets[1] == 228, "A destroyed named texture clears only its side");
    TaxiButtonRoutes chosen;
    require(chosen.select_explicit({7, 8}) && !chosen.adopt_named({229, 228, 0}, 2) && chosen.targets[0] == 7,
            "An explicit choice is never overridden");
    TaxiButtonRoutes moved;
    require(moved.select_explicit({228, 0}) && !moved.adopt_named({229, 228, 0}, 2) && moved.targets[0] == 228 && !moved.targets[1],
            "A texture chosen for the other side is not named onto this one");
    TaxiButtonRoutes swapped;
    require(swapped.adopt_detected({6, 5}) && swapped.adopt_named({5, 6, 0}, 2) && swapped.targets[0] == 5 && swapped.targets[1] == 6,
            "Names correct a swapped automatic pair");
    TaxiButtonRoutes a346;
    require(a346.select_single(500) && !a346.adopt_named({937, 937, 937}, 3) && a346.targets[0] == 500 && a346.targets[1] == 500 &&
                a346.targets[2] == 500,
            "A single-display choice keeps every side, side 2 included");
    TaxiButtonRoutes pmdg;
    require(
        pmdg.adopt_single(7, true) && pmdg.adopt_lower(8) && pmdg.adopt_named({8, 8, 7}, 3) && pmdg.targets[0] == 8 && pmdg.targets[2] == 7,
        "Names correct a swapped automatic 777 guess");
    TaxiButtonRoutes single;
    require(single.adopt_named({233, 233, 232}, 3) && single.targets[2] == 232 && single.targets[0] == single.targets[1],
            "Single-display profiles name one texture for both sides and a separate lower");
    single.select_lower(240);
    require(!single.adopt_named({233, 233, 232}, 3) && single.targets[2] == 240, "An explicit lower choice is kept");
  }
  require(
      section_name("VCockpit01") && section_name("VPainting02") && !section_name("Window01") && !section_name("") && !section_name("VCock"),
      "Only VCockpit and VPainting sections pass the layout check");
  {
    require(textured_count(panels) == 11, "The 777 table has eleven textured panels");
    ProcessTables tables;
    require(tables.observe(0x777), "The process's first cockpit table is clean");
    require(tables.observe(0x777), "The same aircraft again is clean");
    require(!tables.observe(0xa350), "A switch to another aircraft is not clean");
    require(tables.observe(0xa350), "Reloading that aircraft is clean again");
    require(!tables.observe(0x777), "Switching back is not clean");
  }
  {
    // The Fenix A320 rebuilt its table in one load with its VCockpit17 panels
    // moved from first to last: the same aircraft.
    Panels first, rebuilt, other;
    add(first, 0, "VCockpit17", "Panel_Refueling", 4, 512);
    add(first, 1, "VCockpit17", "$FAP", 4, 1024);
    add(first, 2, "VCockpit01", "$A320_PFD_Captain", 4, 1024);
    add(first, 3, "VCockpit15", "NO_TEXTURE", 4, 32);
    add(rebuilt, 0, "VCockpit01", "$A320_PFD_Captain", 4, 1024);
    add(rebuilt, 1, "VCockpit15", "NO_TEXTURE", 4, 32);
    add(rebuilt, 2, "VCockpit17", "Panel_Refueling", 4, 512);
    add(rebuilt, 3, "VCockpit17", "$FAP", 4, 1024);
    add(other, 0, "VCockpit01", "$A320_PFD_Captain", 4, 1024);
    add(other, 1, "VCockpit17", "$FAP", 4, 1024);
    require(aircraft_signature(first) == aircraft_signature(rebuilt), "A reordered table is the same aircraft");
    require(aircraft_signature(first) != aircraft_signature(other), "Other panels are another aircraft");
    ProcessTables tables;
    require(tables.observe(aircraft_signature(first)) && tables.observe(aircraft_signature(rebuilt)),
            "A rebuilt table of the same aircraft stays clean");
  }
  std::printf("PASS: %u display identity checks.\n", checks);
  return 0;
}
