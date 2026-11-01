#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../src/app/settings_store.hpp"
#include "../../src/camera/aircraft_identity.hpp"
#include "../../src/camera/display_panels.hpp"
#include "../../src/shared/discovered_setup.hpp"

using namespace taxi_camera;
using namespace taxi_camera::standalone;

namespace {
unsigned checks{};
void require(bool ok, const char* label) {
  ++checks;
  if (!ok)
    throw std::runtime_error(label);
}
// AircraftLoaded paths as the bridge logged them.
constexpr const char* Fenix320 = "SimObjects\\Airplanes\\FNX_32X\\presets\\fnx\\FNX_320_CFM_SL\\config\\aircraft.CFG";
constexpr const char* Fenix321 = "SimObjects\\Airplanes\\FNX_32X\\presets\\fnx\\FNX_321_IAE_WF_SC\\config\\aircraft.CFG";
constexpr const char* Pmdg777F = "SimObjects\\Airplanes\\PMDG 777F\\aircraft.cfg";
constexpr const char* Fbw380 = "SimObjects\\Airplanes\\FlyByWire_A380X\\presets\\flybywire\\FlyByWire_A380_842\\config\\aircraft.CFG";
constexpr const char* Fenix320Key = "SimObjects/Airplanes/FNX_32X/presets/fnx/FNX_320_CFM_SL";
constexpr const char* Fenix321Key = "SimObjects/Airplanes/FNX_32X/presets/fnx/FNX_321_IAE_WF_SC";

std::string key_of(const char* path) {
  profiles::VariantKey key{};
  return profiles::variant_key(path, key) ? std::string(key.data()) : std::string("<none>");
}
profiles::DiscoveredLayout fenix_layout(const char* path = Fenix320) {
  profiles::DiscoveredLayout l{};
  profiles::VariantKey key{};
  require(profiles::variant_key(path, key), "Fixture path has a variant key");
  std::memcpy(l.key, key.data(), sizeof(l.key));
  l.sides = 2;
  l.shapes[0] = l.shapes[1] = {768, 768, 5};
  std::strcpy(l.textures[0], "$PFD_CAPT");
  std::strcpy(l.textures[1], "$PFD_FO");
  std::strcpy(l.anchors[0].name, "$ND_CAPT");
  l.anchors[0].width = l.anchors[0].height = 768;
  std::strcpy(l.anchors[1].name, "$EFB");
  l.anchors[1].width = 1024;
  l.anchors[1].height = 768;
  return l;
}

void variant_keys() {
  require(key_of(Fenix320) == Fenix320Key, "A preset's key drops config/aircraft.cfg");
  require(key_of(Fenix321) == Fenix321Key, "Each preset of one airplane folder is its own variant");
  require(key_of(Pmdg777F) == "SimObjects/Airplanes/PMDG 777F", "A folder without presets is its own variant");
  require(profiles::variant_family(Fenix320Key) == "SimObjects/Airplanes/FNX_32X" &&
              profiles::variant_family(key_of(Fenix321)) == profiles::variant_family(Fenix320Key),
          "Presets of one airplane folder share a family");
  require(profiles::variant_name(Fenix320Key) == "FNX_320_CFM_SL", "The preset names the variant");
  require(key_of("C:\\Community\\fnx-aircraft\\SimObjects\\Airplanes\\FNX_32X\\presets\\fnx\\FNX_320_CFM_SL\\config\\aircraft.cfg") ==
              Fenix320Key,
          "A package prefix is not part of the key");
  require(profiles::same_variant("simobjects/airplanes/fnx_32x/presets/fnx/fnx_320_cfm_sl/config/AIRCRAFT.CFG", Fenix320Key),
          "Variants compare without case");
  require(!profiles::same_variant(Fenix321, Fenix320Key) && !profiles::same_variant(Fenix320, ""), "Another preset is another variant");
  require(key_of(Fenix320Key) == Fenix320Key, "A key is its own variant key");
  for (const char* refused :
       {"", "Airplanes\\FNX_32X\\aircraft.cfg", "SimObjects\\Airplanes\\aircraft.cfg", "SimObjects\\Airplanes\\..\\x\\aircraft.cfg",
        "SimObjects\\Airplanes\\bad:name\\aircraft.cfg", "SimObjects\\Airplanes\\trailing.\\aircraft.cfg"})
    require(key_of(refused) == "<none>", "Paths without a usable SimObject folder have no key");
  const std::string long_component = "SimObjects\\Airplanes\\" + std::string(profiles::MaxVariantComponent + 1, 'x') + "\\aircraft.cfg";
  require(key_of(long_component.c_str()) == "<none>", "Over-long folder names have no key");
}

void layouts() {
  require(profiles::valid_layout(fenix_layout()), "A complete layout is valid");
  const auto refused = [](auto change, const char* label) {
    auto l = fenix_layout();
    change(l);
    require(!profiles::valid_layout(l), label);
  };
  refused([](auto& l) { l.shapes[1].mips = 1; }, "Panel textures have mips");
  refused([](auto& l) { l.shapes[0].width = 0; }, "Every side has its panel's shape");
  refused([](auto& l) { l.shapes[2] = {768, 768, 5}; }, "Unused sides have no shape");
  refused([](auto& l) { l.sides = 1; }, "At least two sides");
  auto mixed = fenix_layout();
  mixed.shapes[1] = {1024, 768, 5};
  require(profiles::valid_layout(mixed), "Sides can be panels of different sizes");
  refused([](auto& l) { l.padding = {400, 0, 400, 0}; }, "Padding leaves a camera area");
  refused([](auto& l) { std::strcpy(l.anchors[0].name, "$PFD_FO"); }, "A display is not its own anchor");
  refused([](auto& l) { l.anchors[0] = {}; }, "Anchors have no gaps");
  refused([](auto& l) { l.key[std::strlen(l.key) + 1] = 'x'; }, "Unused key bytes stay zero");
  refused([](auto& l) { std::strcpy(l.textures[2], "$ECAM"); }, "Unused sides stay empty");
  refused([](auto& l) { std::strcpy(l.key, "SimObjects/Airplanes/FNX_32X/config"); }, "A key is already a variant key");
  refused([](auto& l) { l.textures[0][0] = 0; }, "Every side has a panel name");
}

void publish_and_detect() {
  require(!profiles::find(profiles::DiscoveredProfileId) && !profiles::detect_aircraft("Airbus", Fenix320),
          "Nothing is set up before a layout is published");
  const auto fenix = fenix_layout();
  const auto* first = profiles::publish_discovered(fenix);
  require(first && profiles::find(profiles::DiscoveredProfileId) == first && profiles::discovered_current(fenix),
          "find() returns the published layout's profile");
  require(first->id == profiles::DiscoveredProfileId && std::wstring(first->name) == L"FNX_320_CFM_SL" &&
              first->pfd_detection == profiles::PfdDetectionPolicy::named && !profiles::commandable_buttons(*first) &&
              profiles::side_mask(*first) == 3 && !profiles::separate_lower_texture(*first) && first->mounts == profiles::DiscoveredMounts,
          "A set-up aircraft routes by name and uses shortcuts only");
  require(std::strcmp(first->panel_textures[1], "$PFD_FO") == 0 && std::strcmp(first->panel_shapes[1].name, "$EFB") == 0 &&
              first->panel_shapes[1].width == 1024 && !first->panel_shapes[2].name,
          "Names and anchors come from the layout");
  require(first->fit_texture && profiles::matches_display(*first, 768, 768, 5, 28) && profiles::matches_display(*first, 1024, 768, 5, 29) &&
              profiles::matches_display(*first, 2048, 1536, 10, 28) && !profiles::matches_display(*first, 768, 768, 1, 28) &&
              !profiles::matches_display(*first, 768, 768, 5, 87),
          "Any multi-mip RGBA8 panel texture can be a set-up aircraft's display");
  require(profiles::named_display(*first, 768, 768, 5, 28) && !profiles::named_display(*first, 1024, 768, 5, 28),
          "Name routing expects the panels the user picked");
  const auto area = profiles::side_rect(*first, 1, 1024, 512);
  const auto content = profiles::side_content_rect(*first, 1, 1024, 512);
  require(area.left == 0 && area.top == 0 && area.right == 1024 && area.bottom == 512, "Each side's area is the texture routed to it");
  require(content.left == 256 && content.top == 0 && content.right == 768 && content.bottom == 512,
          "Its cameras keep side 0's square shape inside it");
  const auto catalog = profiles::side_rect(profiles::A380, 0, 1024, 512);
  require(catalog.right == 768 && catalog.bottom == 763, "Catalog profiles keep their display regions");
  native_camera::AircraftIdentitySample set_up{};
  set_up.fresh = true;
  set_up.detected_profile = profiles::DiscoveredProfileId;
  std::strcpy(set_up.path.data(), Fenix320);
  require(native_camera::next_panel_discovery(false, set_up), "A set-up aircraft keeps every panel texture tracked");
  require(profiles::detect_aircraft("ATCCOM.ATC_NAME AIRBUS.0.text", Fenix320) == profiles::DiscoveredProfileId,
          "The set-up variant is detected");
  require(!profiles::detect_aircraft("ATCCOM.ATC_NAME AIRBUS.0.text", Fenix321), "Another preset is not");
  require(profiles::detect_aircraft("ATCCOM.ATC_NAME AIRBUS.0.text", Fbw380) == profiles::A380.id, "Catalog aircraft come first");
  require(profiles::publish_discovered(fenix) == first, "An unchanged layout keeps its profile");
  const auto generation = profiles::discovered_generation();
  const auto* second = profiles::publish_discovered(fenix_layout(Fenix321));
  require(second && second != first && profiles::discovered_generation() == generation + 1 && !profiles::discovered_current(fenix),
          "A new layout is a new profile");
  require(std::wstring(first->name) == L"FNX_320_CFM_SL" && std::strcmp(first->key.data(), Fenix320Key) == 0,
          "An earlier profile stays intact for its readers");
  require(profiles::detect_aircraft("Airbus", Fenix321) == profiles::DiscoveredProfileId && !profiles::detect_aircraft("Airbus", Fenix320),
          "Detection follows the newest layout");
  auto invalid = fenix;
  invalid.shapes[0].mips = 0;
  require(!profiles::publish_discovered(invalid) && profiles::find(profiles::DiscoveredProfileId) == second,
          "An invalid layout publishes nothing");
}

void settings_validation() {
  Settings s;
  s.profile = profiles::DiscoveredProfileId;
  s.discovered = fenix_layout();
  s.mounts = profiles::DiscoveredMounts;
  s.follow_taxi = 0;
  require(valid_settings(s), "Settings carry a set-up aircraft");
  s.panel_setup = 1;
  require(valid_settings(s), "The setup can ask for every panel texture");
  s.panel_setup = 2;
  require(!valid_settings(s), "The setup flag is 0 or 1");
  s.panel_setup = 0;
  s.manual_mask = 4;
  require(!valid_settings(s), "Sides follow the layout");
  s.manual_mask = 3;
  s.left_id = 1;
  s.lower_id = 2;
  require(!valid_settings(s), "No separate lower texture");
  s.lower_id = 0;
  s.discovered = {};
  require(!valid_settings(s), "The set-up aircraft needs its layout");
  Settings catalog;
  catalog.discovered.sides = 7;
  require(valid_settings(catalog), "Catalog profiles ignore the layout");
}

void settings_files(const std::wstring& root) {
  settings_override = root;
  Settings s;
  s.profile = profiles::DiscoveredProfileId;
  s.discovered = fenix_layout();
  s.mounts = profiles::DiscoveredMounts;
  s.mounts[0][2] = 12.5;
  s.follow_taxi = 0;
  // The right tail bracket is its own (not the left one mirrored).
  s.tail_upper_right = {0.25f, 0.5f};
  s.tail_corner_right = {0.125f, 0.75f};
  s.tail_inner_right = {0.375f, 0.875f};
  require(save_settings(s), "A set-up aircraft saves");
  const auto variant = root + L"\\profiles\\discovered\\Airplanes\\FNX_32X\\presets\\fnx\\FNX_320_CFM_SL\\variant.ini";
  const auto family = root + L"\\profiles\\discovered\\Airplanes\\FNX_32X\\layout.ini";
  require(settings_path(s) == variant && std::filesystem::exists(variant) && std::filesystem::exists(family),
          "Saved per variant, with the airplane folder's latest layout");
  wchar_t text[256]{};
  GetPrivateProfileStringW(L"aircraft", L"variant", L"", text, 256, (root + L"\\settings.ini").c_str());
  require(std::wstring(text) == L"SimObjects/Airplanes/FNX_32X/presets/fnx/FNX_320_CFM_SL" &&
              GetPrivateProfileIntW(L"aircraft", L"profile", 0, (root + L"\\settings.ini").c_str()) == profiles::DiscoveredProfileId,
          "settings.ini remembers the variant");
  Settings loaded;
  require(load_settings(loaded, L"missing") && loaded.profile == profiles::DiscoveredProfileId &&
              profiles::same_layout(loaded.discovered, s.discovered) && loaded.mounts == s.mounts && valid_settings(loaded),
          "The saved selection loads the variant's layout and calibration");
  require(loaded.tail_upper_right == s.tail_upper_right && loaded.tail_corner_right == s.tail_corner_right &&
              loaded.tail_inner_right == s.tail_inner_right && loaded.tail_upper != loaded.tail_upper_right,
          "The right tail bracket is saved on its own");
  auto defaults = loaded;
  reset_guide_settings(defaults, profiles::A359);
  require(defaults.tail_upper_right == profiles::A359.composition.tail_upper &&
              defaults.tail_inner_right == profiles::A359.composition.tail_inner,
          "Default guides mirror the left bracket");
  auto out_of_range = loaded;
  out_of_range.tail_corner_right = {0.75f, 0.5f};
  require(!valid_settings(out_of_range), "The right bracket stays within its half");
  Settings sibling;
  require(load_settings(sibling, L"missing", profiles::DiscoveredProfileId, Fenix321Key), "A new preset starts from its family");
  auto expected = fenix_layout(Fenix321);
  require(profiles::same_layout(sibling.discovered, expected) && sibling.mounts == profiles::DiscoveredMounts,
          "It takes the family's displays and the starting camera mounts");
  // Setup: a new layout replaces the saved one; the variant keeps its saved
  // calibration, and a variant never saved starts from the starting mounts.
  auto changed = fenix_layout();
  std::strcpy(changed.textures[0], "$ND_CAPT");
  std::memset(changed.anchors[0].name, 0, sizeof(changed.anchors[0].name));
  std::strcpy(changed.anchors[0].name, "$PFD_CAPT");
  Settings again;
  require(load_settings(again, L"missing", profiles::DiscoveredProfileId, {}, &changed) &&
              profiles::same_layout(again.discovered, changed) && again.mounts == s.mounts,
          "Setting a variant up again keeps its camera mounts");
  const auto new_variant = fenix_layout("SimObjects\\Airplanes\\Other_A320\\aircraft.cfg");
  Settings fresh;
  require(load_settings(fresh, L"missing", profiles::DiscoveredProfileId, {}, &new_variant) &&
              profiles::same_layout(fresh.discovered, new_variant) && fresh.mounts == profiles::DiscoveredMounts && fresh.follow_taxi == 0,
          "A new variant starts from the starting mounts, shortcuts only");
  auto broken = changed;
  broken.shapes[0].height = 0;
  Settings refused;
  require(!load_settings(refused, L"missing", profiles::DiscoveredProfileId, {}, &broken), "An invalid new layout is refused");
  Settings missing;
  require(!load_settings(missing, L"missing", profiles::DiscoveredProfileId, "SimObjects/Airplanes/Unknown") &&
              !load_settings(missing, L"missing", profiles::DiscoveredProfileId, "SimObjects/Airplanes/../x") &&
              !load_settings(missing, L"missing", profiles::DiscoveredProfileId, {}),
          "An explicit set-up aircraft needs a saved layout");
  require(WritePrivateProfileStringW(L"aircraft", L"variant", L"SimObjects/Airplanes/Unknown", (root + L"\\settings.ini").c_str()),
          "Point the saved selection at a variant without a layout");
  WritePrivateProfileStringW(nullptr, nullptr, nullptr, (root + L"\\settings.ini").c_str());
  Settings fallback;
  require(load_settings(fallback, L"missing") && fallback.profile == profiles::A380.id,
          "A saved selection without its layout falls back to the default profile");
  // Copying a setup to another preset carries its cameras and guides.
  Settings copied;
  require(copy_discovered_variant(Fenix320Key, "SimObjects/Airplanes/FNX_32X/presets/fnx/FNX_320_IAE_WF", L"missing", copied) &&
              copied.mounts == s.mounts && copied.tail_corner_right == s.tail_corner_right &&
              std::strcmp(copied.discovered.key, "SimObjects/Airplanes/FNX_32X/presets/fnx/FNX_320_IAE_WF") == 0,
          "A copy starts from the source's cameras, guides and displays");
  Settings reloaded;
  require(load_settings(reloaded, L"missing", profiles::DiscoveredProfileId, "SimObjects/Airplanes/FNX_32X/presets/fnx/FNX_320_IAE_WF") &&
              reloaded.mounts == s.mounts,
          "The copy is saved as its own variant");
  require(remove_discovered_variant("SimObjects/Airplanes/FNX_32X/presets/fnx/FNX_320_IAE_WF"), "Remove the copy again");
  require(!copy_discovered_variant("SimObjects/Airplanes/Unknown", Fenix321Key, L"missing", copied) &&
              !copy_discovered_variant(Fenix320Key, "Airplanes/x", L"missing", copied),
          "Copies need a saved source and a variant key");
  // The set-up aircraft list, and removal.
  require(save_settings(sibling), "A second preset saves");
  const auto saved = saved_discovered_variants();
  require(saved.size() == 2 && saved[0] == Fenix320Key && saved[1] == Fenix321Key, "Every saved variant is listed");
  require(remove_discovered_variant(Fenix320Key) && saved_discovered_variants() == std::vector<std::string>{Fenix321Key} &&
              !std::filesystem::exists(variant) && std::filesystem::exists(family),
          "Removing a preset keeps its airplane folder's layout for the others");
  require(remove_discovered_variant(Fenix321Key) && saved_discovered_variants().empty() && !std::filesystem::exists(family) &&
              !std::filesystem::exists(root + L"\\profiles\\discovered\\Airplanes\\FNX_32X"),
          "Removing the last preset removes the layout and its folders");
  require(!remove_discovered_variant(Fenix321Key) && !remove_discovered_variant("SimObjects/Airplanes/.."),
          "Nothing to remove, or not a variant key");
  settings_override.clear();
}

void selection() {
  native_camera::AutoProfileSelection selection;
  require(!selection.observe(profiles::DiscoveredProfileId, 1000, 11), "First sample of a variant");
  require(!selection.observe(profiles::DiscoveredProfileId, 2000, 22), "Another variant starts again");
  require(selection.observe(profiles::DiscoveredProfileId, 3000, 22) == profiles::DiscoveredProfileId, "Two samples agree");
}

void learning() {
  Status status{};
  std::strcpy(status.aircraft_path, Fenix320);
  status.panel_clean = 1;
  status.panel_state = static_cast<std::uint32_t>(PanelMapState::paired);
  const struct {
    const char* name;
    std::uint32_t width, height;
  } map[]{{"$EFB", 1024, 768},     {"$ND_FO", 768, 768},   {"$PFD_FO", 768, 768}, {"$ECAM_UPPER", 768, 768},
          {"$PFD_CAPT", 768, 768}, {"$ND_CAPT", 768, 768}, {"$MCDU", 512, 432},   {"$MCDU", 512, 432}};
  for (const auto& panel : map) {
    auto& entry = status.panels[status.panel_count];
    std::strcpy(entry.name, panel.name);
    entry.width = panel.width;
    entry.height = panel.height;
    entry.mips = 5;
    entry.index = 40 - status.panel_count++;
  }
  profiles::DiscoveredLayout l{};
  require(learn_layout(status, {"$PFD_CAPT", "$PFD_FO", nullptr}, 2, l) == LayoutLearning::learned && profiles::valid_layout(l),
          "A clean map gives a layout");
  require(std::strcmp(l.key, Fenix320Key) == 0 && l.shapes[0].width == 768 && l.shapes[0].mips == 5 && l.shapes[1].height == 768,
          "Key and each side's panel shape");
  require(std::strcmp(l.anchors[0].name, "$ECAM_UPPER") == 0 && std::strcmp(l.anchors[1].name, "$ND_CAPT") == 0 &&
              std::strcmp(l.anchors[2].name, "$EFB") == 0 && std::strcmp(l.anchors[3].name, "$ND_FO") == 0 && !l.anchors[4].name[0],
          "Anchors are the unique panels within two creations of each display");
  profiles::DiscoveredLayout three{};
  require(learn_layout(status, {"$PFD_CAPT", "$PFD_FO", "$ND_FO"}, 3, three) == LayoutLearning::learned && three.sides == 3 &&
              std::strcmp(three.textures[2], "$ND_FO") == 0 && profiles::valid_layout(three),
          "A lower display is a third side");
  for (const auto& anchor : three.anchors)
    require(std::strcmp(anchor.name, "$ND_FO") != 0, "A display is never an anchor");
  require(learn_layout(status, {"$PFD_CAPT", "$EFB", nullptr}, 2, l) == LayoutLearning::learned && l.shapes[1].width == 1024 &&
              l.shapes[1].height == 768 && profiles::valid_layout(l),
          "Sides can be panels of different sizes");
  require(learn_layout(status, {"$PFD_CAPT", "$NONE", nullptr}, 2, l) == LayoutLearning::unknown_panel, "Names come from the map");
  status.panel_clean = 0;
  require(learn_layout(status, {"$PFD_CAPT", "$PFD_FO", nullptr}, 2, l) == LayoutLearning::not_clean,
          "Nothing is learned after a switch to another aircraft");
  require(learn_layout(status, {"$PFD_CAPT", "$PFD_FO", nullptr}, 2, l, false) == LayoutLearning::learned,
          "A cockpit test, never saved, uses the map anyway");
  status.panel_clean = 1;
  status.panel_state = static_cast<std::uint32_t>(PanelMapState::incomplete);
  require(learn_layout(status, {"$PFD_CAPT", "$PFD_FO", nullptr}, 2, l) == LayoutLearning::not_paired, "Only a complete map");
  status.panel_state = static_cast<std::uint32_t>(PanelMapState::paired);
  std::strcpy(status.aircraft_path, "Airbus");
  require(learn_layout(status, {"$PFD_CAPT", "$PFD_FO", nullptr}, 2, l) == LayoutLearning::no_variant, "Only a known variant");
}

// The user's display choices for a built-in profile: another panel or a
// display area per side, as a copy of the profile that find() returns.
void display_choices(const std::wstring& root) {
  profiles::DisplayOverride none{};
  require(profiles::valid_override(2, false, profiles::A380.camera_padding, none), "No choices is valid");
  profiles::DisplayOverride choices{};
  std::strcpy(choices.textures[0], "SCREEN_DU_NDL");
  choices.areas[1] = {0, 0, 768, 900};
  require(profiles::valid_override(2, false, profiles::A380.camera_padding, choices), "A panel and an area are valid");
  auto refused = choices;
  refused.areas[2] = {0, 0, 768, 900};
  require(!profiles::valid_override(2, false, profiles::A380.camera_padding, refused), "No choices for sides the profile lacks");
  refused = choices;
  refused.areas[1] = {0, 0, 40, 900};
  require(!profiles::valid_override(2, false, profiles::A380.camera_padding, refused), "A built-in display area keeps a usable size");
  refused.areas[1] = {100, 0, 50, 900};
  require(!profiles::valid_override(2, true, profiles::A380.camera_padding, refused), "An area is a real rectangle");
  require(profiles::find(profiles::A380.id) == &profiles::A380, "Built-in until choices are published");
  const auto* chosen = profiles::publish_override(&profiles::A380, choices);
  require(chosen && profiles::find(profiles::A380.id) == chosen && profiles::base_profile(profiles::A380.id) == &profiles::A380 &&
              profiles::override_current(&profiles::A380, choices),
          "find() returns the profile with the user's choices");
  require(std::strcmp(chosen->panel_textures[0], "SCREEN_DU_NDL") == 0 && std::strcmp(chosen->panel_textures[1], "SCREEN_DU_PFDR") == 0 &&
              chosen->display_regions[1].bottom == 900 && chosen->display_regions[0].bottom == profiles::A380.display_regions[0].bottom,
          "Only the chosen sides change");
  require(profiles::publish_override(&profiles::A380, choices) == chosen, "The same choices keep their profile");
  require(profiles::find(profiles::A359.id) == &profiles::A359, "Other profiles stay built-in");
  // A set-up aircraft's area crops whichever texture its side uses.
  auto fit = profiles::A380;
  fit.fit_texture = true;
  fit.areas[0] = {100, 50, 600, 700};
  const auto crop = profiles::side_rect(fit, 0, 768, 768), clamped = profiles::side_rect(fit, 0, 400, 400),
             whole = profiles::side_rect(fit, 1, 768, 768);
  require(crop.left == 100 && crop.bottom == 700 && clamped.right == 400 && clamped.bottom == 400 && whole.right == 768,
          "An area crops the texture, kept on it, else the whole texture");
  // Saved with the profile.
  settings_override = root;
  Settings s;
  s.display_override = choices;
  require(save_settings(s), "Display choices save with the profile");
  Settings loaded;
  require(load_settings(loaded, L"missing", profiles::A380.id) && std::memcmp(&loaded.display_override, &choices, sizeof(choices)) == 0,
          "Display choices load with the profile");
  settings_override.clear();
  profiles::publish_override(&profiles::A380, none);
}
// The cameras render at the shape of side 0's camera area (shape_cameras).
void camera_shapes() {
  const auto same = [](const profiles::DisplayRect& a, profiles::DisplayRect b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
  };
  // Another panel alone keeps the built-in cameras exactly.
  profiles::DisplayOverride panel{};
  std::strcpy(panel.textures[0], "$EFIS_RIGHT");
  const auto* kept = profiles::publish_override(&profiles::A359, panel);
  require(kept && kept->camera_panes == profiles::A359.camera_panes && kept->composition.ui_aspect == 1.f,
          "A display area of the profile's size keeps its cameras");
  require(same(profiles::side_content_rect(*kept, 1, 1644, 1024), {854, 12, 1628, 763}), "Both displays keep their camera areas");
  // A350 left area twice as wide: 1607 x 751 cameras area.
  profiles::DisplayOverride wide{};
  wide.areas[0] = {0, 0, 1639, 763};
  auto next = profiles::A359;
  profiles::apply_choices(next, profiles::A359, wide);
  const auto* shaped = profiles::publish_override(&profiles::A359, wide);
  require(shaped && shaped->camera_panes == next.camera_panes, "The bridge's preview of the choices is the published profile");
  require(shaped->camera_panes[0] == std::array<std::int32_t, 2>{1607, 251} &&
              shaped->camera_panes[1] == std::array<std::int32_t, 2>{1607, 496},
          "Cameras widen with the area, keeping their height");
  require(shaped->composition.ui_aspect > 2.07f && shaped->composition.ui_aspect < 2.08f, "Overlays are drawn narrower by the stretch");
  require(same(profiles::side_content_rect(*shaped, 0, 1644, 1024), {16, 12, 1623, 763}), "The reshaped side fills its area");
  require(same(profiles::side_content_rect(*shaped, 1, 1644, 1024), {854, 206, 1628, 568}),
          "The other display shows the wide picture at its shape, black above and below");
  require(profiles::camera_candidate(1607, 251) && profiles::camera_candidate(1607, 496), "Cameras at the new shape are admitted");
  profiles::publish_override(&profiles::A359, {});
  // A set-up aircraft's cameras take its panel's shape.
  profiles::DiscoveredProfile fenix{};
  fenix.layout = fenix_layout();
  profiles::build_discovered(fenix);
  const auto& p = fenix.profile;
  require(p.camera_panes[0] == std::array<std::int32_t, 2>{768, 257} && p.camera_panes[1] == std::array<std::int32_t, 2>{768, 507},
          "A square 768 panel renders 768-wide cameras");
  require(p.composition.ui_aspect > 1.02f && p.composition.ui_aspect < 1.021f, "Overlays keep their proportions on the panel");
  require(same(profiles::side_content_rect(p, 1, 768, 768), {0, 0, 768, 768}), "A side with the same panel shape fills it");
  // A panel larger than a camera view: one scale keeps the shape.
  profiles::DiscoveredProfile wall{};
  wall.layout = fenix_layout();
  wall.layout.shapes[0] = wall.layout.shapes[1] = {4096, 1024, 5};
  profiles::build_discovered(wall);
  const auto& w = wall.profile.camera_panes;
  require(w[0][0] == profiles::MaxCameraPane && w[1][0] == profiles::MaxCameraPane && w[0][1] >= profiles::MinCameraPane &&
              w[1][1] < profiles::MaxCameraPane,
          "No camera edge passes the engine's limit");
  const double nose = double(w[0][0]) / w[0][1], intended = 4096.0 / (251.0 * 1024 / 751);
  require(nose > intended * 0.99 && nose < intended * 1.01, "A limited camera keeps its shape");
  // Another panel shape on side 1 shows side 0's picture at its shape.
  profiles::DiscoveredProfile mixed{};
  mixed.layout = fenix_layout();
  mixed.layout.shapes[1] = {1024, 768, 5};
  profiles::build_discovered(mixed);
  require(same(profiles::side_content_rect(mixed.profile, 1, 1024, 768), {128, 0, 896, 768}),
          "A wider panel shows the square picture with black either side");
}
void trimming() {
  std::vector<display_identity::Creation> burst;
  for (const std::uint32_t mips : {5u, 5u, 10u, 5u, 11u, 12u})
    burst.push_back({burst.size() + 1, 0, 768, 768, mips, 28});
  display_identity::trim_trailing_ui(burst);
  require(burst.size() == 4 && burst.back().mips == 5 && burst[2].mips == 10, "Only trailing UI textures are dropped");
}
}  // namespace

int main() {
  try {
    wchar_t repository[32768]{};
    const auto length = GetCurrentDirectoryW(32768, repository);
    require(length && length < 32768, "Locate working directory for ignored fixture files");
    const auto root = std::wstring(repository) + L"\\build\\discovered-aircraft-test-" + std::to_wstring(GetCurrentProcessId());
    struct Cleanup {
      std::wstring root;
      ~Cleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
      }
    } cleanup{root};
    variant_keys();
    layouts();
    publish_and_detect();
    settings_validation();
    settings_files(root);
    selection();
    learning();
    trimming();
    display_choices(root);
    camera_shapes();
    std::printf("PASS: %u discovered aircraft checks (variant keys, layouts, detection, settings files, learning).\n", checks);
    return 0;
  } catch (const std::exception& error) {
    settings_override.clear();
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
