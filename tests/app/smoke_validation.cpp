#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include "../../src/app/launcher.hpp"
#include "../../src/app/settings_store.hpp"
#include "../../src/shared/protocol.hpp"

namespace {
unsigned checks{};
void require(bool value, const char* label) {
  ++checks;
  if (!value)
    throw std::runtime_error(label);
}
void audit_bridge(const wchar_t* path) {
  // Include dynamically resolved entry-point strings as well as import/symbol
  // names. D3D11On12CreateDevice previously bypassed an import-only audit.
  const auto file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(file != INVALID_HANDLE_VALUE, "Open exact bridge for bootstrap audit");
  LARGE_INTEGER length{};
  const bool sized = GetFileSizeEx(file, &length) && length.QuadPart > 0 && length.QuadPart <= 64 * 1024 * 1024;
  if (!sized) {
    CloseHandle(file);
    require(false, "Bounded exact bridge audit size");
  }
  std::vector<char> bytes(static_cast<std::size_t>(length.QuadPart));
  DWORD read{};
  const bool loaded = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
  CloseHandle(file);
  require(loaded, "Read exact bridge for bootstrap audit");
  for (const char* forbidden :
       {"SuspendThread", "ResumeThread", "NtSuspendThread", "ZwSuspendThread", "NtResumeThread", "ZwResumeThread", "NtSuspendProcess",
        "ZwSuspendProcess", "NtResumeProcess", "ZwResumeProcess", "D3D11On12CreateDevice", "MH_Initialize", "MH_CreateHook",
        "MH_EnableHook", "MH_QueueEnableHook", "MH_ApplyQueued", "MH_Uninitialize"}) {
    if (std::search(bytes.begin(), bytes.end(), forbidden, forbidden + std::strlen(forbidden)) != bytes.end())
      throw std::runtime_error(std::string("Forbidden bridge startup dependency: ") + forbidden);
    ++checks;
  }
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    using namespace taxi_camera::standalone;
    Settings settings;
    require(valid_settings(settings), "Default settings");
    require(settings.camera_rate == taxi_camera::kDefaultCameraRate, "Default camera rate changed");
    for (unsigned rate = 5; rate <= 60; ++rate) {
      auto accepted = settings;
      accepted.camera_rate = rate;
      require(valid_settings(accepted), "Supported camera rate refused");
    }
    for (const auto value : {4u, 61u, 0u, UINT32_MAX}) {
      auto bad = settings;
      bad.camera_rate = value;
      require(!valid_settings(bad), "Invalid rate admitted");
    }
    for (const auto value : {-17.f, 5.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
      auto bad = settings;
      bad.exposure = value;
      require(!valid_settings(bad), "Invalid exposure admitted");
    }
    auto bad = settings;
    bad.profile = 999;
    require(!valid_settings(bad), "Unknown aircraft admitted");
    bad = settings;
    bad.mounts[0][0] = 501;
    require(!valid_settings(bad), "Unbounded position admitted");
    bad = settings;
    bad.mounts[1][5] = 0;
    require(!valid_settings(bad), "Zero lens admitted");
    bad = settings;
    bad.calibration_budget = 0;
    require(!valid_settings(bad), "Invalid calibration budget admitted");
    for (unsigned channel = 0; channel < 3; ++channel) {
      for (const auto value : {-0.01f, 1.01f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        bad = settings;
        bad.guide_color[channel] = value;
        require(!valid_settings(bad), "Invalid marking RGB admitted");
      }
    }
    Mailbox owner, reader;
    require(owner.open(GetCurrentProcessId(), true), "Create mailbox");
    require(reader.open(GetCurrentProcessId(), false), "Open mailbox");
    require(owner.lock(100), "Lock mailbox");
    owner.data()->settings.camera_rate = 60;
    owner.data()->settings.profile_request = 17;
    owner.data()->settings.aircraft_session_epoch = 23;
    owner.data()->settings.nose_dot = {0.125f, 0.375f};
    owner.data()->settings.tail_upper = {0.25f, 0.625f};
    owner.data()->settings.tail_corner = {0.1875f, 0.75f};
    owner.data()->settings.tail_inner = {0.375f, 0.875f};
    owner.data()->settings.guide_color = {0.125f, 0.5f, 0.875f};
    owner.data()->settings.parked_rate = 8;
    owner.data()->status.effective_rate = 8;
    owner.data()->status.useful_rate = 15;
    owner.data()->status.rate_limits = 1;
    owner.data()->status.parked = 1;
    owner.data()->status.notifications[0] = {1, 5000, static_cast<std::uint32_t>(taxi_camera::SimEvent::cameras_ready), 0};
    owner.data()->settings.lower_id = 271;
    owner.data()->status.lower_id = 272;
    owner.unlock();
    require(reader.lock(100), "Read mailbox lock");
    require(reader.data()->settings.camera_rate == 60, "Settings exchange");
    require(ProtocolVersion == 15 && reader.data()->settings.notifications == 1 &&
                reader.data()->settings.nose_dot == std::array<float, 2>{0.125f, 0.375f} &&
                reader.data()->settings.tail_upper == std::array<float, 2>{0.25f, 0.625f} &&
                reader.data()->settings.tail_corner == std::array<float, 2>{0.1875f, 0.75f} &&
                reader.data()->settings.tail_inner == std::array<float, 2>{0.375f, 0.875f},
            "Guide pairs exchanged in protocol11");
    require(reader.data()->settings.parked_rate == 8 && reader.data()->status.effective_rate == 8 &&
                reader.data()->status.useful_rate == 15 && reader.data()->status.rate_limits == 1 && reader.data()->status.parked == 1,
            "Parked floor setting and rate-in-use status exchanged in protocol11");
    require(reader.data()->status.notifications[0].serial == 1 &&
                reader.data()->status.notifications[0].event == static_cast<std::uint32_t>(taxi_camera::SimEvent::cameras_ready),
            "Notification log exchanged in protocol12");
    require(reader.data()->settings.lower_id == 271 && reader.data()->status.lower_id == 272, "Lower DU texture exchanged in protocol15");
    reader.data()->settings.lower_id = 0;  // The default A380 profile has no separate lower texture.
    require(reader.data()->settings.guide_color == std::array<float, 3>{0.125f, 0.5f, 0.875f} &&
                reader.data()->settings.speed_color == settings.speed_color,
            "Marking colour exchanged independently of ground-speed colour");
    require(reader.data()->settings.profile_request == 17 && reader.data()->settings.aircraft_session_epoch == 23,
            "Profile retry and flight scope exchanged");
    reader.unlock();
    for (const auto rate : {5u, 10u}) {
      require(owner.lock(100), "Lock low-rate mailbox");
      owner.data()->settings.camera_rate = rate;
      owner.unlock();
      require(reader.lock(100), "Read low-rate mailbox");
      require(reader.data()->settings.camera_rate == rate && valid_settings(reader.data()->settings), "Low-rate IPC exchange");
      reader.unlock();
    }
    require(owner.lock(100), "Mutate test version");
    owner.data()->version = 8;
    owner.unlock();
    Mailbox refused;
    require(!refused.open(GetCurrentProcessId(), false), "Previous settings layout accepted");
    owner.close();
    reader.close();
    const DWORD found = find_simulator(L"C:\\not-a-simulator\\FlightSimulator2024.exe");
    if (found) {
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, found);
      require(process != nullptr, "Live simulator process disappeared");
      wchar_t path[32768]{};
      DWORD n = 32768;
      require(QueryFullProcessImageNameW(process, 0, path, &n), "Query live simulator path");
      CloseHandle(process);
      require(known_msfs2024_layout(path), "Non-2024 process accepted as simulator");
      require(!same_path(L"C:\\not-a-simulator\\FlightSimulator2024.exe", path), "Fake configured path must not be the live image");
    }
    require(argc == 2, "Provide exact bridge DLL");
    audit_bridge(argv[1]);
    std::wstring install = argv[1];
    install.resize(install.find_last_of(L"\\/"));
    settings_override = install + L"\\smoke-settings-" + std::to_wstring(GetCurrentProcessId());
    Settings saved = settings;
    saved.camera_rate = 60;
    saved.mounts[0][2] = 27.25;
    saved.exposure = -7.5f;
    saved.nose_dot = {0.125f, 0.375f};
    saved.tail_upper = {0.25f, 0.625f};
    saved.tail_corner = {0.1875f, 0.75f};
    saved.tail_inner = {0.375f, 0.875f};
    saved.guide_color = {0.125f, 0.5f, 0.875f};
    saved.speed_color = {0.25f, 0.75f, 0.375f};
    saved.calibration_budget = 1024;
    saved.manual_mask = 3;
    saved.left_id = 999;
    saved.right_id = 888;
    saved.route_request = 12;
    saved.profile_request = 17;
    saved.aircraft_session_epoch = 23;
    require(save_settings(saved), "Atomic profile save");
    Settings loaded;
    require(load_settings(loaded, install), "Profile reload");
    require(loaded.camera_rate == 60 && loaded.mounts[0][2] == 27.25 && loaded.exposure == -7.5f && loaded.calibration_budget == 1024,
            "Camera, exposure, rate and calibration persistence");
    require(loaded.nose_dot == saved.nose_dot && loaded.tail_upper == saved.tail_upper && loaded.tail_corner == saved.tail_corner &&
                loaded.tail_inner == saved.tail_inner,
            "All guide pairs persist with camera settings");
    require(loaded.guide_color == saved.guide_color && loaded.speed_color == saved.speed_color,
            "Marking and ground-speed colours persist independently");
    require(!loaded.manual_mask && !loaded.left_id && !loaded.right_id && !loaded.route_request && !loaded.profile_request &&
                !loaded.aircraft_session_epoch,
            "Session texture IDs and manual tests must not persist");
    saved.camera_rate = 999;
    require(!save_settings(saved), "Reject invalid save");
    require(load_settings(loaded, install) && loaded.camera_rate == 60, "Invalid save must preserve previous settings");
    for (const auto rate : {5u, 10u}) {
      saved.camera_rate = rate;
      require(save_settings(saved), "Save lower camera budget");
      require(load_settings(loaded, install) && loaded.camera_rate == rate && loaded.mounts == saved.mounts &&
                  loaded.nose_dot == saved.nose_dot && loaded.exposure == saved.exposure,
              "Low-rate persistence must retain calibration and display settings");
    }
    saved.camera_rate = 15;
    require(save_settings(saved), "Save established 15 fps preference");
    require(load_settings(loaded, install) && loaded.camera_rate == 15, "Saved 15 fps is not rewritten to the new shipped default");
    const auto profile_path = settings_path(saved);
    require(WritePrivateProfileStringW(L"display", L"camera_rate", nullptr, profile_path.c_str()) != FALSE,
            "Remove camera_rate key from isolated profile");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, profile_path.c_str());
    require(load_settings(loaded, install) && loaded.camera_rate == taxi_camera::kDefaultCameraRate && loaded.mounts == saved.mounts,
            "Missing camera_rate key uses the shipped default without dropping calibration");
    // Exercise the actual rename migration with process-local environment paths.
    const auto migration = settings_override + L"\\migration";
    require(CreateDirectoryW(migration.c_str(), nullptr) != FALSE, "Create isolated migration directory");
    wchar_t original_local[32768]{};
    const DWORD local_size = GetEnvironmentVariableW(L"LOCALAPPDATA", original_local, 32768);
    require(local_size && local_size < 32768, "Read original local app data path");
    settings_override = migration + L"\\380 Taxi Cam";
    saved.camera_rate = 45;
    require(save_settings(saved), "Save old application profile");
    const auto legacy = settings_path(saved);
    settings_override.clear();
    require(SetEnvironmentVariableW(L"LOCALAPPDATA", migration.c_str()) != FALSE, "Isolate migration environment");
    require(load_settings(loaded, install) && loaded.camera_rate == 45 && loaded.mounts[0][2] == 27.25,
            "Rename imports existing calibration");
    require(GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES, "Rename preserves original profile");
    loaded.camera_rate = 30;
    require(save_settings(loaded), "Save new application profile");
    require(load_settings(loaded, install) && loaded.camera_rate == 30, "Old profile cannot overwrite newer settings");
    require(SetEnvironmentVariableW(L"LOCALAPPDATA", original_local) != FALSE, "Restore local app data environment");
    HMODULE dll = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    require(dll != nullptr, "Load exact standalone bridge DLL");
    auto start = reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(dll, "TaxiCameraStart"));
    require(start != nullptr, "Native bridge entry point");
    require(start(nullptr) == ERROR_BAD_ENVIRONMENT, "Private camera started in wrong host");
    require(start(nullptr) == ERROR_BAD_ENVIRONMENT, "Repeated wrong-host call admitted");
    require(FreeLibrary(dll) != FALSE, "Unused wrong-host bridge unload");
    std::printf("PASS native smoke: %u settings, IPC, startup path, exact-DLL bootstrap audit and wrong-host checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL native smoke: %s\n", error.what());
    return 1;
  }
}
