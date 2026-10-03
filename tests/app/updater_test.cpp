#include "../../src/app/updater.hpp"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace taxi_camera::standalone;
namespace {
const std::string Clean = "taxi-cam-0.8.0-windows-x64-setup.exe";
const std::string Legacy = "taxi-cam-0.8.0-build.12-windows-x64-setup.exe";
const std::string Download = "https://github.com/rthoms334/taxi-cam/releases/download/v0.8.0-build.12/";
struct Asset {
  std::string name, url, state = "\"uploaded\"", size = "1234", digest = "\"sha256:" + std::string(64, 'a') + "\"";
};
Asset clean() {
  return {Clean, Download + Clean};
}
Asset legacy() {
  return {Legacy, Download + Legacy};
}
Asset sums() {
  return {"SHA256SUMS.txt", Download + "SHA256SUMS.txt", "\"uploaded\"", "100", ""};
}
std::string replace(std::string text, const std::string& from, const std::string& to) {
  for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
    text.replace(at, from.size(), to);
  return text;
}
std::string release(const std::vector<Asset>& assets, const std::string& draft = "false", const std::string& prerelease = "false") {
  std::string text = "{\"draft\":" + draft + ",\"prerelease\":" + prerelease + ",\"tag_name\":\"v0.8.0-build.12\",\"assets\":[";
  for (std::size_t i = 0; i < assets.size(); ++i) {
    const auto& a = assets[i];
    text += std::string(i ? "," : "") + "{\"name\":\"" + a.name + "\",\"state\":" + a.state + ",\"size\":" + a.size +
            ",\"browser_download_url\":\"" + a.url + "\"" + (a.digest.empty() ? "" : ",\"digest\":" + a.digest) +
            ",\"uploader\":{\"login\":\"x\"}}";
  }
  return text + "]}";
}
UpdateSelection select(const std::string& text, const UpdateVersion& current, UpdateAsset& asset) {
  json::Value value;
  if (!json::parse(text, value))
    return UpdateSelection::Invalid;
  return select_update_asset(value, current, asset);
}
}  // namespace
int main() {
  int failures = 0;
  const auto check = [&](bool ok, const char* name) {
    if (!ok) {
      std::fprintf(stderr, "FAIL: %s\n", name);
      ++failures;
    }
  };
  {
    json::Value value;
    check(json::parse("{\"a\":[1,-2.5e3,true,false,null,\"\\u00e9\\ud83d\\ude00\"]}", value) && value.find("a") &&
              value.find("a")->items.size() == 6 && value.find("a")->items[5].text == "\xC3\xA9\xF0\x9F\x98\x80",
          "JSON values and surrogate pairs");
    for (const char* bad : {"{\"a\":1,\"a\":2}", "{\"a\":1} x", "[01]", "[1.]", "\"\\ud800\"", "\"\xC3\"", "{\"a\":1,}", "[\"a\nb\"]"})
      check(!json::parse(bad, value), "malformed JSON rejected");
    check(!json::parse(std::string(40, '[') + std::string(40, ']'), value), "JSON depth bounded");
    std::int64_t number{};
    check(json::parse("[1234, 1.5, 3e2]", value) && value.items[0].integer(1, 2000, number) && number == 1234 &&
              !value.items[1].integer(0, 10, number) && value.items[2].integer(0, 300, number) && number == 300,
          "JSON integral numbers");
  }
  UpdateVersion current{}, latest{};
  check(parse_update_version(L"v0.8.0-build.7", current), "current version");
  check(parse_update_version(L"v0.8.0-build.10", latest) && newer_update(latest, current), "numeric build ordering");
  check(parse_update_version(L"v0.8.1-build.1", latest) && newer_update(latest, current), "patch version takes priority over build");
  check(parse_update_version(L"v0.9.0-build.1", latest) && newer_update(latest, current), "version takes priority over build");
  check(!newer_update(current, current), "no reinstall same release");
  check(!newer_update(current, latest), "no downgrade");
  check(parse_update_version(L"v0.8.0-build.6", latest) && !newer_update(latest, current), "lower published build ignored");
  check(parse_update_version(L"v0.7.9-build.999", latest) && !newer_update(latest, current),
        "older version ignored despite larger build");
  check(parse_update_version(L"v0.8.0-build.0", latest) && newer_update(current, latest),
        "published build is newer than local or PR build 0");
  for (const auto* tag : {L"v0.8.0-build.01", L"v0.8.0-build.-1", L"v0.8.0-build.4294967296", L"v0.8.0-build.9;calc",
                          L"0.8.0-build.9", L"v0.8.0-build.9-preview", L"v0.8.0-build.", L"v0.8.0-build.9\n",
                          L"v0.8.0-pr.12", L"v0.8.0-test.1", L"v0.8.0", L"pr-12"})
    check(!parse_update_version(tag, latest), "malformed tag rejected");
  {
    UpdateVersion before{};
    parse_update_version(L"v0.8.0-build.9", before);
    UpdateAsset asset;
    check(select(release({clean()}), before, asset) == UpdateSelection::Selected && asset.digest == std::string(64, 'a') &&
              asset.name == Clean && asset.url == Download + Clean && asset.size == 1234 && asset.tag == "v0.8.0-build.12",
          "GitHub asset digest selects the clean installer");
    UpdateVersion newer{};
    parse_update_version(L"v0.9.0-build.1", newer);
    check(select(release({clean()}), newer, asset) == UpdateSelection::Current, "older release ignored");
    check(select(release({legacy()}), before, asset) == UpdateSelection::Selected && asset.name == Legacy && asset.url == Download + Legacy,
          "exact legacy-only installer supported");
    for (bool clean_first : {true, false}) {
      auto old = legacy();
      old.digest = "\"sha256:" + std::string(64, 'c') + "\"";
      const auto text = clean_first ? release({clean(), old}) : release({old, clean()});
      check(select(text, before, asset) == UpdateSelection::Selected && asset.name == Clean && asset.digest == std::string(64, 'a'),
            "clean installer preferred regardless of asset order");
    }
    check(select(release({clean()}, "true"), before, asset) == UpdateSelection::Invalid, "draft rejected");
    check(select(release({clean()}, "false", "true"), before, asset) == UpdateSelection::Invalid, "prerelease rejected");
    check(select(release({clean()}, "\"false\""), before, asset) == UpdateSelection::Invalid, "non-boolean draft rejected");
    check(select(release({clean(), clean()}), before, asset) == UpdateSelection::Invalid, "duplicate installers rejected");
    check(select(release({clean(), legacy(), legacy()}), before, asset) == UpdateSelection::Invalid,
          "duplicate legacy names rejected when both styles present");
    check(select(release({clean(), legacy(), clean()}), before, asset) == UpdateSelection::Invalid,
          "duplicate clean names rejected when both styles present");
    check(select(release({legacy(), legacy()}), before, asset) == UpdateSelection::Invalid, "duplicate legacy-only installers rejected");
    for (bool legacy_style : {true, false}) {
      auto wrong = legacy_style ? legacy() : clean();
      wrong.name = replace(wrong.name, "0.8.0", "0.8.1");
      wrong.url = replace(wrong.url, "0.8.0", "0.8.1");
      check(select(release({wrong}), before, asset) == UpdateSelection::Invalid, "wrong semantic-version filename rejected");
      for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{{"/v0.8.0-build.12/", "/v0.8.0-build.13/"},
                                                                                     {"/v0.8.0-build.12/", "/v0.8.1-build.12/"},
                                                                                     {"/rthoms334/taxi-cam/", "/other/taxi-cam/"},
                                                                                     {"-setup.exe", "-setup.exe?other=true"}}) {
        auto changed = legacy_style ? legacy() : clean();
        changed.url = replace(changed.url, from, to);
        check(select(release({changed}), before, asset) == UpdateSelection::Invalid,
              "both filename styles require exact version, build and repository URL");
      }
    }
    auto later = legacy();
    later.name = replace(later.name, "build.12", "build.13");
    later.url = replace(later.url, "build.12-windows", "build.13-windows");
    check(select(release({later}), before, asset) == UpdateSelection::Invalid, "wrong legacy build filename rejected");
    for (int bad = 0; bad < 7; ++bad) {
      auto broken = clean();
      if (bad == 0)
        broken.url += "?other=true";
      else if (bad == 1)
        broken.state = "\"new\"";
      else if (bad == 2)
        broken.size = std::to_string(257LL * 1024 * 1024);
      else if (bad == 3)
        broken.digest = "\"md5:abcd\"";
      else if (bad == 4)
        broken.digest = "null";
      else if (bad == 5)
        broken.size = "12.5";
      else
        broken.digest = "\"\"";
      check(select(release({broken, legacy()}), before, asset) == UpdateSelection::Invalid,
            "invalid clean asset never falls back to a valid legacy asset");
    }
    auto missing = clean();
    missing.digest.clear();
    check(select(release({missing}), before, asset) == UpdateSelection::Invalid, "no digest or checksum rejected");
    check(select(release({missing, sums(), legacy()}), before, asset) == UpdateSelection::Selected && asset.digest.empty() &&
              asset.sums_url == Download + "SHA256SUMS.txt" && asset.name == Clean,
          "clean checksum fallback preferred over legacy asset digest");
    auto oversized_sums = sums();
    oversized_sums.size = std::to_string(2 * 1024 * 1024);
    check(select(release({missing, oversized_sums}), before, asset) == UpdateSelection::Invalid, "oversized checksum asset rejected");
    check(select(release({missing, sums(), sums()}), before, asset) == UpdateSelection::Invalid, "ambiguous checksum assets rejected");
    for (const auto* url : {L"http://github.com/a", L"https://github.com.evil.example/a", L"https://github.com@evil.example/a",
                            L"https://github.com:444/a", L"file:///c:/a", L"https://github.com/a#b", L"https://evil.example/a",
                            L"https://user:pass@github.com/a", L"https://github.com/a b", L"github.com/a"})
      check(!trusted_download_url(url), "unsafe transport rejected");
    for (const auto* url : {L"https://github.com/a", L"https://GitHub.com/a", L"https://api.github.com/repos/x",
                            L"https://release-assets.githubusercontent.com/a?sig=1&b=2", L"https://objects.githubusercontent.com/a"})
      check(trusted_download_url(url), "trusted release host accepted");
    const std::string line = std::string(64, 'B') + "  " + Clean;
    std::string digest;
    check(read_installer_checksum(line, Clean, digest) && digest == std::string(64, 'b'), "exact checksum filename");
    check(read_installer_checksum(std::string(64, 'c') + " *" + Legacy + "\r\n" + line + "\r\n", Clean, digest) &&
              digest == std::string(64, 'b'),
          "clean checksum selected independently of legacy checksum");
    check(!read_installer_checksum(std::string(64, 'c') + "  " + Legacy, Clean, digest), "legacy checksum cannot validate clean installer");
    check(!read_installer_checksum(line + "\n" + line, Clean, digest), "duplicate checksum rejected");
    check(!read_installer_checksum(line + ".other", Clean, digest), "similar filename rejected");
    check(!read_installer_checksum(std::string(63, 'b') + "g  " + Clean, Clean, digest), "non-hex checksum rejected");
  }
  check(update_installer_arguments(L"C:\\Users\\A B\\Taxi Cam", 123) ==
            L"/DIR=\"C:\\Users\\A B\\Taxi Cam\" /UPDATEFROMPID=123", "installer args preserve path spaces");
  check(update_installer_arguments(L"C:\\Bad\" /SILENT", 123).empty(), "reject argument injection");
  check(update_installer_arguments(L"C:\\App\\", 123).empty(), "reject trailing slash quote ambiguity");
  check(update_installer_arguments(L"C:\\App", 0).empty(), "require parent pid");
  // Test files live beside this test executable in the ignored build directory.
  wchar_t module[32768]{};
  GetModuleFileNameW(nullptr, module, 32768);
  std::wstring path(module);
  path.resize(path.find_last_of(L"\\/"));
  path += L"\\updater-integrity-test-" + std::to_wstring(GetCurrentProcessId()) + L".bin";
  { std::ofstream file(path.c_str(), std::ios::binary); file << "abc"; }
  const std::wstring hash = L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  HANDLE verified = verified_update_file(path, hash);
  check(verified != INVALID_HANDLE_VALUE, "known SHA256 digest");
  if (verified != INVALID_HANDLE_VALUE) {
    HANDLE write = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(write == INVALID_HANDLE_VALUE, "verified file cannot change before launch");
    if (write != INVALID_HANDLE_VALUE) CloseHandle(write);
    check(!DeleteFileW(path.c_str()), "verified file cannot be replaced before launch");
    CloseHandle(verified);
  }
  { std::ofstream file(path.c_str(), std::ios::binary); file << "tampered"; }
  verified = verified_update_file(path, hash);
  check(verified == INVALID_HANDLE_VALUE, "tampered installer rejected");
  if (verified != INVALID_HANDLE_VALUE) CloseHandle(verified);
  check(verified_update_file(path, L"not-a-sha256") == INVALID_HANDLE_VALUE, "malformed checksum rejected");
  Updater updater;
  UpdateResult result;
  result.available = true;
  result.installer = path;
  result.sha256 = hash;
  std::wstring error;
  check(!updater.launch(result, L"C:\\App", error), "arbitrary installer path cannot launch");
  DeleteFileW(path.c_str());
  if (!failures) std::puts("Updater version, integrity and handoff guards passed.");
  return failures ? 1 : 0;
}
