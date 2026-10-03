#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include "../../src/app/settings_store.hpp"

using namespace taxi_camera::standalone;

namespace {
unsigned checks{};
void require(bool ok, const char* label) {
  ++checks;
  if (!ok)
    throw std::runtime_error(label);
}
std::wstring ini(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
  wchar_t value[256]{};
  GetPrivateProfileStringW(section, key, L"<missing>", value, 256, path.c_str());
  return value;
}
void write_utf16(const std::wstring& path, const std::wstring& text) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream stream(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
  const wchar_t bom = 0xfeff;
  stream.write(reinterpret_cast<const char*>(&bom), sizeof(bom));
  stream.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size() * sizeof(wchar_t)));
}
void write_ansi(const std::wstring& path, const std::string& text) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream(std::filesystem::path(path), std::ios::binary | std::ios::trunc) << text;
}
bool utf16(const std::wstring& path) {
  std::ifstream stream(std::filesystem::path(path), std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  return bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff && static_cast<unsigned char>(bytes[1]) == 0xfe;
}
}  // namespace

int main() {
  wchar_t module[MAX_PATH]{};
  GetModuleFileNameW(nullptr, module, MAX_PATH);
  const auto root = std::filesystem::path(module).parent_path() / (L"camera-rate-migration-test-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::remove_all(root);
  try {
    SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
    const auto folder = (root / L"Taxi Cam").wstring();
    const auto settings = folder + L"\\settings.ini";
    const auto profile = folder + L"\\profiles\\fbw-a380x.ini";
    const auto legacy = (root / L"380 Taxi Cam" / L"profiles" / L"ini-a350-900.ini").wstring();
    const auto unknown = folder + L"\\profiles\\custom-aircraft.ini";
    write_ansi(settings, "[aircraft]\r\nprofile=4\r\nautomatic=1\r\n");
    write_utf16(profile, L"[display]\r\ncamera_rate=15\r\ncalibration_budget=4096\r\n[nose]\r\nforward=27.25\r\n");
    write_ansi(legacy, "[display]\r\ncamera_rate=30\r\n");
    write_ansi(unknown, "[display]\r\ncamera_rate=30\r\n");
    migrate_saved_camera_rate();
    require(ini(profile, L"display", L"camera_rate") == L"10", "Existing profile receives camera_rate=10 once");
    require(ini(profile, L"nose", L"forward") == L"27.25" && ini(profile, L"display", L"calibration_budget") == L"4096",
            "Migration keeps calibration");
    require(utf16(profile), "Migration keeps the UTF-16 profile encoding");
    require(ini(legacy, L"display", L"camera_rate") == L"10", "Former settings folder profile migrated");
    require(ini(unknown, L"display", L"camera_rate") == L"30", "Unknown profile untouched");
    require(ini(settings, L"display", L"camera_rate_revision") == L"2", "Migration stamps settings.ini");
    require(ini(settings, L"aircraft", L"profile") == L"4", "Migration keeps the aircraft selection");

    WritePrivateProfileStringW(L"display", L"camera_rate", L"15", profile.c_str());
    migrate_saved_camera_rate();
    require(ini(profile, L"display", L"camera_rate") == L"15", "A later user choice survives after the stamp");

    // Setup's earlier revision 1 forced camera_rate=5; it is moved to 10 once.
    WritePrivateProfileStringW(L"display", L"camera_rate_revision", L"1", settings.c_str());
    WritePrivateProfileStringW(L"display", L"camera_rate", L"5", profile.c_str());
    migrate_saved_camera_rate();
    require(ini(profile, L"display", L"camera_rate") == L"10", "Revision-1 rate restamped to 10");

    // A missing settings.ini receives only the stamp.
    std::filesystem::remove(settings);
    WritePrivateProfileStringW(L"display", L"camera_rate", L"15", profile.c_str());
    migrate_saved_camera_rate();
    require(ini(settings, L"display", L"camera_rate_revision") == L"2" && ini(settings, L"display", L"camera_rate") == L"<missing>",
            "Missing settings.ini receives stamp-only settings");
    require(ini(profile, L"display", L"camera_rate") == L"10", "Profile migrated when settings.ini was missing");

    // A profile that cannot be written leaves the stamp unset for a retry.
    std::filesystem::remove(settings);
    WritePrivateProfileStringW(L"display", L"camera_rate", L"15", profile.c_str());
    HANDLE lock = CreateFileW(profile.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    require(lock != INVALID_HANDLE_VALUE, "Lock isolated profile");
    migrate_saved_camera_rate();
    CloseHandle(lock);
    require(ini(settings, L"display", L"camera_rate_revision") == L"<missing>", "Failed profile write leaves migration pending");
    migrate_saved_camera_rate();
    require(ini(profile, L"display", L"camera_rate") == L"10" && ini(settings, L"display", L"camera_rate_revision") == L"2",
            "Pending migration completes at the next start");

    // A junction in place of the former profiles folder is never followed.
    std::filesystem::remove(settings);
    const auto outside = (root / L"outside").wstring();
    write_ansi(outside + L"\\ini-a350-900.ini", "[display]\r\ncamera_rate=30\r\n");
    std::filesystem::remove_all(root / L"380 Taxi Cam" / L"profiles");
    const auto junction = (root / L"380 Taxi Cam" / L"profiles").wstring();
    const std::wstring command = L"cmd.exe /c mklink /J \"" + junction + L"\" \"" + outside + L"\" >nul";
    require(_wsystem(command.c_str()) == 0, "Create isolated junction fixture");
    migrate_saved_camera_rate();
    require(ini(outside + L"\\ini-a350-900.ini", L"display", L"camera_rate") == L"30", "Migration did not follow a junction");
    require(ini(settings, L"display", L"camera_rate_revision") == L"<missing>", "Refused junction leaves migration pending");
    RemoveDirectoryW(junction.c_str());

    settings_override = (root / L"preview").wstring();
    std::filesystem::remove(settings);
    migrate_saved_camera_rate();
    require(!std::filesystem::exists(settings), "Preview settings never migrate the user's profiles");
    settings_override.clear();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    std::filesystem::remove_all(root);
    return 1;
  }
  std::filesystem::remove_all(root);
  std::printf("PASS camera rate migration: %u checks.\n", checks);
  return 0;
}
