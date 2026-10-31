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
  l.width = l.height = 768;
  l.mips = 5;
  l.format = profiles::DiscoveredFormat;
  std::strcpy(l.textures[0], "$PFD_CAPT");
  std::strcpy(l.textures[1], "$PFD_FO");
  l.regions[0] = l.regions[1] = {0, 0, 768, 768};
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
  refused([](auto& l) { l.format = 87; }, "Only R8G8B8A8_UNORM panel textures");
  refused([](auto& l) { l.mips = 1; }, "Panel textures have mips");
  refused([](auto& l) { l.sides = 1; }, "At least two sides");
  refused([](auto& l) { l.regions[1] = {0, 0, 700, 768}; }, "Every side is one patch size");
  refused([](auto& l) { l.regions[0] = l.regions[1] = {100, 0, 868, 768}; }, "Regions stay inside the texture");
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
  require(profiles::matches_display(*first, 768, 768, 5, 28) && !profiles::matches_display(*first, 768, 768, 5, 29),
          "The layout's shape is the display shape");
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
  invalid.mips = 0;
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
  Settings sibling;
  require(load_settings(sibling, L"missing", profiles::DiscoveredProfileId, Fenix321Key), "A new preset starts from its family");
  auto expected = fenix_layout(Fenix321);
  require(profiles::same_layout(sibling.discovered, expected) && sibling.mounts == profiles::DiscoveredMounts,
          "It takes the family's displays and the starting camera mounts");
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
  require(std::strcmp(l.key, Fenix320Key) == 0 && l.width == 768 && l.mips == 5 && l.regions[1].right == 768,
          "Key, shape and whole-texture regions");
  require(std::strcmp(l.anchors[0].name, "$ECAM_UPPER") == 0 && std::strcmp(l.anchors[1].name, "$ND_CAPT") == 0 &&
              std::strcmp(l.anchors[2].name, "$EFB") == 0 && std::strcmp(l.anchors[3].name, "$ND_FO") == 0 && !l.anchors[4].name[0],
          "Anchors are the unique panels within two creations of each display");
  require(learn_layout(status, {"$PFD_CAPT", "$EFB", nullptr}, 2, l) == LayoutLearning::mixed_shapes, "Sides share one shape");
  require(learn_layout(status, {"$PFD_CAPT", "$NONE", nullptr}, 2, l) == LayoutLearning::unknown_panel, "Names come from the map");
  status.panel_clean = 0;
  require(learn_layout(status, {"$PFD_CAPT", "$PFD_FO", nullptr}, 2, l) == LayoutLearning::not_clean,
          "Nothing is learned after a switch to another aircraft");
  status.panel_clean = 1;
  status.panel_state = static_cast<std::uint32_t>(PanelMapState::incomplete);
  require(learn_layout(status, {"$PFD_CAPT", "$PFD_FO", nullptr}, 2, l) == LayoutLearning::not_paired, "Only a complete map");
  status.panel_state = static_cast<std::uint32_t>(PanelMapState::paired);
  std::strcpy(status.aircraft_path, "Airbus");
  require(learn_layout(status, {"$PFD_CAPT", "$PFD_FO", nullptr}, 2, l) == LayoutLearning::no_variant, "Only a known variant");
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
    std::printf("PASS: %u discovered aircraft checks (variant keys, layouts, detection, settings files, learning).\n", checks);
    return 0;
  } catch (const std::exception& error) {
    settings_override.clear();
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
