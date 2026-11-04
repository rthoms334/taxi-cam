#include "../../src/setup/marks_package.hpp"
#include <cstdio>
#include <cstdlib>
#include "../../src/app/legacy_display_names.hpp"

namespace {
using namespace taxi_camera::standalone;
unsigned checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
// The stock VCockpit.html (asobo-vcockpits-core, Store 1.9.21), abridged.
constexpr std::string_view Stock =
    "<!DOCTYPE html>\r\n<html>\r\n<head>\r\n"
    "    <script type=\"text/javascript\" src=\"/Pages/VCockpit/Instruments/Shared/BaseInstrument.js\"></script>\r\n"
    "    <script type=\"text/javascript\" src=\"/Pages/VCockpit/Instruments/Shared/Utils/XMLLogic.js\"></script>\r\n"
    "    <script type=\"text/javascript\" src=\"VCockpit.js\"></script>\r\n"
    "</head>\r\n<body>\r\n    <div id=\"panel\"></div>\r\n</body>\r\n</html>\r\n";
}  // namespace

int main() {
  // The page: one line added after VCockpit.js, same indent and line ending.
  const auto page = marks_package::patch_page(Stock);
  require(!page.empty(), "the stock page is patched");
  require(
      page.find("<script type=\"text/javascript\" src=\"VCockpit.js\"></script>\r\n"
                "    <script type=\"text/javascript\" src=\"/Pages/VCockpit/Core/taxi-cam-marks.js\"></script>\r\n") != std::string::npos,
      "the helper line follows VCockpit.js");
  require(
      page.size() ==
          Stock.size() +
              std::string_view("    <script type=\"text/javascript\" src=\"/Pages/VCockpit/Core/taxi-cam-marks.js\"></script>\r\n").size(),
      "nothing else changes");
  require(marks_package::patch_page(page).empty(), "a page that already names the helper is refused");
  std::string twice(Stock);
  twice.insert(twice.find("</head>"), "    <script type=\"text/javascript\" src=\"VCockpit.js\"></script>\r\n");
  require(marks_package::patch_page(twice).empty(), "two VCockpit.js lines are refused");
  std::string moved(Stock);
  moved.replace(moved.find("src=\"VCockpit.js\""), 17, "src=\"VCockpitMain.js\"");
  require(marks_package::patch_page(moved).empty(), "a renamed script is refused");
  const auto unix_page = marks_package::patch_page("<head>\n\t<script type=\"text/javascript\" src=\"VCockpit.js\"></script>\n</head>\n");
  require(unix_page ==
              "<head>\n\t<script type=\"text/javascript\" src=\"VCockpit.js\"></script>\n"
              "\t<script type=\"text/javascript\" src=\"/Pages/VCockpit/Core/taxi-cam-marks.js\"></script>\n</head>\n",
          "LF pages keep LF and the tab indent");
  // Manifest and layout.
  require(marks_package::minimum_game_version("{ \"minimum_game_version\": \"1.9.21\", \"x\": 1 }") == "1.9.21", "minimum version read");
  require(marks_package::minimum_game_version("{ \"minimum_game_version\": \"1.9\\\"\" }").empty(), "odd version refused");
  const auto manifest = marks_package::manifest_json("1.9.21");
  require(manifest.find("\"minimum_game_version\": \"1.9.21\"") != std::string::npos &&
              manifest.find("\\\\html_ui\\\\pages\\\\VCockpit\\\\Core\\\\VCockpit.html") != std::string::npos,
          "manifest names the overridden file");
  const auto layout = marks_package::layout_json({{"a/b.html", 10, 20}, {"a/c.js", 30, 40}});
  require(layout.find("{ \"path\": \"a/b.html\", \"size\": 10, \"date\": 20 },") != std::string::npos &&
              layout.find("{ \"path\": \"a/c.js\", \"size\": 30, \"date\": 40 }\r\n") != std::string::npos,
          "layout lists both files");

  // Old saved names: texture= to the section and first gauge.
  require(legacy_names::gauge_name("WasmInstrument/WasmInstrument.html?wasm_module=pmdg_777_msfs.wasm&wasm_gauge=DU_LeftOutboard") ==
              "DU_LeftOutboard",
          "WASM gauge name");
  require(legacy_names::gauge_name("PMDGTablet/pmdg-777-300ER/PMDGTabletCA.html") == "PMDGTabletCA", "HTML gauge name");
  constexpr std::string_view cfg =
      "[VCockpit01]\r\nsize_mm=2048,2048\r\npixel_size=2048,2048\r\ntexture=DUS\r\n"
      "htmlgauge00=WasmInstrument/WasmInstrument.html?wasm_module=pmdg_777_msfs.wasm&wasm_gauge=DU_LeftOutboard,    30,   33, 958, 971\r\n"
      "htmlgauge01=WasmInstrument/WasmInstrument.html?wasm_module=pmdg_777_msfs.wasm&wasm_gauge=DU_LeftInboard, 1058, 33, 958, 971\r\n"
      "\r\n[VCockpit02] // lower\r\ntexture = EICASCDU ; comment\r\n"
      "htmlgauge00=WasmInstrument/WasmInstrument.html?wasm_module=pmdg_777_msfs.wasm&wasm_gauge=DU_Upper, 30, 33, 958, 971\r\n"
      "[VPainting01]\r\ntexture=$RegistrationNumber\r\n";
  auto sections = legacy_names::parse_panel_cfg(cfg);
  require(sections.size() == 2 && sections[0].section == "VCockpit01" && sections[0].gauge == "DU_LeftOutboard", "sections parsed");
  require(std::string(legacy_names::key_for(sections, "DUS").data()) == "@DU_LeftOutboard", "DUS converts");
  require(legacy_names::key_for(sections, "$RegistrationNumber")[0] == 0, "VPainting sections are not displays");
  // An attachment naming the same texture another way: no guess.
  sections.push_back({"VCockpit17", "DUS", "Other", {"Other"}});
  require(legacy_names::key_for(sections, "DUS")[0] == 0, "ambiguous texture refused");
  // Fenix A320 PFD (Part_Interior_Cockpit panel.cfg): three full-panel gauges,
  // only the last one's block is read. FSLabs starts at htmlgauge01. A gauge
  // too small for a block leaves the section alone.
  constexpr std::string_view more =
      "[VCockpit01]\r\nsize_mm=768,768\r\npixel_size=768,768\r\ntexture=$A320_PFD_Captain\r\n"
      "htmlgauge00=WasmInstrument/WasmInstrument.html?wasm_module=FenixDisplayModule.wasm&wasm_gauge=WXR, 0,0,768,768\r\n"
      "htmlgauge01=WasmInstrument/WasmInstrument.html?wasm_module=FenixDisplayModule.wasm&wasm_gauge=WXR_2, 0,0,768,768\r\n"
      "htmlgauge02=WasmInstrument/WasmInstrument.html?wasm_module=FenixDisplayModule.wasm&wasm_gauge=PFD_Captain, 0,0,768,768\r\n"
      "[VCockpit03]\r\nsize_mm=768,768\r\npixel_size=768,768\r\ntexture=$DU1\r\n\r\n"
      "htmlgauge01=WasmInstrument/WasmInstrument.html?wasm_module=fsl-a32x.wasm&wasm_gauge=LCDScreen_1,\t\t   0,    0, 768,768\r\n"
      "[VCockpit09]\r\nsize_mm=100,100\r\npixel_size=100,100\r\ntexture=$TINY\r\nhtmlgauge00=Tiny/Tiny.html, 0,0,10,10\r\n";
  const auto more_sections = legacy_names::parse_panel_cfg(more);
  require(std::string(legacy_names::key_for(more_sections, "$A320_PFD_Captain").data()) == "@PFD_Captain",
          "a gauge under a later block is skipped");
  require(std::string(legacy_names::key_for(more_sections, "$DU1").data()) == "@LCDScreen_1", "gauges from htmlgauge01");
  require(legacy_names::key_for(more_sections, "$TINY")[0] == 0, "no gauge fits: nothing to name it by");
  // Keys saved by a test build that named panels by number convert too.
  require(std::string(legacy_names::key_for(more_sections, "VCockpit01/PFD_Captain").data()) == "@PFD_Captain", "test-build key converts");
  // The A350's two EFIS panels: the same gauge, ranked in panel order.
  constexpr std::string_view a350 =
      "[VCockpit02]\r\nsize_mm=1644,1024\r\npixel_size=1644,1024\r\ntexture=$EFIS_LEFT\r\n"
      "htmlgauge00=WasmInstrument/WasmInstrument.html?wasm_module=efis.wasm&wasm_gauge=EFIS, 0,0,1644,1024,L\r\n"
      "[VCockpit03]\r\nsize_mm=1644,1024\r\npixel_size=1644,1024\r\ntexture=$EFIS_RIGHT\r\n"
      "htmlgauge00=WasmInstrument/WasmInstrument.html?wasm_module=efis.wasm&wasm_gauge=EFIS, 0,0,1644,1024,R\r\n";
  const auto twins = legacy_names::parse_panel_cfg(a350);
  require(std::string(legacy_names::key_for(twins, "$EFIS_LEFT").data()) == "@EFIS#0/2" &&
              std::string(legacy_names::key_for(twins, "$EFIS_RIGHT").data()) == "@EFIS#1/2",
          "twins ranked in panel order");
  require(legacy_names::airplane_folder("SimObjects\\Airplanes\\FNX_32X\\presets\\fnx\\FNX_320_CFM_SL\\config\\aircraft.CFG") ==
              std::filesystem::path("SimObjects") / "Airplanes" / "FNX_32X",
          "airplane folder from the sim path");
  // Writing and removing the package, on a temporary simulator and Community.
  {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("taxi-cam-marks-" + std::to_string(GetCurrentProcessId()));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    const auto simulator = root / "sim", community = root / "Community", other = root / "OtherCommunity";
    const auto stock_page = simulator / "Packages" / "asobo-vcockpits-core" / "html_ui" / "Pages" / "VCockpit" / "Core" / "VCockpit.html";
    fs::create_directories(stock_page.parent_path());
    fs::create_directories(community);
    fs::create_directories(other / marks_package::PackageName);
    const auto write = [](const fs::path& path, std::string_view text) { std::ofstream(path, std::ios::binary) << text; };
    write(stock_page, Stock);
    write(simulator / "Packages" / "asobo-vcockpits-core" / "manifest.json", "{ \"minimum_game_version\": \"1.9.21\" }");
    using marks_package::Outcome;
    const auto package = community / marks_package::PackageName;
    const auto marker = package / marks_package::MarkerName;
    require(marks_package::ensure({}, community, "helper") == Outcome::no_simulator, "no simulator folder: nothing written");
    require(marks_package::ensure(simulator, {}, "helper") == Outcome::no_community, "no Community folder: nothing written");
    require(marks_package::ensure(simulator, community, "helper") == Outcome::written && fs::exists(marker) &&
                fs::exists(package / marks_package::PageRelative) && fs::exists(package / "layout.json"),
            "a first check writes the package with its marker");
    require(marks_package::ensure(simulator, community, "helper") == Outcome::current, "an unchanged package is left alone");
    require(marks_package::ensure(simulator, community, "helper 2") == Outcome::written, "a changed helper rewrites it");
    write(marker, "incomplete\n");
    require(marks_package::ensure(simulator, community, "helper 2") == Outcome::written,
            "a package whose write stopped part way is rewritten");
    // Another package's folder of the same name is never touched.
    require(marks_package::ensure(simulator, other, "helper") == Outcome::failed && fs::exists(other / marks_package::PackageName) &&
                !fs::exists(other / marks_package::PackageName / marks_package::MarkerName),
            "a folder without our marker is refused");
    require(!marks_package::remove(other) && fs::exists(other / marks_package::PackageName), "the uninstaller keeps a foreign folder");
    // A simulator page Taxi Cam cannot extend removes the package.
    std::string twice(Stock);
    twice.insert(twice.find("</head>"), "    <script type=\"text/javascript\" src=\"VCockpit.js\"></script>\r\n");
    write(stock_page, twice);
    require(marks_package::ensure(simulator, community, "helper 2") == Outcome::removed && !fs::exists(package),
            "a changed stock page removes the package");
    require(marks_package::ensure(simulator, community, "helper 2") == Outcome::page_changed, "and keeps saying so");
    write(stock_page, Stock);
    require(marks_package::ensure(simulator, community, "helper") == Outcome::written, "a usable page writes it again");
    require(marks_package::remove(community) && !fs::exists(package), "the uninstaller removes our package");
    require(marks_package::remove(community), "nothing left to remove");

    // A PC with both builds: each simulator's own Community folder, never
    // the other's (Steam test, 2026-11-04: the Store's was checked while
    // Steam ran without a package).
    const auto local = root / "local", roaming = root / "roaming";
    const auto store_community = root / "store-packages" / "Community", steam_community = root / "steam-packages" / "Community";
    fs::create_directories(local / "Packages" / "Microsoft.Limitless_8wekyb3d8bbwe" / "LocalCache");
    fs::create_directories(roaming / "Microsoft Flight Simulator 2024");
    fs::create_directories(store_community);
    fs::create_directories(steam_community);
    const auto store_cfg = local / "Packages" / "Microsoft.Limitless_8wekyb3d8bbwe" / "LocalCache" / "UserCfg.opt";
    write(store_cfg, "InstalledPackagesPath \"" + (root / "store-packages").string() + "\"\r\n");
    write(roaming / "Microsoft Flight Simulator 2024" / "UserCfg.opt",
          "InstalledPackagesPath \"" + (root / "steam-packages").string() + "\"\r\n");
    wchar_t saved_local[MAX_PATH]{}, saved_roaming[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", saved_local, MAX_PATH);
    GetEnvironmentVariableW(L"APPDATA", saved_roaming, MAX_PATH);
    SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
    SetEnvironmentVariableW(L"APPDATA", roaming.c_str());
    require(marks_package::community_folder("F:\\SteamLibrary\\steamapps\\common\\MSFS2024") == steam_community &&
                marks_package::community_folder("F:/SteamLibrary/steamapps/common/MSFS2024/FlightSimulator2024.exe") == steam_community,
            "a Steam install uses Steam's Community folder");
    require(marks_package::community_folder("C:\\XboxGames\\Microsoft Flight Simulator 2024\\Content") == store_community,
            "a Store install uses the Store's");
    require(marks_package::community_folders() == std::vector<fs::path>{store_community, steam_community}, "the uninstaller sees both");
    fs::remove(store_cfg);
    require(marks_package::community_folder("C:\\XboxGames\\Microsoft Flight Simulator 2024\\Content") == steam_community,
            "an install without Store settings falls back to Steam's");
    SetEnvironmentVariableW(L"LOCALAPPDATA", saved_local);
    SetEnvironmentVariableW(L"APPDATA", saved_roaming);
    fs::remove_all(root, ignored);
  }
  std::printf("marks-package: %u checks passed\n", checks);
  return 0;
}
