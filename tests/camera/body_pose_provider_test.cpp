// Explicitly invoked live read-only provider validation; no camera acquisition.
#define WIN32_LEAN_AND_MEAN
#include "../../src/camera/body_pose_provider.hpp"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include "../../src/camera/body_pose_math.hpp"
using namespace taxi_camera::native_camera;
namespace {
#ifdef TAXI_BODY_POSE_PROVIDER_TESTING
int offline_tests() {
  unsigned checks = 0;
  const auto check = [&](bool good) {
    ++checks;
    if (!good) {
      std::fprintf(stderr, "Provider regression failed at check %u\n", checks);
      std::exit(1);
    }
  };
  namespace testing = body_pose_provider_testing;
  std::array<unsigned char, 96> packet{};
  const std::array<DWORD, 10> header{96, 0, 8, 1, 0, 1, 0, 0, 1, 7};
  std::array<double, 7> values{51.0, -0.1, 123.0, 2.0, -1.0, 270.0, 17.123456789123};
  const auto refresh = [&]() {
    std::memcpy(packet.data(), header.data(), sizeof(header));
    std::memcpy(packet.data() + 40, values.data(), sizeof(values));
  };
  shutdown_body_pose_provider();
  check(!get_ground_speed().valid && std::isnan(get_ground_speed().knots));
  refresh();
  check(testing::accept_aircraft_packet(packet.data(), packet.size(), 10000));
  auto speed = testing::ground_speed_at(10000);
  check(speed.valid && speed.knots == values[6] && speed.sample_ms == 10000 && !*speed.error);
  check(testing::ground_speed_at(10500).valid);
  speed = testing::ground_speed_at(10501);
  check(!speed.valid && std::isnan(speed.knots) && std::strcmp(speed.error, "ground_speed_telemetry_stale") == 0);
  check(!testing::ground_speed_at(9999).valid);
  reset_body_pose_calibration();
  check(testing::ground_speed_at(10000).valid);
  check(!sample_body_pose(GetTickCount64()).valid);
  for (DWORD bytes = 0; bytes < packet.size(); ++bytes)
    check(!testing::accept_aircraft_packet(packet.data(), bytes, 10000));
  check(!testing::accept_aircraft_packet(nullptr, packet.size(), 10000));
  check(!testing::accept_aircraft_packet(packet.data(), 65537, 10000));
  check(!testing::accept_aircraft_packet(packet.data(), packet.size(), 0));
  // Reject old six-double payloads and wrong request/definition/flags.
  for (unsigned field : {0u, 2u, 3u, 5u, 6u, 9u}) {
    refresh();
    const DWORD wrong = header[field] + 1;
    std::memcpy(packet.data() + field * 4, &wrong, 4);
    check(!testing::accept_aircraft_packet(packet.data(), packet.size(), 10000));
    check(testing::ground_speed_at(10000).knots == values[6]);
  }
  // USER(0) is only the request alias. The live server returned2883584.
  for (DWORD object_id : {1ul, 2883584ul, 12345678ul}) {
    refresh();
    std::memcpy(packet.data() + 16, &object_id, 4);
    check(testing::accept_aircraft_packet(packet.data(), packet.size(), 10000));
    check(testing::ground_speed_at(10000).knots == values[6]);
  }
  refresh();
  const DWORD old_bytes = 88, old_count = 6;
  std::memcpy(packet.data(), &old_bytes, 4);
  std::memcpy(packet.data() + 36, &old_count, 4);
  check(!testing::accept_aircraft_packet(packet.data(), packet.size(), 10000));
  for (double bad : {-1.0, 2000.01, std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN()}) {
    values[6] = bad;
    refresh();
    check(testing::accept_aircraft_packet(packet.data(), packet.size(), 10000));
    speed = testing::ground_speed_at(10000);
    check(!speed.valid && std::isnan(speed.knots) && std::strcmp(speed.error, "ground_speed_values") == 0);
  }
  for (double good : {0.0, 0.125, 15.5, 2000.0}) {
    values[6] = good;
    refresh();
    check(testing::accept_aircraft_packet(packet.data(), packet.size(), 10000));
    speed = testing::ground_speed_at(10000);
    check(speed.valid && speed.knots == good);
  }
  // Sub-50ms successive frame arrivals update immediately; the diagnostic
  // counts packets even when dispatch delivers two in the same millisecond.
  const auto initial_count = get_body_telemetry_timing().accepted_samples;
  std::uint64_t previous_frame_ms = 20000;
  unsigned frame_count = 0;
  for (const std::uint64_t arrival : {20000ull, 20008ull, 20016ull, 20016ull, 20025ull, 20033ull}) {
    values[6] = 10.0 + ++frame_count;
    refresh();
    check(testing::accept_aircraft_packet(packet.data(), packet.size(), arrival));
    speed = testing::ground_speed_at(arrival);
    check(speed.valid && speed.sample_ms == arrival && speed.knots == values[6]);
    const auto timing = get_body_telemetry_timing();
    check(timing.accepted_samples == initial_count + frame_count && timing.last_sample_ms == arrival);
    if (frame_count > 1)
      check(timing.last_interval_ms == arrival - previous_frame_ms);
    previous_frame_ms = arrival;
  }
  check(testing::ground_speed_at(20533).valid && !testing::ground_speed_at(20534).valid);
  const auto before_malformed = get_body_telemetry_timing().accepted_samples;
  check(!testing::accept_aircraft_packet(packet.data(), 40, 20034));
  check(get_body_telemetry_timing().accepted_samples == before_malformed);
  std::array<unsigned char, 56> taxi_packet{};
  const std::array<DWORD, 10> taxi_header{56, 0, 8, 3, 2883584, 3, 0, 0, 1, 2};
  std::array<double, 2> taxi_values{0, 0};
  const auto refresh_taxi = [&]() {
    std::memcpy(taxi_packet.data(), taxi_header.data(), sizeof(taxi_header));
    std::memcpy(taxi_packet.data() + 40, taxi_values.data(), sizeof(taxi_values));
  };
  check(!get_taxi_buttons().valid);
  for (unsigned mask = 0; mask < 4; ++mask) {
    taxi_values = {static_cast<double>(mask & 1), static_cast<double>((mask >> 1) & 1)};
    refresh_taxi();
    check(testing::accept_taxi_packet(taxi_packet.data(), taxi_packet.size(), 10000));
    const auto taxi = testing::taxi_buttons_at(10000);
    check(taxi.valid && taxi.left_on == ((mask & 1) != 0) && taxi.right_on == ((mask & 2) != 0));
    check(taxi.sample_ms == 10000 && !*taxi.error);
  }
  check(testing::taxi_buttons_at(10500).valid);
  check(!testing::taxi_buttons_at(10501).valid);
  check(std::strcmp(testing::taxi_buttons_at(10501).error, "taxi_telemetry_stale") == 0);
  check(!testing::taxi_buttons_at(9999).valid);
  for (DWORD bytes = 0; bytes < taxi_packet.size(); ++bytes)
    check(!testing::accept_taxi_packet(taxi_packet.data(), bytes, 10000));
  check(!testing::accept_taxi_packet(taxi_packet.data(), 57, 10000));
  check(!testing::accept_taxi_packet(taxi_packet.data(), 65537, 10000));
  check(!testing::accept_taxi_packet(nullptr, 56, 10000));
  check(!testing::accept_taxi_packet(taxi_packet.data(), 56, 0));
  for (unsigned field : {0u, 2u, 3u, 5u, 6u, 9u}) {
    refresh_taxi();
    const DWORD wrong = taxi_header[field] + 1;
    std::memcpy(taxi_packet.data() + field * 4, &wrong, 4);
    check(!testing::accept_taxi_packet(taxi_packet.data(), 56, 10000));
    check(testing::taxi_buttons_at(10000).left_on && testing::taxi_buttons_at(10000).right_on);
  }
  for (DWORD object_id : {0ul, 1ul, 2883584ul, 0xfffffffful}) {
    refresh_taxi();
    std::memcpy(taxi_packet.data() + 16, &object_id, 4);
    check(testing::accept_taxi_packet(taxi_packet.data(), 56, 10000));
  }
  // Invalid TAXI values invalidate only the independent button cache. A
  // malformed/wrong-definition packet cannot be mistaken for body telemetry.
  values[6] = 12.75;
  refresh();
  const auto now = GetTickCount64();
  check(testing::accept_aircraft_packet(packet.data(), packet.size(), now));
  const auto before_taxi = sample_body_pose(now);
  for (unsigned side = 0; side < 2; ++side) {
    for (double bad : {-1.0, 0.5, 2.0, std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
      taxi_values = {1, 1};
      taxi_values[side] = bad;
      refresh_taxi();
      check(testing::accept_taxi_packet(taxi_packet.data(), 56, now));
      const auto taxi = testing::taxi_buttons_at(now);
      check(!taxi.valid && std::strcmp(taxi.error, "taxi_button_values") == 0);
      check(get_ground_speed().valid && get_ground_speed().knots == 12.75);
      check(std::strcmp(sample_body_pose(now).error, before_taxi.error) == 0);
    }
  }
  taxi_values = {1, 0};
  refresh_taxi();
  check(testing::accept_taxi_packet(taxi_packet.data(), 56, now));
  reset_body_pose_calibration();
  check(get_taxi_buttons().valid && get_taxi_buttons().left_on && !get_taxi_buttons().right_on);
  check(!testing::accept_aircraft_packet(taxi_packet.data(), 56, now));
  check(!testing::accept_taxi_packet(packet.data(), packet.size(), now));
  // Speed remains independent even when pose validation or calibration fails.
  values[0] = 999;
  values[6] = 12.75;
  refresh();
  check(testing::accept_aircraft_packet(packet.data(), packet.size(), GetTickCount64()));
  check(!sample_body_pose(GetTickCount64()).valid);
  check(get_taxi_buttons().valid && get_taxi_buttons().left_on);
  speed = get_ground_speed();
  check(speed.valid && speed.knots == 12.75);
  reset_body_pose_calibration();
  check(get_ground_speed().valid);
  std::array<unsigned char, 56> lighting_packet{};
  const std::array<DWORD, 10> lighting_header{56, 0, 8, 4, 2883584, 4, 0, 0, 1, 2};
  std::array<double, 2> lighting_values{.536, .150114};
  const auto refresh_lighting = [&]() {
    std::memcpy(lighting_packet.data(), lighting_header.data(), sizeof(lighting_header));
    std::memcpy(lighting_packet.data() + 40, lighting_values.data(), sizeof(lighting_values));
  };
  check(!get_lighting().valid);
  refresh_lighting();
  check(testing::accept_lighting_packet(lighting_packet.data(), 56, 10000));
  auto light = testing::lighting_at(10000);
  check(light.valid && light.ambient == lighting_values[0] && light.brightness == lighting_values[1] && light.sample_ms == 10000);
  check(testing::lighting_at(11500).valid);
  light = testing::lighting_at(11501);
  check(!light.valid && std::isnan(light.ambient) && std::strcmp(light.error, "lighting_telemetry_stale") == 0);
  check(!testing::lighting_at(9999).valid);
  for (DWORD bytes = 0; bytes < 56; ++bytes)
    check(!testing::accept_lighting_packet(lighting_packet.data(), bytes, 10000));
  check(!testing::accept_lighting_packet(lighting_packet.data(), 57, 10000));
  check(!testing::accept_lighting_packet(nullptr, 56, 10000));
  check(!testing::accept_lighting_packet(lighting_packet.data(), 56, 0));
  for (unsigned field : {0u, 2u, 3u, 5u, 6u, 9u}) {
    refresh_lighting();
    const DWORD wrong = lighting_header[field] + 1;
    std::memcpy(lighting_packet.data() + field * 4, &wrong, 4);
    check(!testing::accept_lighting_packet(lighting_packet.data(), 56, 10000));
    check(testing::lighting_at(10000).valid);
  }
  for (DWORD object_id : {0ul, 1ul, 2883584ul, 0xfffffffful}) {
    refresh_lighting();
    std::memcpy(lighting_packet.data() + 16, &object_id, 4);
    check(testing::accept_lighting_packet(lighting_packet.data(), 56, 10000));
  }
  for (const auto valid : {std::array<double, 2>{0, 0}, std::array<double, 2>{1e7, 1}}) {
    lighting_values = valid;
    refresh_lighting();
    check(testing::accept_lighting_packet(lighting_packet.data(), 56, 10000));
    check(testing::lighting_at(10000).valid);
  }
  for (unsigned field = 0; field < 2; ++field) {
    for (double invalid :
         {-1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), field == 0 ? 1e7 + 1 : 1.01}) {
      lighting_values = {.5, .15};
      lighting_values[field] = invalid;
      refresh_lighting();
      check(testing::accept_lighting_packet(lighting_packet.data(), 56, now));
      check(!testing::lighting_at(now).valid && std::strcmp(testing::lighting_at(now).error, "lighting_values") == 0);
      check(testing::ground_speed_at(now).valid && testing::taxi_buttons_at(now).valid);
    }
  }
  lighting_values = {.536, .150114};
  refresh_lighting();
  check(testing::accept_lighting_packet(lighting_packet.data(), 56, 10000));
  reset_body_pose_calibration();
  check(testing::lighting_at(10000).valid);
  check(!testing::accept_aircraft_packet(lighting_packet.data(), 56, now));
  check(!testing::accept_taxi_packet(lighting_packet.data(), 56, now));
  check(!testing::accept_lighting_packet(taxi_packet.data(), 56, now));
  check(!testing::accept_lighting_packet(packet.data(), packet.size(), now));
  // Aircraft lifecycle drops every cached telemetry contract. A profile
  // transaction may stop/restart the provider but must not create more epochs.
  const auto before_epoch = get_aircraft_session_epoch();
  std::array<DWORD, 6> sim_event{24, 0, 4, 0, AircraftSessionLifecycle::SimEvent, 1};
  check(!testing::accept_session_packet(sim_event.data(), sizeof(sim_event)));
  check(get_aircraft_session_epoch() == before_epoch);
  std::array<unsigned char, 288> load_event{};
  const std::array<DWORD, 6> load_header{288, 0, 6, 0, AircraftSessionLifecycle::AircraftEvent, 0};
  std::memcpy(load_event.data(), load_header.data(), sizeof(load_header));
  std::strcpy(reinterpret_cast<char*>(load_event.data() + 24), "SimObjects/Airplanes/FlyByWire_A380X/aircraft.cfg");
  check(testing::accept_session_packet(load_event.data(), load_event.size()));
  check(get_aircraft_session_epoch() == before_epoch + 1);
  check(!get_ground_speed().valid && !get_taxi_buttons().valid && !get_lighting().valid);
  check(!get_body_telemetry_timing().fresh && !get_body_telemetry_timing().last_sample_ms);
  check(!get_taxi_cutoff().inhibited && !get_taxi_cutoff().pending_off && !sample_body_pose(now).calibration_required);
  check(select_aircraft_profile(1) && get_aircraft_session_epoch() == before_epoch + 1);
  check(!testing::accept_session_packet(sim_event.data(), sizeof(sim_event)) && get_aircraft_session_epoch() == before_epoch + 1);
  check(testing::accept_session_packet(load_event.data(), load_event.size()) && get_aircraft_session_epoch() == before_epoch + 2);
  // A coherent identity change also catches a missed lifecycle notification.
  std::array<unsigned char, 296> identity_type{};
  const std::array<DWORD, 10> identity_type_header{296, 0, 8, 5, 0, 5, 0, 0, 1, 1};
  std::memcpy(identity_type.data(), identity_type_header.data(), sizeof(identity_type_header));
  std::strcpy(reinterpret_cast<char*>(identity_type.data() + 40), "ATCCOM.ATC_NAME AIRBUS.0.text");
  std::array<unsigned char, 284> identity_path{};
  const std::array<DWORD, 6> identity_path_header{284, 0, 15, 6, 0, 0};
  std::memcpy(identity_path.data(), identity_path_header.data(), sizeof(identity_path_header));
  std::strcpy(reinterpret_cast<char*>(identity_path.data() + 24), "SimObjects/Airplanes/FlyByWire_A380X/aircraft.cfg");
  const auto identity_now = GetTickCount64();
  check(!testing::accept_identity_packet(identity_type.data(), identity_type.size(), identity_now));
  check(!testing::accept_identity_packet(identity_path.data(), identity_path.size(), identity_now));
  check(aircraft_matches_profile());
  check(testing::accept_lighting_packet(lighting_packet.data(), 56, identity_now));
  std::strcpy(reinterpret_cast<char*>(identity_type.data() + 40), "C172");
  check(!testing::accept_identity_packet(identity_type.data(), identity_type.size(), identity_now + 1));
  check(aircraft_matches_profile());
  std::strcpy(reinterpret_cast<char*>(identity_path.data() + 24), "SimObjects/Airplanes/Asobo_C172/aircraft.cfg");
  check(testing::accept_identity_packet(identity_path.data(), identity_path.size(), identity_now + 1));
  check(get_aircraft_session_epoch() == before_epoch + 3 && !aircraft_matches_profile() && !get_lighting().valid);
  std::array<unsigned char, 48> ground_packet{};
  const std::array<DWORD, 10> ground_header{48, 0, 8, 7, 2883584, 7, 0, 0, 1, 1};
  const auto refresh_ground = [&](double value) {
    std::memcpy(ground_packet.data(), ground_header.data(), sizeof(ground_header));
    std::memcpy(ground_packet.data() + 40, &value, sizeof(value));
  };
  check(!get_on_ground().valid);
  for (double value : {0.0, 1.0}) {
    refresh_ground(value);
    check(testing::accept_on_ground_packet(ground_packet.data(), ground_packet.size(), 10000));
    const auto sample = testing::on_ground_at(10000);
    check(sample.valid && sample.on_ground == (value == 1) && sample.sample_ms == 10000);
  }
  check(testing::on_ground_at(10500).valid && !testing::on_ground_at(10501).valid && !testing::on_ground_at(9999).valid);
  check(std::strcmp(testing::on_ground_at(10501).error, "on_ground_telemetry_stale") == 0);
  for (DWORD bytes = 0; bytes < ground_packet.size(); ++bytes)
    check(!testing::accept_on_ground_packet(ground_packet.data(), bytes, 10000));
  check(!testing::accept_on_ground_packet(ground_packet.data(), 49, 10000));
  check(!testing::accept_on_ground_packet(nullptr, 48, 10000));
  check(!testing::accept_on_ground_packet(ground_packet.data(), 48, 0));
  for (unsigned field : {0u, 2u, 3u, 5u, 6u, 9u}) {
    refresh_ground(1);
    const DWORD wrong = ground_header[field] + 1;
    std::memcpy(ground_packet.data() + field * 4, &wrong, 4);
    check(!testing::accept_on_ground_packet(ground_packet.data(), 48, 10000));
  }
  refresh();
  check(testing::accept_aircraft_packet(packet.data(), packet.size(), 10000));
  const auto independent_speed = testing::ground_speed_at(10000);
  for (double value : {-1.0, 0.5, 2.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    refresh_ground(value);
    check(testing::accept_on_ground_packet(ground_packet.data(), 48, 10000));
    check(!testing::on_ground_at(10000).valid);
    check(testing::ground_speed_at(10000).knots == independent_speed.knots);
  }
  refresh_ground(1);
  check(testing::accept_on_ground_packet(ground_packet.data(), 48, 10000));
  check(testing::accept_session_packet(load_event.data(), load_event.size()));
  check(!testing::on_ground_at(10000).valid);
  shutdown_body_pose_provider();
  check(!get_ground_speed().valid && std::isnan(get_ground_speed().knots));
  check(!get_on_ground().valid && std::strcmp(get_on_ground().error, "not_initialized") == 0);
  check(!get_taxi_buttons().valid && std::strcmp(get_taxi_buttons().error, "not_initialized") == 0);
  check(!get_lighting().valid && std::strcmp(get_lighting().error, "not_initialized") == 0);
  std::printf(
      "{\"passed\":true,\"checks\":%u,\"payload_bytes\":56,\"packet_bytes\":96,\"definition_count\":7,"
      "\"taxi_packet_bytes\":56,\"taxi_definition_count\":2,\"lighting_packet_bytes\":56,\"lighting_definition_count\":2}\n",
      checks);
  return 0;
}
#endif
int live_ground_speed() {
  if (!initialize_body_pose_provider())
    return 1;
  const auto start = GetTickCount64();
  GroundSpeedSample speed;
  BodyPoseSnapshot body;
  do {
    speed = get_ground_speed();
    body = sample_body_pose(GetTickCount64());
    if (speed.valid && body.calibration_required)
      break;
    Sleep(20);
  } while (GetTickCount64() - start < 5000);
  const bool uncalibrated = !body.valid;
  const bool good = speed.valid && uncalibrated && body.calibration_required;
  std::printf("{\"passed\":%s,\"valid\":%s,\"knots\":", good ? "true" : "false", speed.valid ? "true" : "false");
  if (speed.valid)
    std::printf("%.12f", speed.knots);
  else
    std::printf("null");
  std::printf(",\"error\":\"%s\",\"body_uncalibrated\":%s,\"body_calibration_required\":%s,\"body_error\":\"%s\"}\n", speed.error,
              uncalibrated ? "true" : "false", body.calibration_required ? "true" : "false", body.error);
  shutdown_body_pose_provider();
  return good ? 0 : 1;
}
int live_lighting() {
  if (!initialize_body_pose_provider())
    return 1;
  const auto start = GetTickCount64();
  LightingSample light;
  std::uint64_t previous = 0;
  unsigned count = 0;
  do {
    light = get_lighting();
    if (light.valid && light.sample_ms != previous) {
      previous = light.sample_ms;
      ++count;
    }
    if (count >= 3)
      break;
    Sleep(20);
  } while (GetTickCount64() - start < 5000);
  const bool good = light.valid && count >= 3 && get_ground_speed().valid;
  std::printf("{\"passed\":%s,\"samples\":%u,\"ambient\":%.17g,\"brightness\":%.17g,\"error\":\"%s\"}\n", good ? "true" : "false", count,
              light.valid ? light.ambient : -1, light.valid ? light.brightness : -1, light.error);
  shutdown_body_pose_provider();
  return good ? 0 : 1;
}
}  // namespace
int main(int argc, char** argv) {
#ifdef TAXI_BODY_POSE_PROVIDER_TESTING
  if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0)
    return offline_tests();
#endif
  if (argc == 2 && std::strcmp(argv[1], "--ground-speed") == 0)
    return live_ground_speed();
  if (argc == 2 && std::strcmp(argv[1], "--lighting") == 0)
    return live_lighting();
  if (argc != 5)
    return 2;
  Vector3 camera{};
  float fov = 0;
  for (unsigned i = 0; i < 4; ++i) {
    char* end = nullptr;
    const double v = std::strtod(argv[i + 1], &end);
    if (!end || *end || !std::isfinite(v))
      return 2;
    if (i < 3)
      camera[i] = v;
    else
      fov = static_cast<float>(v);
  }
  if (!initialize_body_pose_provider())
    return 1;
  const auto start = GetTickCount64();
  BodyPoseSnapshot sample;
  do {
    sample = sample_body_pose(GetTickCount64());
    if (sample.calibration_required)
      break;
    Sleep(20);
  } while (GetTickCount64() - start < 5000);
  bool good = sample.calibration_required && !sample.valid;
  // Fresh validated parked-camera position supplied by the read-only helper.
  // A changed view correctly refuses; this fixture commands no camera pose.
  good = good && !calibrate_body_pose(camera, 0.2f, GetTickCount64());
  bool calibrated = false;
  for (unsigned i = 0; i < 20 && !calibrated; ++i) {
    calibrated = calibrate_body_pose(camera, fov, GetTickCount64());
    if (!calibrated)
      Sleep(60);
  }
  good = good && calibrated;
  sample = sample_body_pose(GetTickCount64());
  good = good && sample.valid && !sample.calibration_required;
  // Model a caller clock sampled before the worker published the current
  // cache. Freshness must use the time at the locked read, not that old clock.
  const auto delayed_read = sample_body_pose(start);
  good = good && delayed_read.valid;
  auto stale = sample_body_pose(GetTickCount64() + 1000);
  good = good && !stale.valid;
  std::printf(
      "{\"passed\":%s,\"valid\":%s,\"error\":\"%s\",\"origin\":[%.12f,%.12f,%.12f],\"right\":[%.12f,%.12f,%.12f],\"up\":[%.12f,%.12f,%.12f]"
      ",\"forward\":[%.12f,%.12f,%.12f]}\n",
      good ? "true" : "false", sample.valid ? "true" : "false", sample.error, sample.pose.origin[0], sample.pose.origin[1],
      sample.pose.origin[2], sample.pose.right[0], sample.pose.right[1], sample.pose.right[2], sample.pose.up[0], sample.pose.up[1],
      sample.pose.up[2], sample.pose.forward[0], sample.pose.forward[1], sample.pose.forward[2]);
  reset_body_pose_calibration();
  good = good && !sample_body_pose(GetTickCount64()).valid;
  shutdown_body_pose_provider();
  good = good && !sample_body_pose(GetTickCount64()).valid;
  return good ? 0 : 1;
}
