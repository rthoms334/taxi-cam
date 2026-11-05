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
              manifest.find("\\\\html_ui\\\\pages\\\\VCockpit\\\\Core\\\\VCockpit.html") != std::string::npos &&
              manifest.find("\"package_order_hint\": \"PANEL_PATCH\"") != std::string::npos,
          "manifest names the overridden file");
  const auto script_manifest = marks_package::manifest_json("1.9.21", marks_package::Mode::script);
  require(script_manifest.find("\\\\html_ui\\\\pages\\\\VCockpit\\\\Core\\\\VCockpit.js\"") != std::string::npos &&
              script_manifest.find("VCockpit.html") == std::string::npos,
          "the script mode's manifest names the script only");
  require(marks_package::lists_file("{ \"globally_overriden_base_sim_files\": [ \"/HTML_UI/pages/vcockpit/core/vcockpit.js\" ] }",
                                    marks_package::ScriptRelative) &&
              !marks_package::lists_page(
                  "{ \"globally_overriden_base_sim_files\": [ \"\\\\html_ui\\\\Pages\\\\VCockpit\\\\Core\\\\VCockpit.js\" ] }"),
          "listed files are matched by path, whatever the case and slashes");
  // The script: the stock VCockpit.js with the helper after it, once.
  const auto script = marks_package::patch_script("class VCockpitPanel {}\r\n//# sourceMappingURL=VCockpit.js.map", "(function () {})();");
  require(script.rfind("class VCockpitPanel {}\r\n//# sourceMappingURL=VCockpit.js.map\r\n", 0) == 0 &&
              script.find(marks_package::ScriptBanner) != std::string::npos && script.find("(function () {})();\r\n") != std::string::npos,
          "the helper follows the stock script on lines of its own");
  require(marks_package::patch_script(script, "(function () {})();").empty(), "a script that already carries the helper is refused");
  require(marks_package::patch_script("", "(function () {})();").empty(), "an empty stock script is refused");
  require(marks_package::loads_script(Stock) && !marks_package::loads_script("<html><head></head></html>"),
          "a page that loads VCockpit.js is told apart");
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
    require(marks_package::ensure({}, community, "helper", 0) == Outcome::no_simulator, "no simulator folder: nothing written");
    require(marks_package::ensure(simulator, {}, "helper", 0) == Outcome::no_community, "no Community folder: nothing written");
    require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::written && fs::exists(marker) &&
                fs::exists(package / marks_package::PageRelative) && fs::exists(package / "layout.json"),
            "a first check writes the package with its marker");
    require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::current, "an unchanged package is left alone");
    require(marks_package::ensure(simulator, community, "helper 2", 0) == Outcome::written, "a changed helper rewrites it");
    write(marker, "incomplete\n");
    require(marks_package::ensure(simulator, community, "helper 2", 0) == Outcome::written,
            "a package whose write stopped part way is rewritten");
    // Another package's folder of the same name is never touched.
    require(marks_package::ensure(simulator, other, "helper", 0) == Outcome::failed && fs::exists(other / marks_package::PackageName) &&
                !fs::exists(other / marks_package::PackageName / marks_package::MarkerName),
            "a folder without our marker is refused");
    require(!marks_package::remove(other) && fs::exists(other / marks_package::PackageName), "the uninstaller keeps a foreign folder");
    // A simulator page Taxi Cam cannot extend removes the package.
    std::string twice(Stock);
    twice.insert(twice.find("</head>"), "    <script type=\"text/javascript\" src=\"VCockpit.js\"></script>\r\n");
    write(stock_page, twice);
    require(marks_package::ensure(simulator, community, "helper 2", 0) == Outcome::removed && !fs::exists(package),
            "a changed stock page removes the package");
    require(marks_package::ensure(simulator, community, "helper 2", 0) == Outcome::page_changed, "and keeps saying so");
    write(stock_page, Stock);
    require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::written, "a usable page writes it again");
    // Another package's copy of the page, listed as overridden (issue 184:
    // vRAAS's panel-raas, same order hint, still won): ours replaces the
    // stock VCockpit.js instead, which every copy of the page loads, and
    // goes back to the page when that package goes.
    {
      const auto read = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
      };
      const auto stock_script = stock_page.parent_path() / "VCockpit.js";
      write(stock_script, "class VCockpitPanel {}\r\n//# sourceMappingURL=VCockpit.js.map\r\n");
      const auto other = community / "panel-raas";
      fs::create_directories((other / marks_package::PageRelative).parent_path());
      std::string other_page(Stock);
      other_page.insert(other_page.find("</body>"), "    <script defer src=\"/RAAS/RaasMain.js\"></script>\r\n");
      write(other / marks_package::PageRelative, other_page);
      const std::string listed = "\"globally_overriden_base_sim_files\": [ \"\\\\html_ui\\\\pages\\\\VCockpit\\\\Core\\\\VCockpit.html\" ]";
      write(other / "manifest.json", "{ \"package_order_hint\": \"PANEL_PATCH\", " + listed + " }");
      const auto taken = marks_package::takers(community);
      require(taken.page == std::vector<std::wstring>{L"panel-raas"} && taken.script.empty() &&
                  marks_package::mode_for(taken) == marks_package::Mode::script,
              "another package taking the page is found");
      // Our old page held open by another program (the simulator, a virus
      // scanner): the switch still succeeds, and the page goes at the next
      // check once it is let go.
      const HANDLE held = CreateFileW((package / marks_package::PageRelative).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      require(held != INVALID_HANDLE_VALUE, "our page can be held open");
      require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::written,
              "a page taken by another package rewrites ours");
      require(fs::exists(package / marks_package::PageRelative), "a held page stays for now");
      CloseHandle(held);
      require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::current &&
                  !fs::exists(package / marks_package::PageRelative),
              "a page let go is removed at the next check");
      const auto script_file = package / marks_package::ScriptRelative;
      require(fs::exists(script_file) && !fs::exists(package / marks_package::PageRelative) &&
                  !fs::exists(package / marks_package::HelperRelative),
              "ours holds the script only");
      require(read(script_file).rfind("class VCockpitPanel {}", 0) == 0 && read(script_file).find("helper") != std::string::npos,
              "the stock script with the helper after it");
      require(read(package / "manifest.json").find("VCockpit.js\"") != std::string::npos &&
                  read(package / "manifest.json").find("VCockpit.html") == std::string::npos,
              "the manifest names the script");
      require(read(package / "layout.json").find("VCockpit.js") != std::string::npos &&
                  read(package / "layout.json").find("VCockpit.html") == std::string::npos,
              "the layout lists the script only");
      require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::current, "an unchanged script leaves ours alone");
      write(other / marks_package::PageRelative, other_page + "<!-- updated -->\r\n");
      require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::current,
              "the other package's own page changing needs nothing of ours");
      write(stock_script, "class VCockpitPanel { update() {} }\r\n");
      require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::written &&
                  read(script_file).find("update()") != std::string::npos,
              "a simulator update to the script rewrites ours");
      // Not listed: in MSFS 2024 that copy replaces nothing, so ours keeps the page.
      write(other / "manifest.json", "{ \"package_order_hint\": \"PANEL_PATCH\" }");
      require(marks_package::takers(community).page.empty(), "an unlisted copy takes nothing");
      require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::written &&
                  fs::exists(package / marks_package::PageRelative) && fs::exists(package / marks_package::HelperRelative) &&
                  !fs::exists(script_file),
              "back on the page, with no script left behind");
      write(other / "manifest.json", "{ " + listed + " }");
      require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::written && fs::exists(script_file) &&
                  !fs::exists(package / marks_package::PageRelative),
              "an unhinted package taking the page moves ours to the script as well");
      // The other add-on removed: ours goes back to the page.
      fs::remove_all(other);
      require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::written &&
                  fs::exists(package / marks_package::PageRelative) && !fs::exists(script_file),
              "a removed package returns ours to the page");
      // A taken page that doesn't load VCockpit.js: that add-on works alone.
      fs::create_directories((other / marks_package::PageRelative).parent_path());
      write(other / marks_package::PageRelative, "<html><head></head></html>");
      write(other / "manifest.json", "{ " + listed + " }");
      require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::conflict && !fs::exists(package),
              "a taken page without the script leaves ours out");
      write(other / marks_package::PageRelative, other_page);
      require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::written, "with the script line, ours comes back");
      // Another package replacing the script too: left alone.
      const auto script_tool = community / "script-tool";
      fs::create_directories((script_tool / marks_package::ScriptRelative).parent_path());
      write(script_tool / marks_package::ScriptRelative, "tool");
      write(script_tool / "manifest.json",
            "{ \"globally_overriden_base_sim_files\": [ \"\\\\html_ui\\\\pages\\\\VCockpit\\\\Core\\\\VCockpit.js\" ] }");
      require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::conflict && !fs::exists(package),
              "a taken script leaves ours out while the page is taken");
      fs::remove_all(other);
      require(
          marks_package::ensure(simulator, community, "helper", 0) == Outcome::written && fs::exists(package / marks_package::PageRelative),
          "with the page free, ours replaces the page and loads that package's script");
      fs::remove_all(script_tool);
      // The stock script gone while the page is taken: like a changed page.
      fs::create_directories((other / marks_package::PageRelative).parent_path());
      write(other / marks_package::PageRelative, other_page);
      write(other / "manifest.json", "{ " + listed + " }");
      fs::remove(stock_script);
      require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::removed && !fs::exists(package),
              "no stock script removes ours");
      require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::page_changed, "and keeps saying so");
      write(stock_script, "class VCockpitPanel {}\r\n");
      // A 1.3.3 test build's later-named package is replaced by ours.
      const auto late = community / marks_package::LatePackageName;
      fs::create_directories(late);
      write(late / marks_package::MarkerName, "format=2\r\n");
      require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::written && !fs::exists(late) && fs::exists(script_file),
              "the later-named package goes");
      // A running simulator mounted the package as it was when it started
      // (FILETIME 1: long before this package was written): a switch while
      // it runs keeps the old mode's file for that session, and the file goes
      // at the first check of a session started after the switch.
      fs::remove_all(other);
      require(marks_package::ensure(simulator, community, "helper", 1) == Outcome::written &&
                  fs::exists(package / marks_package::PageRelative) && fs::exists(script_file),
              "a switch while the simulator runs keeps its mounted script");
      require(marks_package::ensure(simulator, community, "helper", 1) == Outcome::current && fs::exists(script_file),
              "and keeps it for the rest of that session");
      require(
          marks_package::ensure(simulator, community, "helper", marks_package::StartUnknown) == Outcome::current && fs::exists(script_file),
          "a simulator whose start can't be read keeps it too");
      require(marks_package::ensure(simulator, community, "helper", marks_package::detail::file_time(marker) + 1) == Outcome::current &&
                  !fs::exists(script_file),
              "a session started after the switch lets it go");
      fs::create_directories((other / marks_package::PageRelative).parent_path());
      write(other / marks_package::PageRelative, other_page);
      write(other / "manifest.json", "{ " + listed + " }");
      require(marks_package::ensure(simulator, community, "helper", 1) == Outcome::written && fs::exists(script_file) &&
                  fs::exists(package / marks_package::PageRelative) && fs::exists(package / marks_package::HelperRelative),
              "the other way round too: the mounted page and helper stay");
      require(marks_package::ensure(simulator, community, "helper", marks_package::StartUnknown) == Outcome::current &&
                  fs::exists(package / marks_package::PageRelative) && fs::exists(package / marks_package::HelperRelative),
              "an unknown start keeps the mounted page and helper too");
      fs::remove_all(other);
      require(marks_package::ensure(simulator, community, "helper", 0) == Outcome::written && !fs::exists(script_file) &&
                  fs::exists(package / marks_package::PageRelative),
              "without it ours is on the page again, the script gone with no simulator running");
    }
    // Other packages replacing the page, its script or BaseInstrument.js are
    // named for the logs; ours is not.
    fs::create_directories(community / "zzz-page" / "html_ui" / "Pages" / "VCockpit" / "Core");
    write(community / "zzz-page" / marks_package::PageRelative, "page");
    write(community / "zzz-page" / marks_package::ScriptRelative, "script");
    fs::create_directories(community / "base-tool" / "html_ui" / "Pages" / "VCockpit" / "Instruments" / "Shared");
    write(community / "base-tool" / marks_package::BaseInstrumentRelative, "base");
    fs::create_directories(community / "livery-only");
    const auto overrides = marks_package::other_page_overrides(community);
    require(overrides.page == std::vector<std::wstring>{L"zzz-page"} && overrides.script == std::vector<std::wstring>{L"zzz-page"} &&
                overrides.base_instrument == std::vector<std::wstring>{L"base-tool"},
            "other page, script and BaseInstrument packages are listed, ours and unrelated ones are not");
    require(marks_package::other_page_overrides({}).page.empty(), "no Community folder: nothing listed");
    fs::remove_all(community / "zzz-page", ignored);
    fs::remove_all(community / "base-tool", ignored);
    fs::remove_all(community / "livery-only", ignored);
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
