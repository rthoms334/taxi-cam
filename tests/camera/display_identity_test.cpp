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
    require(
        resolve_names(a350, usual.data(), usual.size(), names, 3, fit, ids, out, pairs) == 0 && ids[0] == 978 && ids[1] == 977 && !ids[2],
        "Usual A350 burst names without a shift");
    const std::vector<Creation> extras{{975, 0, 2048, 2048, 5, 28}, {976, 1, 1644, 1024, 5, 28}, {977, 2, 1644, 1024, 5, 28},
                                       {978, 3, 1644, 1024, 5, 28}, {979, 4, 2048, 2048, 5, 28}, {980, 5, 2048, 2048, 5, 28},
                                       {981, 6, 2048, 2048, 5, 28}};
    require(resolve_names(a350, extras.data(), extras.size(), names, 3, fit, ids, out, pairs) == 2 && ids[0] == 978 && ids[1] == 977,
            "Two trailing extras shift the A350 names back by two");
    const auto never = [](const Creation&) { return false; };
    require(resolve_names(a350, extras.data(), extras.size(), names, 3, never, ids, out, pairs) == -1 && !ids[0] && !ids[1],
            "No display-shaped pairing leaves the names unresolved");
    const char* none[]{"", "", ""};
    require(resolve_names(a350, extras.data(), extras.size(), none, 3, fit, ids, out, pairs) == -1,
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
    require(resolve_names(panels, multimip.data(), multimip.size(), names, 3, fit, ids, out, pairs) == 0 && ids[0] == 233 &&
                ids[1] == 233 && ids[2] == 232,
            "777 names resolve without a shift");
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
    TaxiButtonRoutes single;
    require(single.adopt_named({233, 233, 232}, 3) && single.targets[2] == 232 && single.targets[0] == single.targets[1],
            "Single-display profiles name one texture for both sides and a separate lower");
    single.select_lower(240);
    require(!single.adopt_named({233, 233, 232}, 3) && single.targets[2] == 240, "An explicit lower choice is kept");
  }
  std::printf("PASS: %u display identity checks.\n", checks);
  return 0;
}
