#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include "../../src/app/settings_store.hpp"
#include "../../src/camera/aircraft_identity.hpp"
#include "../../src/camera/view_resize.hpp"
#include "../../src/graphics/pfd_target_detector.hpp"
using namespace taxi_camera;
namespace {
// The vertical-FOV interpretation matches the live -900 horizon/gear fit.
// Project independent aircraft-model contact points to catch a return to
// the wide inherited framing, not just changed literal defaults.
std::array<double, 2> project_landmark(const profiles::AircraftProfile& profile, unsigned feed, std::array<double, 3> point) {
  const auto& mount = profile.mounts[feed];
  constexpr double radians = 3.14159265358979323846 / 180;
  const double pitch = mount[3] * radians, yaw = mount[4] * radians;
  const double cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
  for (unsigned i = 0; i < 3; ++i)
    point[i] -= mount[i];
  const double x = point[0] * cy - point[2] * sy;
  const double y = -point[0] * sy * sp + point[1] * cp - point[2] * cy * sp;
  const double z = point[0] * sy * cp + point[1] * sp + point[2] * cy * cp;
  assert(z > 0);
  const double half_height = z * std::tan(mount[5] / 2);
  const double aspect = double(profile.camera_panes[feed][0]) / profile.camera_panes[feed][1];
  return {0.5 + x / (2 * half_height * aspect), 0.5 - y / (2 * half_height)};
}
using GuideMember = std::array<float, 2> standalone::Settings::*;
constexpr std::array<GuideMember, 4> GuideMembers{&standalone::Settings::nose_dot, &standalone::Settings::tail_upper,
                                                  &standalone::Settings::tail_corner, &standalone::Settings::tail_inner};
bool same_guides(const standalone::Settings& a, const standalone::Settings& b) {
  for (auto member : GuideMembers)
    if (a.*member != b.*member)
      return false;
  return a.guide_color == b.guide_color;
}
void aircraft_swap_tests() {
  // Read-only IPC capture from the installed FBW A380, 2026-09-14. The brand
  // string is not the A388 ICAO designator that the old matcher required.
  constexpr char fbw_type[] = "ATCCOM.ATC_NAME AIRBUS.0.text";
  constexpr char fbw_path[] = "SimObjects\\Airplanes\\FlyByWire_A380X\\presets\\flybywire\\FlyByWire_A380_842\\config\\aircraft.CFG";
  assert(profiles::detect_aircraft(fbw_type, fbw_path) == 1);
  assert(profiles::detect_aircraft("Airbus", "SIMOBJECTS/AIRPLANES/FLYBYWIRE_A380X/liveries/Custom Airline/aircraft.cfg") == 1);
  assert(profiles::detect_aircraft(
             fbw_type, "SimObjects/Airplanes/FlyByWire_A380X/presets/flybywire/FlyByWire_A380_842_NoCabin/config/aircraft.cfg") == 1);
  assert(!profiles::detect_aircraft(fbw_type, "SimObjects/Airplanes/Other_A380/aircraft.cfg"));
  assert(!profiles::detect_aircraft(fbw_type, "SimObjects/Airplanes/FlyByWire_A380X-copy/aircraft.cfg"));
  assert(!profiles::detect_aircraft("A388", "SimObjects/Airplanes/not_FlyByWire_A380X/aircraft.cfg"));
  assert(profiles::detect_aircraft("a359 ulr", "SimObjects/Airplanes/A350/presets/iniBuilds/A350-900/config/aircraft.cfg") == 2);
  assert(!profiles::detect_aircraft("AIRBUS", "SimObjects/Airplanes/A350/presets/iniBuilds/A350-900/config/aircraft.cfg"));

  native_camera::AircraftIdentityCache identity;
  std::array<unsigned char, 296> type_packet{};
  std::array<unsigned char, 284> path_packet{};
  const auto type = [&](std::uint32_t request, const char* text, std::uint64_t now) {
    type_packet = {};
    const std::array<std::uint32_t, 10> h{296, 0, 8, request, 0, 5, 0, 0, 1, 1};
    std::memcpy(type_packet.data(), h.data(), sizeof(h));
    std::strcpy(reinterpret_cast<char*>(type_packet.data() + 40), text);
    return identity.accept(type_packet.data(), type_packet.size(), now);
  };
  const auto path = [&](std::uint32_t request, const char* text, std::uint64_t now) {
    path_packet = {};
    const std::array<std::uint32_t, 6> h{284, 0, 15, request, 0, 0};
    std::memcpy(path_packet.data(), h.data(), sizeof(h));
    std::strcpy(reinterpret_cast<char*>(path_packet.data() + 24), text);
    return identity.accept(path_packet.data(), path_packet.size(), now);
  };
  identity.begin_request(1000, 1001);
  assert(type(1000, fbw_type, 1000) && path(1001, fbw_path, 1001));
  assert(identity.sample(1001).detected_profile == 1);
  identity.begin_request(1002, 1003);
  assert(type(1002, "C172", 2000));
  assert(identity.sample(2000).detected_profile == 1);  // Last complete observation only.
  assert(!path(1001, fbw_path, 2001));                  // Delayed previous response.
  assert(path(1003, "SimObjects/Airplanes/Asobo_C172/aircraft.cfg", 2002));
  assert(!identity.sample(2002).detected_profile && identity.sample(2002).fresh);
  identity.begin_request(1004, 1005);
  assert(path(1005, fbw_path, 3000) && type(1004, fbw_type, 3001));
  assert(identity.sample(3001).detected_profile == 1);
  identity.begin_request(1006, 1007);
  assert(type(1006, "A35K", 4000));
  identity.begin_request(1008, 1009);  // Timed-out incomplete poll cannot mix with the next one.
  assert(!path(1007, "SimObjects/Airplanes/A350/presets/iniBuilds/A350-1000/config/aircraft.cfg", 4001));
  assert(type(1008, "A359", 4002) && path(1009, "SimObjects/Airplanes/A350/presets/iniBuilds/A350-900/config/aircraft.cfg", 4003));
  assert(identity.sample(4003).detected_profile == 2);
  identity = {};  // Manual adapter reconnect starts with no previous metadata.
  assert(!identity.sample(4004).fresh && !identity.sample(4004).detected_profile);

  native_camera::AircraftSessionLifecycle session;
  const auto sim = [&](std::uint32_t running) {
    const std::array<std::uint32_t, 6> h{24, 0, 4, 0, session.SimEvent, running};
    return session.accept(h.data(), sizeof(h));
  };
  assert(!sim(0) && session.epoch() == 0 && !session.running());
  assert(!sim(0) && session.epoch() == 0);
  assert(sim(1) && session.epoch() == 1 && session.running());
  assert(!sim(1) && session.epoch() == 1);  // Initial event after reconnect.
  assert(!sim(2) && session.epoch() == 1 && session.running());
  std::array<unsigned char, 288> event{};
  std::array<std::uint32_t, 6> h{288, 0, 6, 0, session.AircraftEvent, 0};
  std::memcpy(event.data(), h.data(), sizeof(h));
  std::strcpy(reinterpret_cast<char*>(event.data() + 24), fbw_path);
  assert(session.accept(event.data(), event.size()) && session.epoch() == 2);
  assert(session.accept(event.data(), event.size()) && session.epoch() == 3);  // Same aircraft reload.
  for (std::uint32_t size = 0; size < event.size(); ++size)
    assert(!session.accept(event.data(), size));
  std::memset(event.data() + 24, 'X', 260);
  assert(!session.accept(event.data(), event.size()) && session.epoch() == 3);
  event[24] = 0;
  h[4] = session.FlightEvent;
  std::memcpy(event.data(), h.data(), sizeof(h));
  assert(session.accept(event.data(), event.size()) && session.epoch() == 4);
  assert(sim(0) && session.epoch() == 5 && !session.running());
  assert(sim(1) && session.epoch() == 6 && session.running());
}
void guide_settings_tests() {
  using namespace standalone;
  std::array<Settings, profiles::Catalog.size()> saved{};
  for (const auto* profile : profiles::Catalog) {
    Settings defaults;
    defaults.profile = profile->id;
    defaults.mounts = profile->mounts;
    defaults.speed_color = profile->composition.speed_color;
    reset_guide_settings(defaults, *profile);
    assert(defaults.nose_dot == profile->composition.nose_dot && defaults.tail_upper == profile->composition.tail_upper &&
           defaults.tail_corner == profile->composition.tail_corner && defaults.tail_inner == profile->composition.tail_inner &&
           defaults.guide_color == profile->composition.guide_color);
    Settings edited = defaults;
    edited.nose_dot = {profile->id * 0.03125f, 0.25f};
    edited.tail_upper = {0.125f, 0.375f + profile->id * 0.03125f};
    edited.tail_corner = {0.21875f, 0.6875f};
    edited.tail_inner = {0.375f, 0.71875f + profile->id * 0.03125f};
    edited.mounts[0][1] += 0.125;
    edited.speed_color = {0.25f, 0.5f, 0.75f};
    edited.guide_color = {profile->id * 0.0625f, 0.125f, 0.875f};
    edited.exposure = -7.5f;
    assert(save_settings(edited));
    Settings loaded;
    assert(load_settings(loaded, L"missing", profile->id) && same_guides(loaded, edited));
    assert(loaded.mounts == edited.mounts && loaded.speed_color == edited.speed_color && loaded.exposure == edited.exposure);
    for (auto member : GuideMembers) {
      for (unsigned axis = 0; axis < 2; ++axis) {
        for (float value : {-0.001f, axis ? 1.001f : 0.501f, std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
          auto invalid = edited;
          (invalid.*member)[axis] = value;
          assert(!valid_settings(invalid) && !save_settings(invalid));
        }
        for (float value : {0.f, axis ? 1.f : 0.5f}) {
          auto edge = edited;
          (edge.*member)[axis] = value;
          assert(valid_settings(edge));
        }
      }
    }
    for (unsigned channel = 0; channel < 3; ++channel) {
      for (float value : {-0.001f, 1.001f, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) {
        auto invalid = edited;
        invalid.guide_color[channel] = value;
        assert(!valid_settings(invalid) && !save_settings(invalid));
      }
      for (float value : {0.f, 1.f}) {
        auto edge = edited;
        edge.guide_color[channel] = value;
        assert(valid_settings(edge));
      }
    }
    assert(load_settings(loaded, L"missing", profile->id) && same_guides(loaded, edited));
    auto reset = edited;
    reset_guide_settings(reset, *profile);
    assert(same_guides(reset, defaults) && reset.mounts == edited.mounts && reset.speed_color == edited.speed_color &&
           reset.exposure == edited.exposure && reset.profile == edited.profile);

    const auto path = settings_path(edited);
    constexpr const wchar_t* color_keys[]{L"guide_red", L"guide_green", L"guide_blue"};
    for (const auto* key : color_keys)
      assert(WritePrivateProfileStringW(L"guides", key, nullptr, path.c_str()));
    auto older = edited;
    older.guide_color = defaults.guide_color;
    assert(load_settings(loaded, L"missing", profile->id) && same_guides(loaded, older));
    assert(loaded.mounts == edited.mounts && loaded.speed_color == edited.speed_color);
    // Loading an older profile supplies the marking default without rewriting
    // its saved camera calibration, guide positions or settings format.
    for (const auto* key : color_keys) {
      wchar_t raw[32]{};
      GetPrivateProfileStringW(L"guides", key, L"missing", raw, 32, path.c_str());
      assert(std::wstring(raw) == L"missing");
    }
    assert(WritePrivateProfileStringW(L"guides", color_keys[0], L"0.625", path.c_str()));
    assert(WritePrivateProfileStringW(L"guides", color_keys[1], L"nan", path.c_str()));
    assert(WritePrivateProfileStringW(L"guides", color_keys[2], L"not-a-number", path.c_str()));
    older.guide_color[0] = 0.625f;
    assert(load_settings(loaded, L"missing", profile->id) && same_guides(loaded, older));
    for (const auto* key : color_keys) {
      assert(WritePrivateProfileStringW(L"guides", key, L"1.001", path.c_str()));
      loaded = edited;
      assert(!load_settings(loaded, L"missing", profile->id) && same_guides(loaded, edited) && loaded.mounts == edited.mounts);
      assert(WritePrivateProfileStringW(L"guides", key, nullptr, path.c_str()));
    }
    assert(WritePrivateProfileStringW(L"guides", nullptr, nullptr, path.c_str()));
    assert(load_settings(loaded, L"missing", profile->id) && same_guides(loaded, defaults));
    assert(loaded.mounts == edited.mounts && loaded.speed_color == edited.speed_color);
    // Partial older configuration: missing coordinates retain this profile's
    // defaults. Nonfinite/malformed INI text also uses the existing parser's
    // per-key fallback; finite out-of-range data refuses the entire load.
    assert(WritePrivateProfileStringW(L"guides", L"nose_dot_x", L"0.0625", path.c_str()));
    assert(WritePrivateProfileStringW(L"guides", L"tail_upper_y", L"nan", path.c_str()));
    assert(WritePrivateProfileStringW(L"guides", L"tail_inner_x", L"not-a-number", path.c_str()));
    assert(load_settings(loaded, L"missing", profile->id));
    auto partial = defaults;
    partial.nose_dot[0] = 0.0625f;
    assert(same_guides(loaded, partial));
    assert(WritePrivateProfileStringW(L"guides", L"tail_corner_x", L"0.75", path.c_str()));
    loaded = edited;
    assert(!load_settings(loaded, L"missing", profile->id) && same_guides(loaded, edited) && loaded.mounts == edited.mounts);
    assert(save_settings(edited));
    saved[profile->id - 1] = edited;
  }
  for (const auto& expected : saved) {
    Settings loaded;
    assert(load_settings(loaded, L"missing", expected.profile) && same_guides(loaded, expected));
    assert(loaded.mounts == expected.mounts && loaded.speed_color == expected.speed_color);
  }
  assert(settings_path(saved[0]) != settings_path(saved[1]) && settings_path(saved[1]) != settings_path(saved[2]));
}
}  // namespace
int main() {
  aircraft_swap_tests();
  using namespace standalone;
  wchar_t temporary[32768];
  assert(GetTempPathW(32768, temporary));
  settings_override = std::wstring(temporary) + L"TaxiCam-profile-test-" + std::to_wstring(GetCurrentProcessId());
  struct Cleanup {
    ~Cleanup() { std::filesystem::remove_all(settings_override); }
  } cleanup;
  Settings a380;
  a380.mounts[0][2] = 27.123;
  a380.exposure = -9.1f;
  assert(save_settings(a380));
  Settings a350;
  assert(load_settings(a350, L"missing", 2));
  assert(a350.profile == 2 && a350.mounts == profiles::A359.mounts);
  a350.mounts[1][3] = -19.125;
  a350.exposure = -7;
  a350.auto_profile = 0;
  a350.speed_color = {0.125f, 0.875f, 0.25f};
  assert(save_settings(a350));
  Settings loaded;
  assert(load_settings(loaded, L"missing") && loaded.profile == 2 && loaded.mounts == a350.mounts && loaded.exposure == -7);
  assert(loaded.speed_color == a350.speed_color && loaded.auto_profile == 0);
  auto invalid = loaded;
  invalid.speed_color[0] = 1.1f;
  assert(!valid_settings(invalid));
  assert(load_settings(loaded, L"missing", 1) && loaded.mounts == a380.mounts && loaded.exposure == a380.exposure);
  assert(settings_path(a380) != settings_path(a350));
  assert(profiles::matches_aircraft(profiles::A359, "A359 ULR"));
  assert(!profiles::matches_aircraft(profiles::A35K, "A359 ULR"));
  assert(!profiles::matches_aircraft(profiles::A359, ""));
  const char* ini_path = "SimObjects/airplanes/A350/presets/iniBuilds/A350-900_Default_Cabin_ULR/config/aircraft.cfg";
  assert(profiles::detect_aircraft("A359 ULR", ini_path) == 2);
  assert(profiles::detect_aircraft("A35K", ini_path) == 3);
  assert(profiles::detect_aircraft("A388", "D:\\Community\\flybywire-aircraft-a380-842\\aircraft.cfg") == 1);
  assert(!profiles::detect_aircraft("A359", "D:/Community/other-a350/aircraft.cfg"));
  assert(!profiles::detect_aircraft("A359", "D:/Community/not-inibuilds-aircraft-a350/aircraft.cfg"));
  assert(!profiles::detect_aircraft("A359", "D:/Community/inibuilds-aircraft-a350-copy/aircraft.cfg"));
  constexpr auto ini_a380_path = "SimObjects/Airplanes/inibuilds-a380/presets/inibuilds/a380-800_rr_basic/config/aircraft.CFG";
  assert(profiles::detect_aircraft("Airbus", ini_a380_path) == profiles::IniA380.id);
  assert(profiles::detect_aircraft("AIRBUS", "SimObjects\\Airplanes\\INIBUILDS-A380\\presets\\iniBuilds\\other\\aircraft.cfg") ==
         profiles::IniA380.id);
  assert(!profiles::detect_aircraft("Airbus", "SimObjects/Airplanes/inibuilds-a380-copy/aircraft.cfg"));
  assert(!profiles::detect_aircraft("Airbus", "SimObjects/Airplanes/Other_A380/presets/inibuilds/aircraft.cfg"));
  assert(!profiles::detect_aircraft("A359", ini_a380_path));
  assert(profiles::IniA380.taxi_control == profiles::TaxiControl::manual_only);
  for (const auto* pmdg_profile : {&profiles::Pmdg777, &profiles::Pmdg777300ER, &profiles::Pmdg777F}) {
    // The Display Select Panel is read only; Taxi Cam never commands it.
    assert(pmdg_profile->taxi_control == profiles::TaxiControl::pmdg_dsp_cam && !profiles::commandable_buttons(*pmdg_profile));
    assert(pmdg_profile->sides == 3 && profiles::side_mask(*pmdg_profile) == 7 && profiles::separate_lower_texture(*pmdg_profile));
    assert(profiles::shared_texture_sides(*pmdg_profile) == 3);
  }
  // The A340-600 SD shares its display texture.
  assert(!profiles::separate_lower_texture(profiles::AerosoftA346) && profiles::shared_texture_sides(profiles::AerosoftA346) == 7);
  for (const auto* other : {&profiles::A380, &profiles::A359, &profiles::A35K, &profiles::IniA380})
    assert(!profiles::separate_lower_texture(*other));
  assert(profiles::commandable_buttons(profiles::A380) && profiles::commandable_buttons(profiles::A359) &&
         profiles::commandable_buttons(profiles::AerosoftA346) && !profiles::commandable_buttons(profiles::IniA380) &&
         !profiles::commandable_buttons(profiles::IniA343));
  // The A340-300 SD shares the one display texture; its displays follow shortcuts only.
  assert(profiles::IniA343.taxi_control == profiles::TaxiControl::manual_only);
  assert(!profiles::separate_lower_texture(profiles::IniA343) && profiles::shared_texture_sides(profiles::IniA343) == 7);
  assert(profiles::detect_aircraft("ATCCOM.ATC_NAME BOEING.0.text",
                                   "SimObjects\\Airplanes\\PMDG 777-200ER\\presets\\pmdg\\PMDG 777-200ER RR\\config\\aircraft.CFG") ==
         profiles::Pmdg777.id);
  assert(profiles::detect_aircraft("777-200ER GE", "Community/pmdg-aircraft-77er/SimObjects/Airplanes/PMDG 777-200ER/aircraft.cfg") ==
         profiles::Pmdg777.id);
  assert(profiles::detect_aircraft("", "Community\\pmdg-aircraft-77w\\SimObjects\\Airplanes\\PMDG 777-300ER\\aircraft.cfg") ==
         profiles::Pmdg777300ER.id);
  assert(profiles::detect_aircraft("B77F", "Community/pmdg-aircraft-77f/SimObjects/Airplanes/PMDG 777F/aircraft.cfg") ==
         profiles::Pmdg777F.id);
  assert(profiles::Pmdg777300ER.mounts[0][0] == 0 && profiles::Pmdg777300ER.mounts[0][1] == -2 &&
         profiles::Pmdg777300ER.mounts[0][2] == 22 && profiles::Pmdg777300ER.mounts[0][3] == -18 &&
         profiles::Pmdg777300ER.mounts[0][4] == 0 && profiles::Pmdg777300ER.mounts[0][5] == 1);
  assert(profiles::Pmdg777300ER.mounts[0] != profiles::Pmdg777.mounts[0]);
  assert(profiles::Pmdg777F.mounts[0] == profiles::Pmdg777.mounts[0]);
  assert(profiles::Pmdg777300ER.key != profiles::Pmdg777.key && profiles::Pmdg777F.key != profiles::Pmdg777.key);
  assert(!profiles::detect_aircraft("ATCCOM.ATC_NAME BOEING.0.text", "Community/pmdg-aircraft-77er/aircraft.cfg"));
  assert(!profiles::detect_aircraft("ATCCOM.ATC_NAME BOEING.0.text",
                                    "SimObjects\\Airplanes\\Other\\presets\\pmdg\\PMDG 777-200ER RR\\config\\aircraft.CFG"));
  assert(!profiles::detect_aircraft("777", "Community/pmdg-aircraft-77er-copy/aircraft.cfg"));
  assert(!profiles::detect_aircraft("777", "SimObjects/Airplanes/PMDG 777-200ER-copy/aircraft.cfg"));
  assert(!profiles::detect_aircraft("777", "Community/not-pmdg-aircraft-77f/aircraft.cfg"));
  // aircraft.cfg atc_type is the Airbus brand string; the SimObject folder
  // identifies the Aerosoft product. The live AircraftLoaded path is not yet logged.
  assert(profiles::detect_aircraft("ATCCOM.ATC_NAME AIRBUS.0.text", "SimObjects\\Airplanes\\airbus-a346-pro\\aircraft.CFG") ==
         profiles::AerosoftA346.id);
  assert(profiles::detect_aircraft("", "Community/aerosoft-aircraft-a346-pro/SimObjects/Airplanes/airbus-a346-pro/aircraft.cfg") ==
         profiles::AerosoftA346.id);
  assert(!profiles::detect_aircraft("ATCCOM.ATC_NAME AIRBUS.0.text", "SimObjects/Airplanes/airbus-a346-pro-copy/aircraft.cfg"));
  assert(!profiles::detect_aircraft("A346", "Community/aerosoft-aircraft-a346-pro_CVT_/aircraft.cfg"));
  assert(!profiles::detect_aircraft("A346", "SimObjects/Airplanes/Other_A346/aircraft.cfg"));
  // Live iniBuilds A340-300 identity: ATC TYPE Airbus and this SimObject path.
  constexpr auto ini_a343_path = "SimObjects\\Airplanes\\inibuilds-a340\\presets\\inibuilds\\a340-300_eis2\\config\\aircraft.CFG";
  assert(profiles::detect_aircraft("Airbus", ini_a343_path) == profiles::IniA343.id);
  assert(profiles::detect_aircraft("", "StreamedPackages/fs24-inibuilds-aircraft-a340/SimObjects/Airplanes/other/aircraft.cfg") ==
         profiles::IniA343.id);
  assert(!profiles::detect_aircraft("Airbus", "SimObjects/Airplanes/inibuilds-a340-copy/aircraft.cfg"));
  assert(!profiles::detect_aircraft("A346", "SimObjects/Airplanes/inibuilds-a340x/aircraft.cfg"));
  assert(profiles::detect_aircraft(
             "ATCCOM.ATC_NAME AIRBUS.0.text",
             "SimObjects\\Airplanes\\FlyByWire_A380X\\presets\\flybywire\\FlyByWire_A380_842\\config\\aircraft.CFG") == 1);
  Settings pmdg;
  assert(load_settings(pmdg, L"missing", profiles::Pmdg777.id));
  // CAM-button control is on by default.
  assert(pmdg.profile == profiles::Pmdg777.id && pmdg.follow_taxi && pmdg.mounts == profiles::Pmdg777.mounts);
  assert(settings_path(pmdg) != settings_path(a380));
  Settings pmdg_300, pmdg_f;
  assert(load_settings(pmdg_300, L"missing", profiles::Pmdg777300ER.id));
  assert(load_settings(pmdg_f, L"missing", profiles::Pmdg777F.id));
  assert(pmdg_300.mounts[0][2] == 22 && pmdg_f.mounts[0] == profiles::Pmdg777.mounts[0]);
  assert(pmdg_300.exposure == -8.f && pmdg_f.exposure == -8.f && pmdg.exposure == -8.f);
  assert(settings_path(pmdg_300) != settings_path(pmdg) && settings_path(pmdg_f) != settings_path(pmdg));
  assert(settings_path(pmdg_300) != settings_path(pmdg_f));
  {
    // Profiles saved while the 777 was manual-only turn CAM control on once.
    const auto pmdg_path = settings_path(pmdg_f);
    assert(WritePrivateProfileStringW(L"service", L"follow_taxi", L"0", pmdg_path.c_str()));
    assert(load_settings(pmdg_f, L"missing", profiles::Pmdg777F.id) && pmdg_f.follow_taxi == 1);
    assert(WritePrivateProfileStringW(L"service", L"cam_button_revision", L"1", pmdg_path.c_str()));
    assert(load_settings(pmdg_f, L"missing", profiles::Pmdg777F.id) && pmdg_f.follow_taxi == 0);
    assert(save_settings(pmdg_f));
    assert(GetPrivateProfileIntW(L"service", L"cam_button_revision", 0, pmdg_path.c_str()) == 1);
    assert(load_settings(pmdg_f, L"missing", profiles::Pmdg777F.id) && pmdg_f.follow_taxi == 0);
  }
  Settings a346;
  assert(load_settings(a346, L"missing", profiles::AerosoftA346.id));
  assert(a346.profile == profiles::AerosoftA346.id && a346.follow_taxi && a346.mounts == profiles::AerosoftA346.mounts);
  assert(a346.exposure == -8.f && a346.tail_corner == profiles::AerosoftA346.composition.tail_corner);
  assert((a346.tail_upper == std::array<float, 2>{0.28f, 0.72f} && a346.tail_corner == std::array<float, 2>{0.24f, 0.86f} &&
          a346.tail_inner == std::array<float, 2>{0.31f, 0.86f} && a346.nose_dot == profiles::A380.composition.nose_dot));
  assert(a346.mounts[0][1] == -2.5 && a346.mounts[0][2] == 23.33);
  assert(settings_path(a346) != settings_path(pmdg) && settings_path(a346) != settings_path(a380));
  assert(profiles::matches_display(profiles::IniA380, 768, 1024, 1, 27));
  assert(profiles::matches_display(profiles::IniA380, 768, 1024, 1, 28));
  assert(profiles::matches_display(profiles::IniA380, 768, 1024, 1, 87));
  assert(!profiles::matches_display(profiles::IniA380, 768, 1024, 5, 28));
  assert(!profiles::matches_display(profiles::IniA380, 768, 1024, 12, 87));
  assert(profiles::matches_display(profiles::A380, 768, 1024, 5, 28));
  assert(!profiles::matches_display(profiles::A380, 768, 1024, 1, 28));
  assert(!profiles::matches_display(profiles::IniA380, 768, 1024, 0, 28));
  assert(!profiles::matches_display(profiles::IniA380, 768, 1024, 13, 28));
  assert(!profiles::matches_display(profiles::IniA380, 1644, 1024, 1, 28));
  assert(!profiles::matches_display(profiles::IniA380, 768, 1024, 1, 10));
  Settings ini_a380;
  assert(load_settings(ini_a380, L"missing", profiles::IniA380.id));
  assert(ini_a380.profile == profiles::IniA380.id && !ini_a380.follow_taxi && !ini_a380.manual_mask && !ini_a380.calibration_mask);
  assert(ini_a380.mounts == profiles::IniA380.mounts && ini_a380.mounts != a380.mounts);
  assert(settings_path(ini_a380) != settings_path(a380));
  assert(settings_path(pmdg) != settings_path(ini_a380));
  assert(ini_a380.exposure == -8.f);
  // A partial saved profile inherits its own exposure; explicit calibration
  // continues to override the shipped default.
  const auto ini_path_settings = settings_path(ini_a380);
  assert(WritePrivateProfileStringW(L"service", L"enabled", L"1", ini_path_settings.c_str()));
  assert(load_settings(ini_a380, L"missing", profiles::IniA380.id) && ini_a380.exposure == -8.f);
  assert(WritePrivateProfileStringW(L"display", L"exposure", L"-10.25", ini_path_settings.c_str()));
  assert(load_settings(ini_a380, L"missing", profiles::IniA380.id) && ini_a380.exposure == -10.25f);
  native_camera::AircraftIdentityCache identity;
  std::array<unsigned char, 296> packet{};
  std::array<std::uint32_t, 10> h{296, 0, 8, 5, 0, 5, 0, 0, 1, 1};
  std::memcpy(packet.data(), h.data(), 40);
  std::strcpy(reinterpret_cast<char*>(packet.data() + 40), "A359 ULR");
  assert(identity.accept(packet.data(), packet.size(), 1000));
  assert(!identity.sample(1000).detected_profile);
  for (unsigned n = 0; n < packet.size(); ++n)
    assert(!identity.accept(packet.data(), n, 1000));
  auto malformed = packet;
  malformed[296 - 1] = 1;
  std::memset(malformed.data() + 40, 'X', 256);
  assert(!identity.accept(malformed.data(), malformed.size(), 1000));
  std::array<unsigned char, 284> path_packet{};
  std::array<std::uint32_t, 6> ph{284, 0, 15, 6, 0, 0};
  std::memcpy(path_packet.data(), ph.data(), 24);
  std::strcpy(reinterpret_cast<char*>(path_packet.data() + 24), ini_path);
  assert(identity.accept(path_packet.data(), path_packet.size(), 1100));
  assert(identity.sample(1100).detected_profile == 2 && identity.sample(1100).fresh);
  assert(identity.sample(4001).detected_profile == 2 && !identity.sample(4001).fresh);
  assert(!identity.sample(999).fresh);
  native_camera::AutoProfileSelection selection;
  assert(!selection.observe(2, 1000));
  assert(!selection.observe(2, 1000));
  assert(selection.observe(2, 2000) == 2);
  assert(!selection.observe(3, 3000));
  assert(!selection.observe(0, 0));
  assert(!selection.observe(3, 4000));
  assert(selection.observe(3, 5000) == 3);
  for (const auto* profile : profiles::Catalog) {
    Settings s;
    s.profile = profile->id;
    s.mounts = profile->mounts;
    assert(valid_settings(s));
    const bool pmdg = std::strcmp(profile->display_texture, profiles::Pmdg777Texture) == 0;
    assert(pmdg != profile->reference_guides);
    assert(pmdg != profile->ground_speed);
    // Every profile shows the PLEASE WAIT page; only the PMDG ND draws it white.
    assert(pmdg == profile->waiting_white_text);
    // A scanned texture name implies the single-display rule. The iniBuilds
    // A340-300 texture is identified by shape only (its package is encrypted).
    assert(!profile->display_texture[0] || profile->pfd_detection == profiles::PfdDetectionPolicy::single_display);
    if (profile->id == profiles::IniA343.id) {
      assert(profile->pfd_detection == profiles::PfdDetectionPolicy::single_display && profile->display_texture[0] == '\0');
      assert(profile->width == 1560 && profile->height == 2340 && profile->mips == 1 && profile->sides == 3);
      // 2 x 3 grid of 780 x 780 cells: CAPT PFD (0,0), SD (1,1), F/O PFD (1,2).
      const auto left = profiles::display_rect(*profile, 0), right = profiles::display_rect(*profile, 1),
                 sd = profiles::display_rect(*profile, 2);
      assert(left.left == 0 && left.top == 0 && left.right == 780 && left.bottom == 780);
      assert(right.left == 780 && right.top == 1560 && right.right == 1560 && right.bottom == 2340);
      assert(sd.left == 780 && sd.top == 780 && sd.right == 1560 && sd.bottom == 1560);
      for (unsigned side = 0; side < 3; ++side) {
        const auto content = profiles::display_content_rect(*profile, side);
        assert(content.right - content.left == 748 && content.bottom - content.top == 768);
      }
      // The scanned display is one-mip R8G8B8A8_TYPELESS; typed views are admitted
      // for patch output. The five-mip 2340 x 2340 texture beside it is not.
      assert(profiles::matches_display(*profile, 1560, 2340, 1, 27) && profiles::matches_display(*profile, 1560, 2340, 1, 28));
      assert(!profiles::matches_display(*profile, 1560, 2340, 5, 27) && !profiles::matches_display(*profile, 2340, 2340, 5, 28));
      assert(!profiles::matches_display(*profile, 1560, 2340, 1, 10));
      assert(profile->composition.tail_corner == profiles::AerosoftA346.composition.tail_corner);
      assert(profile->mounts == profiles::A359.mounts);
      continue;
    }
    if (profile->id == profiles::AerosoftA346.id) {
      assert(profile->pfd_detection == profiles::PfdDetectionPolicy::single_display);
      assert(std::strcmp(profile->display_texture, "$GAUGES_UNIFIED") == 0);
      assert(profile->width == 4096 && profile->height == 4096 && profile->mips == 0 && profile->formats[0] == 0);
      assert(profile->taxi_control == profiles::TaxiControl::lvar_off);
      assert(std::strcmp(profile->taxi_lvars[0], "L:AB_VC_CAM_CAPT_SEL") == 0);
      assert(std::strcmp(profile->taxi_lvars[1], "L:AB_VC_CAM_FO_SEL") == 0);
      assert(profile->taxi_events[0][0] == '\0' && profile->taxi_events[1][0] == '\0');
      assert(std::strcmp(profile->pfd_labels[0], "CaptPFD") == 0 && std::strcmp(profile->pfd_labels[1], "CoPFD") == 0);
      // CAM SD drives the lower ECAM as a third side: ECAM_LOWER 1529,1230,750,750.
      assert(profile->sides == 3 && profiles::side_mask(*profile) == 7);
      assert(std::strcmp(profile->taxi_lvars[2], "L:AB_VC_CAM_SD_SEL") == 0 && std::strcmp(profile->pfd_labels[2], "ECAM_LOWER") == 0);
      const auto sd = profiles::display_rect(*profile, 2), sd_content = profiles::display_content_rect(*profile, 2);
      assert(sd.left == 1529 && sd.top == 1230 && sd.right == 2279 && sd.bottom == 1980);
      assert(sd_content.right - sd_content.left == 718 && sd_content.bottom - sd_content.top == 738);
      // panel.cfg CaptPFD 9,470,750,750 and CoPFD 9,1230,750,750, both live-verified.
      const auto left = profiles::display_rect(*profile, 0), right = profiles::display_rect(*profile, 1);
      assert(left.left == 9 && left.top == 470 && left.right == 759 && left.bottom == 1220);
      assert(right.left == 9 && right.top == 1230 && right.right == 759 && right.bottom == 1980);
      for (unsigned side = 0; side < 2; ++side) {
        const auto outer = profiles::display_rect(*profile, side);
        const auto content = profiles::display_content_rect(*profile, side);
        assert(content.left == outer.left + 16 && content.right + 16 == outer.right && content.top == outer.top + 12 &&
               content.bottom == outer.bottom);
        assert(content.right - content.left == 718 && content.bottom - content.top == 738);
        // A380 source panes; the compositor scales them into the smaller ND.
        assert(profile->camera_panes[side][0] == 736 && profile->camera_panes[side][1] == (side == 0 ? 251 : 496));
        native_camera::ViewDimensions desired, original{{{3413, 913}, {3413, 913}, {3413, 913}}};
        assert(native_camera::plan_view_resize(original, side, desired, profile->camera_panes));
        assert(desired[0] == profile->camera_panes[side] && desired[1] == desired[0] && desired[2] == desired[0]);
        assert(profiles::camera_candidate(desired[0][0], desired[0][1]));
      }
      assert(profile->composition.split_bottom == 0);
      assert(profiles::matches_display(*profile, 4096, 4096, 1, 28));
      assert(profiles::matches_display(*profile, 4096, 4096, 12, 87));
      assert(!profiles::matches_display(*profile, 4096, 4096, 0, 28));
      assert(!profiles::matches_display(*profile, 4096, 4096, 1, 0));
      assert(!profiles::matches_display(*profile, 2048, 2048, 1, 28));
      continue;
    }
    if (profile->pfd_detection == profiles::PfdDetectionPolicy::single_display) {
      const auto left = profiles::display_rect(*profile, 0);
      const auto right = profiles::display_rect(*profile, 1);
      assert(profile->width == 2048 && profile->height == 2048 && profile->mips == 0);
      assert(left.left == profiles::Pmdg777LeftNdX && left.top == profiles::Pmdg777LeftNdY);
      assert(left.right == profiles::Pmdg777LeftNdX + profiles::Pmdg777NdWidth &&
             left.bottom == profiles::Pmdg777LeftNdY + profiles::Pmdg777NdHeight);
      assert(right.left == profiles::Pmdg777RightNdX && right.top == profiles::Pmdg777RightNdY);
      assert(right.right == profiles::Pmdg777RightNdX + profiles::Pmdg777NdWidth &&
             right.bottom == profiles::Pmdg777RightNdY + profiles::Pmdg777NdHeight);
      assert(left.right <= profile->width && right.bottom <= profile->height);
      // Lower DU: DU_Lower on the separate EICASCDU texture ([VCockpit02]).
      const auto lower = profiles::display_rect(*profile, 2);
      assert(lower.left == 1058 && lower.top == 21 && lower.right == 1058 + 958 && lower.bottom == 21 + 971);
      assert(std::strcmp(profile->lower_display_texture, "EICASCDU") == 0 && std::strcmp(profile->display_texture, "DUS") == 0);
      for (unsigned side = 0; side < 3; ++side) {
        const auto outer = profiles::display_rect(*profile, side);
        const auto content = profiles::display_content_rect(*profile, side);
        assert(content.left == outer.left && content.right == outer.right);
        assert(content.top == outer.top + 85 && content.bottom == outer.bottom);
        assert(outer.right - outer.left == 958 && outer.bottom - outer.top == 971);
        assert(content.bottom - content.top == 886);
      }
      assert(profile->camera_panes[0][0] == 736 && profile->camera_panes[0][1] == 268);
      assert(profile->camera_panes[1][0] == 360 && profile->camera_panes[1][1] == 360);
      assert(profile->camera_panes[2][0] == 360 && profile->camera_panes[2][1] == 360);
      assert(profile->composition.split_bottom == 1.f && profile->composition.bottom_gap == 48.f);
      assert(profile->composition.nose_height == 280.f && profile->composition.tail_top == 318.f);
      assert(profile->composition.divider_top == 280.f && profile->composition.divider_bottom == 318.f);
      assert(profile->camera_padding.left == 0 && profile->camera_padding.top == 85 && profile->camera_padding.right == 0 &&
             profile->camera_padding.bottom == 0);
      if (profile->id == profiles::Pmdg777300ER.id) {
        assert(profile->mounts[0][0] == 0 && profile->mounts[0][1] == -2 && profile->mounts[0][2] == 22);
        assert(profile->mounts[0][3] == -18 && profile->mounts[0][4] == 0 && profile->mounts[0][5] == 1);
      } else {
        assert(profile->mounts[0][0] == 0 && profile->mounts[0][1] == -2 && profile->mounts[0][2] == 16);
        assert(profile->mounts[0][3] == -18 && profile->mounts[0][4] == 0 && profile->mounts[0][5] == 1);
      }
      assert(profile->mounts[1][0] == -6 && profile->mounts[1][1] == 1.5 && profile->mounts[1][2] == -28);
      assert(profile->mounts[1][3] == -5 && profile->mounts[1][4] == -12 && profile->mounts[1][5] == 0.6);
      assert(profile->mounts[2][0] == 6 && profile->mounts[2][1] == 1.5 && profile->mounts[2][2] == -28);
      assert(profile->mounts[2][3] == -5 && profile->mounts[2][4] == 12 && profile->mounts[2][5] == 0.6);
      assert(profile->formats[0] == 0);
      assert(profiles::matches_display(*profile, profile->width, profile->height, 1, 28));
      assert(profiles::matches_display(*profile, profile->width, profile->height, 12, 87));
      assert(!profiles::matches_display(*profile, profile->width, profile->height, 1, 0));
      assert(!profiles::matches_display(*profile, profile->width, profile->height, 0, 28));
      assert(!profiles::matches_display(*profile, profile->width, profile->height, 13, 28));
      assert(!profiles::matches_display(*profile, 768, 1024, 1, 28));
      assert(profile->taxi_lvars[0][0] == '\0' && profile->taxi_events[0][0] == '\0');
      assert(std::strcmp(profile->display_texture, "DUS") == 0);
      assert(std::strcmp(profile->pfd_labels[0], "DU_LeftInboard") == 0);
      assert(std::strcmp(profile->pfd_labels[1], "DU_RightInboard") == 0);
      assert(!profile->reference_guides);
      assert(!profile->ground_speed);
      for (unsigned side = 0; side < 3; ++side) {
        native_camera::ViewDimensions desired, original{{{3413, 913}, {3413, 913}, {3413, 913}}};
        assert(native_camera::plan_view_resize(original, side, desired, profile->camera_panes));
        assert(desired[0] == profile->camera_panes[side] && desired[1] == desired[0] && desired[2] == desired[0]);
        assert(profiles::camera_candidate(desired[0][0], desired[0][1]));
      }
      continue;
    }
    for (unsigned side = 0; side < 2; ++side) {
      const auto rect = profiles::display_rect(*profile, side);
      assert(rect.left < rect.right && rect.right <= profile->width && rect.bottom <= profile->height);
      const auto content = profiles::display_content_rect(*profile, side);
      assert(content.left == rect.left + 16 && content.right + 16 == rect.right && content.top == rect.top + 12 &&
             content.bottom == rect.bottom);
      assert(rect.top == 0 && rect.bottom == 763);
      assert(content.left < content.right && content.top < content.bottom && content.left >= rect.left && content.top >= rect.top &&
             content.right <= rect.right && content.bottom <= rect.bottom);
      assert(content.bottom - content.top == 751);
      const int expected_width = profile->width == 768 ? 736 : 774;
      assert(content.right - content.left == static_cast<unsigned>(expected_width));
      assert(profile->camera_panes[side][0] == expected_width && profile->camera_panes[side][1] == (side == 0 ? 251 : 496));
      native_camera::ViewDimensions desired, original{{{3413, 913}, {3413, 913}, {3413, 913}}};
      assert(native_camera::plan_view_resize(original, side, desired, profile->camera_panes));
      assert(desired[0] == profile->camera_panes[side] && desired[1] == desired[0] && desired[2] == desired[0]);
      assert(profiles::camera_candidate(desired[0][0], desired[0][1]));
    }
  }
  for (unsigned side = 0; side < 2; ++side) {
    const auto outer = profiles::display_rect(profiles::A380, side);
    assert(outer.left == 0 && outer.right == 768 && outer.top == 0 && outer.bottom == 763);
    const auto content = profiles::display_content_rect(profiles::A380, side);
    assert(content.left == 16 && content.right == 752 && content.top == 12 && content.bottom == 763);
  }
  for (const auto* profile : {&profiles::A359, &profiles::A35K}) {
    assert(profile->composition.guide_color == profiles::A380.composition.guide_color && profile->composition.square_nose_markers);
    const auto left = profiles::display_rect(*profile, 0), right = profiles::display_rect(*profile, 1);
    assert(left.left == 0 && left.right == 806 && right.left == 838 && right.right == 1644);
    assert(right.left - left.right == 32);
    const auto left_content = profiles::display_content_rect(*profile, 0), right_content = profiles::display_content_rect(*profile, 1);
    assert(left_content.left == 16 && left_content.right == 790 && right_content.left == 854 && right_content.right == 1628);
    assert(left_content.top == 12 && right_content.top == 12 && left_content.bottom == 763 && right_content.bottom == 763);
    for (const auto& pane : profile->camera_panes)
      assert(pane[0] == 774);
    // iniBuilds 1.2.6 variant flight_model.cfg contact_points are feet:
    // NLG forward/up 79.46/-15.6 (-900), 92.045/-15.5 (-1000);
    // left MLG right/up/forward -20.200737/-16.45/-17.63 for both.
    const bool longer = profile->id == profiles::A35K.id;
    const auto nose = project_landmark(*profile, 0, {0, (longer ? -15.5 : -15.6) * 0.3048, (longer ? 92.045 : 79.46) * 0.3048});
    // Preserve the user's accepted nose calibration on both variants, with
    // the -1000's forward offset keeping the equivalent gear-relative mount.
    assert(profile->mounts[0][0] == 0 && profile->mounts[0][1] == -2 && profile->mounts[0][2] == (longer ? 19.81 : 16));
    assert(profile->mounts[0][3] == -15 && profile->mounts[0][4] == 0 && profile->mounts[0][5] == 0.55);
    assert(std::abs(nose[0] - 0.5) < 1e-6 && nose[1] > 0.55 && nose[1] < 0.75);
    const auto gear = project_landmark(*profile, 1, {-20.200737 * 0.3048, -16.45 * 0.3048, -17.63 * 0.3048});
    assert(gear[0] > 0.28 && gear[0] < 0.34 && gear[1] > (longer ? 0.77 : 0.85) && gear[1] < (longer ? 0.84 : 0.90));
    const auto& tail_mount = profile->mounts[1];
    assert(tail_mount[0] == 0 && tail_mount[1] == 10 && tail_mount[2] == (longer ? -36.17 : -33));
    assert(tail_mount[3] == -15 && tail_mount[4] == 0 && tail_mount[5] == 0.62);
  }
  // Guide bounds and persistence are tested separately. Ground-contact point
  // projections do not establish visible tyre alignment for these overlays.

  // Aerosoft A346 Pro 1.0.1 flight_model.cfg contact points are feet: NLG
  // forward/up 103.51/-21.4; left MLG right/up/forward -17.5/-22.4/-4.46.
  // The live-calibrated nose camera sits 1.27 m higher than the A350-derived
  // start, so the nose gear projects lower in its pane.
  {
    const auto& a346 = profiles::AerosoftA346;
    const auto nose = project_landmark(a346, 0, {0, -21.4 * 0.3048, 103.51 * 0.3048});
    assert(std::abs(nose[0] - 0.5) < 1e-6 && nose[1] > 0.80 && nose[1] < 0.90);
    const auto gear = project_landmark(a346, 1, {-17.5 * 0.3048, -22.4 * 0.3048, -4.46 * 0.3048});
    assert(gear[0] > 0.28 && gear[0] < 0.34 && gear[1] > 0.85 && gear[1] < 0.90);
    // The calibrated bracket corner sits outboard of the projected tyre contact.
    assert(a346.composition.tail_corner[0] < gear[0] && std::abs(a346.composition.tail_corner[1] - gear[1]) < 0.03);
    assert(a346.mounts[1] == a346.mounts[2]);
  }

  for (const auto* profile : profiles::Catalog) {
    if (std::strcmp(profile->display_texture, profiles::Pmdg777Texture) == 0) {
      assert(profile->composition.divider_top == 280 && profile->composition.divider_bottom == 318);
      assert(profile->composition.nose_height == 280 && profile->composition.tail_top == 318);
      continue;
    }
    assert(profile->composition.divider_top == 251 && profile->composition.divider_bottom == 263);
    assert(profile->composition.nose_height == 255 && profile->composition.tail_top == 259);
  }
  static PfdTargetDetector detector;
  detector.configure(profiles::A359);
  std::array<PfdTargetObservation, 4> observations{
      {{10, 0, 1644, 1024, 1, 28}, {20, 0, 1644, 1024, 1, 28}, {30, 0, 1644, 1024, 1, 28}, {40, 0, 768, 1024, 5, 28}}};
  detector.observe(observations.data(), observations.size(), 0);
  for (unsigned i = 1; i <= 3; ++i) {
    observations[0].draws += 100;
    observations[1].draws += 100;
    observations[2].draws += 20;
    observations[3].draws += 1000;
    detector.observe(observations.data(), observations.size(), i * 1000);
  }
  assert(detector.snapshot().valid && detector.snapshot().targets[0] == 20 && detector.snapshot().targets[1] == 10);
  detector.configure(profiles::A380);
  assert(!detector.snapshot().valid);
  assert(!detector.observe(observations.data(), observations.size(), 4000).valid);
  assert(!profiles::matches_display(profiles::A359, 1644, 1024, 0, 28));
  assert(!profiles::matches_display(profiles::A359, 1644, 1024, 1, 0));
  guide_settings_tests();
  std::puts(
      "PASS aircraft profiles: isolated settings, variant identity, pane dimensions, mirrored display regions, "
      "mount geometry, guide position/colour roundtrip/bounds/defaults/isolation and detector reset");
}
