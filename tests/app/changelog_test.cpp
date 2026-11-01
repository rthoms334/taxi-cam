#include "../../src/app/changelog.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
using namespace taxi_camera::standalone;
unsigned checks{};
void require(bool ok, const char* message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}
bool parses(std::string_view text) {
  std::vector<ChangelogRelease> releases;
  return parse_changelog(text, releases);
}
std::string release(const std::string& version, const std::string& change = "\"Change\"") {
  return std::string("{\"version\":\"") + version + "\",\"changes\":[" + change + "]}";
}
std::string wrap(const std::string& releases) {
  return "{\"releases\":[" + releases + "]}";
}
std::string read_file(const char* path) {
  const auto file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    throw std::runtime_error("Could not open the repository changelog");
  std::string text(kChangelogMaxBytes + 1, '\0');
  DWORD bytes{};
  const bool ok = ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &bytes, nullptr);
  CloseHandle(file);
  if (!ok)
    throw std::runtime_error("Could not read the repository changelog");
  text.resize(bytes);
  return text;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    ChangelogVersion v;
    require(parse_changelog_version("0.9.38", v) && v == ChangelogVersion{0, 9, 38}, "Parse major.minor.patch");
    require(parse_changelog_version("65535.0.0", v), "Accept 16-bit components");
    for (const char* bad : {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.2.x", "v1.2.3", "1.2.3-beta", "65536.0.0", "1..3", " 1.2.3"})
      require(!parse_changelog_version(bad, v), "Refuse malformed versions");
    require(ChangelogVersion{0, 10, 0} > ChangelogVersion{0, 9, 99} && ChangelogVersion{1, 0, 0} > ChangelogVersion{0, 99, 99},
            "Order versions numerically");

    std::vector<ChangelogRelease> releases;
    const char* error{};
    const std::string sample =
        "\xEF\xBB\xBF {\n \"releases\" : [\n"
        "  {\"version\": \"0.9.40\", \"date\": \"2026-09-24\", \"changes\": [\"Caf\\u00e9 \\\"quoted\\\" \\ud83d\\ude95\", "
        "\"Line\\nbreak\"]},\n"
        "  {\"changes\": [\"Older\"], \"version\": \"0.9.2\"}\n ]\n}\n";
    require(parse_changelog(sample, releases, &error) && !error, "Parse a valid changelog with BOM, whitespace and escapes");
    require(releases.size() == 2 && releases[0].version == ChangelogVersion{0, 9, 40} && releases[0].date == "2026-09-24" &&
                releases[1].date.empty() && releases[1].changes.size() == 1,
            "Keep release fields and order");
    require(releases[0].changes[0] == "Caf\xC3\xA9 \"quoted\" \xF0\x9F\x9A\x95", "Decode escapes and surrogate pairs as UTF-8");

    const auto installed = ChangelogVersion{0, 9, 39};
    const auto shown = releases_up_to(releases, installed);
    require(shown.size() == 1 && shown[0].version == ChangelogVersion{0, 9, 2}, "Hide notes newer than the installed version");
    require(releases_up_to(releases, {0, 9, 40}).size() == 2, "Show the installed version's own notes");
    using Kind = WhatsNewBlock::Kind;
    const auto legacy = whats_new_blocks(releases, installed);
    require(legacy.size() == 7 && legacy[0].kind == Kind::heading && legacy[0].text == L"Taxi Cam 0.9.40" &&
                legacy[0].detail == L"24 September 2026" && legacy[1].kind == Kind::section && legacy[1].text == L"Changes" &&
                legacy[2].kind == Kind::bullet && legacy[2].text == L"Café \"quoted\" \U0001F695" && legacy[3].text == L"Line\nbreak",
            "The newest release heads the notes with its date spelled out and its changes as bullets");
    require(legacy[4].kind == Kind::section && legacy[4].text == L"Earlier versions" && legacy[5].kind == Kind::release &&
                legacy[5].text == L"Taxi Cam 0.9.2" && legacy[5].detail.empty() && legacy[6].text == L"Older",
            "Earlier releases follow under their own heading; a missing date stays empty");
    const auto none = whats_new_blocks({}, installed);
    require(none.size() == 1 && none[0].kind == Kind::note && none[0].text == L"No notes are listed for Taxi Cam 0.9.39 yet.",
            "Explain an empty list");

    // Features with pictures and fixes (1.4.0 on).
    const std::string rich = wrap(
        "{\"version\":\"1.4.0\",\"date\":\"2026-11-01\",\"features\":[{\"title\":\"Set up any aircraft\",\"text\":\"Pick its "
        "displays.\",\"image\":\"docs/whats-new/1.4.0/setup.png\"},{\"text\":\"No picture.\",\"title\":\"Live view\"}],\"fixes\":[\"A "
        "fix.\"],\"changes\":[\"Other.\"]}," +
        std::string("{\"version\":\"1.3.0\",\"features\":[{\"title\":\"Older\",\"text\":\"Feature.\"}],\"fixes\":[\"Old fix.\"]}"));
    require(parse_changelog(rich, releases, &error) && releases.size() == 2 && releases[0].features.size() == 2 &&
                releases[0].features[0].image == "docs/whats-new/1.4.0/setup.png" && releases[0].features[1].image.empty() &&
                releases[0].fixes.size() == 1 && releases[0].changes.size() == 1,
            "Parse features, pictures, fixes and changes");
    const auto blocks = whats_new_blocks(releases, {1, 4, 0});
    const std::vector<Kind> order{Kind::heading, Kind::section, Kind::feature, Kind::feature, Kind::section, Kind::bullet,
                                  Kind::section, Kind::bullet,  Kind::section, Kind::release, Kind::bullet,  Kind::bullet};
    require(blocks.size() == order.size(), "Every note has its block");
    for (std::size_t i = 0; i < order.size(); ++i)
      require(blocks[i].kind == order[i], "Features, then fixes, then other changes, then earlier releases");
    require(blocks[1].text == L"New features" && blocks[2].text == L"Set up any aircraft" && blocks[2].detail == L"Pick its displays." &&
                blocks[2].image == "docs/whats-new/1.4.0/setup.png" && blocks[4].text == L"Fixes" && blocks[6].text == L"Other changes",
            "Feature cards carry their title, text and picture");
    require(blocks[10].text == L"Older: Feature." && blocks[11].text == L"Fixed: Old fix.", "Earlier releases list features and fixes");
    // Repository pictures only.
    for (const char* path : {"docs/whats-new/1.4.0/a.png", "docs/whats-new/b-c_d.jpg", "docs/whats-new/x.jpeg"})
      require(valid_whats_new_image(path), "Accept pictures under docs/whats-new/");
    for (const char* path :
         {"", "docs/whats-new/", "docs/whats-new/a.gif", "docs/a.png", "/docs/whats-new/a.png", "docs/whats-new/../secret.png",
          "docs/whats-new//a.png", "docs/whats-new/a b.png", "https://example.com/docs/whats-new/a.png", "docs/whats-new/a.png?x=1"})
      require(!valid_whats_new_image(path), "Refuse anything else as a picture");
    const std::string feature_refused[]{
        wrap("{\"version\":\"1.0.0\",\"features\":[{\"title\":\"T\"}]}"),
        wrap("{\"version\":\"1.0.0\",\"features\":[{\"text\":\"T\"}]}"),
        wrap("{\"version\":\"1.0.0\",\"features\":[{\"title\":\" \",\"text\":\"x\"}]}"),
        wrap("{\"version\":\"1.0.0\",\"features\":[{\"title\":\"T\",\"text\":\"x\",\"image\":\"a.png\"}]}"),
        wrap("{\"version\":\"1.0.0\",\"features\":[{\"title\":\"" + std::string(kChangelogMaxTitle + 1, 'x') + "\",\"text\":\"x\"}]}"),
        wrap("{\"version\":\"1.0.0\",\"features\":[]}"),
        wrap("{\"version\":\"1.0.0\",\"fixes\":[\"\"]}"),
        wrap("{\"version\":\"1.0.0\",\"features\":[{\"title\":\"T\",\"text\":\"x\",\"extra\":1}]}"),
    };
    for (const auto& input : feature_refused)
      require(!parses(input), "Refuse incomplete features, pictures outside docs/whats-new/ and empty releases");
    std::string many_features;
    for (std::size_t i = 0; i <= kChangelogMaxFeatures; ++i)
      many_features += std::string(i ? "," : "") + "{\"title\":\"T\",\"text\":\"x\"}";
    require(!parses(wrap("{\"version\":\"1.0.0\",\"features\":[" + many_features + "]}")), "Refuse more features than the bound");
    // A field a later changelog adds: refused by the repository check,
    // skipped by the companion.
    const std::string later =
        "{\"releases\":[{\"version\":\"2.0.0\",\"video\":{\"url\":\"x\",\"size\":[1,2.5e3,true,null]},"
        "\"features\":[{\"title\":\"T\",\"text\":\"x\",\"badge\":\"new\"}]}],\"schema\":2}";
    require(!parses(later), "Strict parsing refuses unknown fields");
    require(parse_changelog(later, releases, &error, ChangelogParse::lenient) && releases.size() == 1 && releases[0].features.size() == 1,
            "Lenient parsing skips fields it does not know");
    require(!parse_changelog(wrap("{\"version\":\"1.0.0\",\"changes\":[\"x\"],\"bad\":[}"), releases, &error, ChangelogParse::lenient),
            "Lenient parsing still refuses malformed JSON");

    require(parses(wrap(release("1.0.0"))), "Minimal release");
    const std::string refused[]{
        "",
        "[]",
        "{}",
        wrap(""),
        "{\"releases\":[]} x",
        "{\"releases\":[],\"releases\":[]}",
        "{\"other\":[]}",
        wrap(release("1.0.0") + ","),
        wrap("{\"version\":\"1.0.0\"}"),
        wrap("{\"version\":\"1.0.0\",\"changes\":[]}"),
        wrap(release("1.0.0", "\"  \"")),
        wrap(release("1.0.0", "1")),
        wrap(release("1.0")),
        wrap("{\"version\":\"1.0.0\",\"version\":\"1.0.0\",\"changes\":[\"x\"]}"),
        wrap("{\"version\":\"1.0.0\",\"changes\":[\"x\"],\"extra\":\"y\"}"),
        wrap("{\"version\":\"1.0.0\",\"date\":\"2026-13-01\",\"changes\":[\"x\"]}"),
        wrap("{\"version\":\"1.0.0\",\"date\":\"26-09-01\",\"changes\":[\"x\"]}"),
        wrap(release("1.0.0") + "," + release("1.0.0")),
        wrap(release("1.0.0") + "," + release("1.0.1")),
        wrap(release("1.0.0", "\"tab\there\"")),
        wrap(release("1.0.0", "\"bad \\x escape\"")),
        wrap(release("1.0.0", "\"lone \\ud83d surrogate\"")),
        wrap(release("1.0.0", "\"lone \\ude95 low\"")),
        wrap(release("1.0.0", "\"bad \xC3 utf8\"")),
        wrap(release("1.0.0", "\"unterminated")),
        wrap(release("1.0.0", "\"" + std::string(kChangelogMaxText + 1, 'x') + "\"")),
    };
    for (const auto& input : refused)
      require(!parses(input), "Refuse malformed or out-of-schema changelogs");
    releases = {ChangelogRelease{{9, 9, 9}, {}, {}, {}, {"kept"}}};
    require(!parse_changelog("{}", releases, &error) && error && releases.size() == 1 && releases[0].changes[0] == "kept",
            "A failed parse reports an error and leaves the previous result");
    require(parses(wrap(release("1.0.0", "\"" + std::string(kChangelogMaxText, 'x') + "\""))), "Accept text at the length limit");
    std::string many, too_many_changes;
    for (std::size_t i = 0; i <= kChangelogMaxReleases; ++i)
      many += (i ? "," : "") + release(std::to_string(kChangelogMaxReleases + 1 - i) + ".0.0");
    require(!parses(wrap(many)), "Refuse more releases than the bound");
    for (std::size_t i = 0; i <= kChangelogMaxChanges; ++i)
      too_many_changes += i ? ",\"x\"" : "\"x\"";
    require(!parses(wrap(release("1.0.0", too_many_changes))), "Refuse more changes than the bound");
    require(!parses(std::string(kChangelogMaxBytes + 1, ' ')), "Refuse oversized input before parsing");

    wchar_t cwd[32768]{};
    require(GetCurrentDirectoryW(32768, cwd), "Read isolated artifact root");
    const auto directory = std::wstring(cwd) + L"\\build\\changelog-test-" + std::to_wstring(GetCurrentProcessId());
    require(CreateDirectoryW(directory.c_str(), nullptr), "Create isolated settings fixture");
    const auto settings = directory + L"\\settings.ini";
    require(whats_new_pending(directory, installed), "Unread notes are pending without settings.ini");
    require(WritePrivateProfileStringW(L"aircraft", L"profile", L"3", settings.c_str()), "Seed unrelated settings");
    require(WritePrivateProfileStringW(L"whats_new", L"seen", L"garbage", settings.c_str()) && whats_new_pending(directory, installed),
            "A malformed marker keeps the link");
    require(record_whats_new_seen(directory, installed) && !whats_new_pending(directory, installed), "Reading records the version");
    require(whats_new_pending(directory, {0, 9, 40}) && whats_new_pending(directory, {0, 9, 38}),
            "Any other installed version shows the link again");
    require(GetPrivateProfileIntW(L"aircraft", L"profile", 0, settings.c_str()) == 3, "Recording keeps unrelated settings");
    require(whats_new_pending(L"", installed) && !record_whats_new_seen(L"", installed),
            "Missing settings directory fails open without writing a relative file");
    require(DeleteFileW(settings.c_str()) && RemoveDirectoryW(directory.c_str()), "Clean up only the isolated fixture");

    if (argc > 1) {
      releases.clear();
      const bool valid = parse_changelog(read_file(argv[1]), releases, &error);
      if (!valid)
        std::fprintf(stderr, "changelog.json: %s\n", error);
      require(valid, "Repository changelog.json is invalid");
      // Every picture is in the repository, within the companion's download limit.
      std::string root = argv[1];
      root.resize(root.find_last_of("\\/") == std::string::npos ? 0 : root.find_last_of("\\/") + 1);
      for (const auto& entry : releases)
        for (const auto& feature : entry.features) {
          if (feature.image.empty())
            continue;
          WIN32_FILE_ATTRIBUTE_DATA data{};
          const bool found = GetFileAttributesExA((root + feature.image).c_str(), GetFileExInfoStandard, &data) &&
                             !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
          if (!found)
            std::fprintf(stderr, "changelog.json: missing picture %s\n", feature.image.c_str());
          require(found && !data.nFileSizeHigh && data.nFileSizeLow > 0 && data.nFileSizeLow <= 2 * 1024 * 1024,
                  "Each feature picture is a repository file of at most 2 MiB");
        }
    }
    std::printf("PASS What's new changelog: %u parsing, filtering, layout and read-state checks%s.\n", checks,
                argc > 1 ? " including the repository changelog" : "");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL What's new changelog: %s\n", error.what());
    return 1;
  }
}
