// Explicitly invoked live read-only provider validation; no camera acquisition.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
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
template <class Check>
void blocked_worker_lifecycle(Check check) {
  namespace testing = body_pose_provider_testing;
  struct BlockedWorker {
    HANDLE entered{}, release{}, stop{};
    static DWORD WINAPI run(void* opaque) {
      const auto& self = *static_cast<BlockedWorker*>(opaque);
      SetEvent(self.entered);
      // Model an SDK call that does not respond to the provider's stop event.
      // Its independent deadline bounds this test even if shutdown regresses.
      return WaitForSingleObject(self.release, 5000) == WAIT_OBJECT_0 && WaitForSingleObject(self.stop, 0) == WAIT_OBJECT_0 ? 0 : 1;
    }
  } blocked;
  check(select_aircraft_profile(1));
  blocked.entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  blocked.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  blocked.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  check(blocked.entered && blocked.release && blocked.stop);
  const auto worker = CreateThread(nullptr, 0, BlockedWorker::run, &blocked, 0, nullptr);
  check(worker && WaitForSingleObject(blocked.entered, 2000) == WAIT_OBJECT_0);
  check(testing::install_worker(worker, blocked.stop));
  const auto before = testing::lifecycle_snapshot();
  check(before.worker && before.stop && before.profile == 1);
  check(initialize_body_pose_provider());
  const std::array<DWORD, 10> body_header{96, 0, 8, 1, 0, 1, 0, 0, 1, 7};
  const std::array<double, 7> body_values{51, -0.1, 123, 0, 0, 270, 5};
  std::array<unsigned char, 96> body{};
  std::memcpy(body.data(), body_header.data(), sizeof(body_header));
  std::memcpy(body.data() + 40, body_values.data(), sizeof(body_values));
  check(testing::accept_aircraft_packet(body.data(), body.size(), GetTickCount64()) && get_ground_speed().valid);
  const auto initial_epoch = get_aircraft_session_epoch();
  check(select_aircraft_profile(1));
  check(!get_ground_speed().valid && get_aircraft_session_epoch() == initial_epoch);
  const auto same = testing::lifecycle_snapshot();
  check(same.worker == before.worker && same.stop == before.stop && WaitForSingleObject(blocked.stop, 0) == WAIT_TIMEOUT);
  std::array<DWORD, 6> sim{24, 0, 4, 0, AircraftSessionLifecycle::SimEvent, 1};
  check(!testing::accept_session_packet(sim.data(), sizeof(sim)));
  sim[5] = 0;
  check(testing::accept_session_packet(sim.data(), sizeof(sim)));
  check(!testing::session_reconnect_required(true) && !select_aircraft_profile(2));
  check(testing::lifecycle_snapshot().profile == 1 && WaitForSingleObject(blocked.stop, 0) == WAIT_TIMEOUT);
  sim[5] = 1;
  check(testing::accept_session_packet(sim.data(), sizeof(sim)));
  const auto began = GetTickCount64();
  check(!shutdown_body_pose_provider());
  check(GetTickCount64() - began < 250 && WaitForSingleObject(worker, 0) == WAIT_TIMEOUT);
  check(WaitForSingleObject(blocked.stop, 0) == WAIT_OBJECT_0);
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    check(!initialize_body_pose_provider());
    check(!select_aircraft_profile(2));
    check(!select_aircraft_profile(1));
    check(!shutdown_body_pose_provider());
    const auto pending = testing::lifecycle_snapshot();
    check(pending.worker == before.worker && pending.stop == before.stop && pending.profile == before.profile);
  }
  check(GetTickCount64() - began < 250);
  // Even while shutdown is pending, snapshots/cache reads and the independent
  // worker remain available. No new-profile commit or replacement worker ran.
  DWORD flags{};
  check(GetHandleInformation(reinterpret_cast<HANDLE>(before.worker), &flags) != FALSE);
  check(GetHandleInformation(reinterpret_cast<HANDLE>(before.stop), &flags) != FALSE);
  check(SetEvent(blocked.release) != FALSE && WaitForSingleObject(worker, 2000) == WAIT_OBJECT_0);
  DWORD result = 1;
  check(GetExitCodeThread(worker, &result) != FALSE && result == 0);
  check(select_aircraft_profile(2));
  const auto stopped = testing::lifecycle_snapshot();
  check(!stopped.worker && !stopped.stop && stopped.profile == 2);
  check(shutdown_body_pose_provider() && shutdown_body_pose_provider());
  check(select_aircraft_profile(1));
  CloseHandle(worker);
  CloseHandle(blocked.entered);
  CloseHandle(blocked.release);
  CloseHandle(blocked.stop);
}
template <class Check>
void session_readiness_regressions(Check check) {
  namespace testing = body_pose_provider_testing;
  using Session = AircraftSessionLifecycle;
  std::array<unsigned char, 272> flow{};
  const auto flow_packet = [&](DWORD event, DWORD receive_id = Session::FlowReceiveId) {
    flow.fill(0);
    const std::array<DWORD, 4> h{272, 0, receive_id, event};
    std::memcpy(flow.data(), h.data(), sizeof(h));
    std::strcpy(reinterpret_cast<char*>(flow.data() + 16), "same-aircraft-new-airport.flt");
  };
  for (DWORD receive_id : {39ul, 40ul}) {
    Session session;
    const auto accept = [&](DWORD event) {
      flow_packet(event, receive_id);
      return session.accept(flow.data(), flow.size());
    };
    std::array<DWORD, 6> sim{24, 0, 4, 0, Session::SimEvent, 1};
    check(!session.accept(sim.data(), sizeof(sim)) && session.running() && session.epoch() == 0);
    check(accept(Session::FltLoad) && session.loading() && session.epoch() == 1);
    check(!accept(Session::FltLoad) && session.epoch() == 1);
    sim[5] = 0;
    check(!session.accept(sim.data(), sizeof(sim)) && session.loading() && session.epoch() == 1);
    sim[5] = 1;
    check(!session.accept(sim.data(), sizeof(sim)) && session.loading() && session.epoch() == 1);
    check(!accept(Session::FlightStart) && session.loading());
    check(accept(Session::FltLoaded) && !session.loading() && session.epoch() == 1);
    check(!accept(Session::FltLoaded) && session.epoch() == 1);
    check(accept(Session::TeleportStart) && session.loading() && session.epoch() == 2);
    check(!accept(Session::TeleportStart) && session.epoch() == 2);
    check(accept(Session::FltLoad) && session.epoch() == 2);
    check(accept(Session::FltLoaded) && session.loading());
    check(accept(Session::TeleportDone) && !session.loading() && session.epoch() == 2);
    check(!accept(Session::TeleportDone));
    check(accept(Session::FlightEnd) && session.loading() && session.epoch() == 3);
    check(!accept(Session::BackToMainMenu) && session.epoch() == 3);
    check(accept(Session::FltLoad) && session.loading());
    check(accept(Session::FlightStart) && session.loading());
    check(accept(Session::FltLoaded) && !session.loading() && session.epoch() == 3);
    check(accept(Session::BackToMainMenu) && session.epoch() == 4);
    check(!accept(Session::BackToMainMenu));
    check(accept(Session::FlightStart) && !session.loading());
    check(accept(Session::FlightEnd) && session.epoch() == 5);
    check(accept(Session::FltLoad));
    check(accept(Session::FltLoaded) && session.loading() && session.epoch() == 5);
    check(!accept(Session::FltLoaded) && session.loading());
    check(!session.accept(sim.data(), sizeof(sim)) && session.loading());
    check(accept(Session::FlightStart) && !session.loading() && session.epoch() == 5);
    flow_packet(Session::FltLoad, receive_id);
    for (DWORD bytes = 0; bytes < flow.size(); ++bytes)
      check(!session.accept(flow.data(), bytes));
    check(!session.accept(nullptr, flow.size()));
    flow.fill(0xff);
    const std::array<DWORD, 4> bad{272, 0, receive_id, Session::FltLoad};
    std::memcpy(flow.data(), bad.data(), sizeof(bad));
    check(!session.accept(flow.data(), flow.size()) && !session.loading());
    flow_packet(17, receive_id);
    check(!session.accept(flow.data(), flow.size()) && !session.loading());
    flow_packet(Session::FltLoad, 41);
    check(!session.accept(flow.data(), flow.size()) && !session.loading());
  }
  // Late Connect starts without a fabricated flow transition. Any supported
  // coherent identity can reopen readiness before its profile is selected.
  check(select_aircraft_profile(2));
  std::uint64_t now = GetTickCount64();
  std::array<unsigned char, 296> type{};
  const std::array<DWORD, 10> th{296, 0, 8, 5, 0, 5, 0, 0, 1, 1};
  std::memcpy(type.data(), th.data(), sizeof(th));
  std::strcpy(reinterpret_cast<char*>(type.data() + 40), "ATCCOM.ATC_NAME AIRBUS.0.text");
  std::array<unsigned char, 284> path{};
  const std::array<DWORD, 6> ph{284, 0, 15, 6, 0, 0};
  std::memcpy(path.data(), ph.data(), sizeof(ph));
  std::strcpy(reinterpret_cast<char*>(path.data() + 24), "SimObjects/Airplanes/FlyByWire_A380X/aircraft.cfg");
  std::array<unsigned char, 96> body{}, camera{};
  const std::array<DWORD, 10> bh{96, 0, 8, 1, 0, 1, 0, 0, 1, 7};
  const std::array<double, 7> bv{51, -0.1, 123, 0, 0, 270, 0};
  std::memcpy(body.data(), bh.data(), sizeof(bh));
  std::memcpy(body.data() + 40, bv.data(), sizeof(bv));
  const std::array<DWORD, 3> ch{96, 0, 40};
  const std::array<double, 3> cv{51, -0.1, 125};
  const DWORD world = 2;
  const double fov = 0.8;
  std::memcpy(camera.data(), ch.data(), sizeof(ch));
  std::memcpy(camera.data() + 12, cv.data(), sizeof(cv));
  std::memcpy(camera.data() + 36, &world, sizeof(world));
  std::memcpy(camera.data() + 88, &fov, sizeof(fov));
  const auto supply = [&]() {
    check(!testing::accept_identity_packet(type.data(), type.size(), now));
    check(!testing::accept_identity_packet(path.data(), path.size(), now));
    check(testing::accept_aircraft_packet(body.data(), body.size(), now));
    check(testing::accept_camera_packet(camera.data(), camera.size(), now));
  };
  check(!testing::session_readiness_at(now).ready);
  const auto epoch = get_aircraft_session_epoch();
  supply();
  check(testing::session_readiness_at(now).ready && !aircraft_matches_profile());
  check(!testing::session_readiness_at(now).flow_subscribed &&
        std::strcmp(testing::session_readiness_at(now).error, "flow_not_subscribed") == 0);
  check(get_aircraft_session_epoch() == epoch);
  check(!testing::session_readiness_at(now + 501).ready);
  flow_packet(Session::FltLoad);
  check(testing::accept_session_packet(flow.data(), flow.size()));
  check(get_aircraft_session_epoch() == epoch + 1);
  auto status = testing::session_readiness_at(now);
  check(status.loading && !status.ready && status.last_flow_event == Session::FltLoad);
  supply();
  check(!testing::session_readiness_at(now).ready && !sample_body_pose(now).calibration_required);
  check(!calibrate_body_pose(body_math::ecef(cv[0], cv[1], cv[2]), static_cast<float>(fov), now));
  update_taxi_button_request({1, epoch + 1, 2, 3, 3}, true);
  const auto request = get_taxi_button_request_status();
  check(request.failed && !request.pending_mask && std::strcmp(request.error, "taxi_request_not_permitted") == 0);
  const std::array<DWORD, 6> sim{24, 0, 4, 0, Session::SimEvent, 1};
  check(!testing::accept_session_packet(sim.data(), sizeof(sim)));
  check(testing::session_readiness_at(now).loading && get_aircraft_session_epoch() == epoch + 1);
  flow_packet(Session::FltLoaded);
  check(testing::accept_session_packet(flow.data(), flow.size()));
  check(!testing::session_readiness_at(now).ready && !testing::session_readiness_at(now).loading);
  check(!testing::accept_session_packet(flow.data(), flow.size()));
  // Completion discarded samples collected during loading, including the
  // apparently matching old aircraft. Each contract must report anew.
  ++now;
  check(!testing::accept_identity_packet(type.data(), type.size(), now));
  check(!testing::accept_identity_packet(path.data(), path.size(), now));
  check(testing::accept_aircraft_packet(body.data(), body.size(), now));
  check(!testing::session_readiness_at(now).ready);
  check(testing::accept_camera_packet(camera.data(), camera.size(), now));
  check(testing::session_readiness_at(now).ready && get_aircraft_session_epoch() == epoch + 1);
  flow_packet(Session::TeleportStart);
  check(testing::accept_session_packet(flow.data(), flow.size()) && get_aircraft_session_epoch() == epoch + 2);
  supply();
  flow_packet(Session::TeleportDone);
  check(testing::accept_session_packet(flow.data(), flow.size()) && !testing::session_readiness_at(now).ready);
  supply();
  check(testing::session_readiness_at(now).ready);
  // A reader that loses the lock race against the telemetry worker keeps the
  // last complete reading of this epoch instead of reporting "not ready": the
  // bridge gates render demand on this every tick, and a one-tick refusal
  // closed and reopened the cameras (IPC taxi_mask dropouts on 0.9.35).
  auto nonblocking = testing::session_readiness_nonblocking_at(now);
  check(nonblocking.ready && !nonblocking.cached);
  testing::hold_telemetry_lock(true);
  nonblocking = testing::session_readiness_nonblocking_at(now + 1);
  check(nonblocking.ready && nonblocking.cached && nonblocking.epoch == get_aircraft_session_epoch());
  nonblocking = testing::session_readiness_nonblocking_at(now + ReadinessCacheMaxAgeMs);
  check(nonblocking.ready && nonblocking.cached);
  nonblocking = testing::session_readiness_nonblocking_at(now + ReadinessCacheMaxAgeMs + 1);
  check(!nonblocking.ready && !nonblocking.cached && std::strcmp(nonblocking.error, "aircraft_session_cache_busy") == 0);
  nonblocking = testing::session_readiness_nonblocking_at(now - 1);
  check(!nonblocking.ready && !nonblocking.cached);
  // The native invalid-WORLD notice gates the cached reading too.
  notify_invalid_camera_world();
  nonblocking = testing::session_readiness_nonblocking_at(now + 1);
  check(!nonblocking.ready && nonblocking.cached && nonblocking.loading &&
        std::strcmp(nonblocking.error, "camera_world_revalidation") == 0);
  testing::hold_telemetry_lock(false);
  // Native precursor is nonblocking and immediately gates output. Repeated
  // bad WORLD packets invalidate intermediate fresh data without more epochs.
  check(!get_aircraft_session_readiness().ready);
  testing::service_world_invalidation();
  check(get_aircraft_session_epoch() == epoch + 3);
  // The cache belongs to the previous epoch: a busy read after an epoch change
  // must not carry the old session's readiness across.
  testing::hold_telemetry_lock(true);
  nonblocking = testing::session_readiness_nonblocking_at(now + 1);
  check(!nonblocking.ready && !nonblocking.cached && nonblocking.epoch == epoch + 3);
  testing::hold_telemetry_lock(false);
  supply();
  notify_invalid_camera_world();
  testing::service_world_invalidation();
  check(get_aircraft_session_epoch() == epoch + 3 && !testing::session_readiness_at(now).ready);
  supply();
  check(testing::session_readiness_at(now).ready);
  const double invalid = std::numeric_limits<double>::quiet_NaN();
  std::memcpy(camera.data() + 12, &invalid, sizeof(invalid));
  check(testing::accept_camera_packet(camera.data(), camera.size(), now));
  check(get_aircraft_session_epoch() == epoch + 4 && !testing::session_readiness_at(now).ready);
  std::memcpy(camera.data() + 12, cv.data(), sizeof(cv));
  const DWORD documented_camera_id = 41;
  std::memcpy(camera.data() + 8, &documented_camera_id, sizeof(documented_camera_id));
  supply();
  check(testing::session_readiness_at(now).ready);
  for (DWORD bytes = 0; bytes < camera.size(); ++bytes)
    check(!testing::accept_camera_packet(camera.data(), bytes, now));
  check(!testing::accept_camera_packet(flow.data(), flow.size(), now));
  check(!testing::accept_session_packet(camera.data(), camera.size()));
  // End Flight -> generic .flt completion can still be a menu or loading
  // scene. Fresh matching identity/body/WORLD is insufficient without the
  // distinct FLIGHT_START event, regardless of intermediate Sim=1 messages.
  const auto ended_epoch = get_aircraft_session_epoch();
  flow_packet(Session::FlightEnd);
  check(testing::accept_session_packet(flow.data(), flow.size()));
  check(!testing::session_reconnect_required(true));
  check(!select_aircraft_profile(1) && testing::lifecycle_snapshot().profile == 2);
  flow_packet(Session::FltLoad);
  check(testing::accept_session_packet(flow.data(), flow.size()));
  supply();
  check(!testing::session_readiness_at(now).ready);
  flow_packet(Session::FltLoaded);
  check(testing::accept_session_packet(flow.data(), flow.size()));
  check(!testing::session_reconnect_required(true));
  supply();
  check(!testing::accept_session_packet(sim.data(), sizeof(sim)));
  status = testing::session_readiness_at(now);
  check(status.loading && !status.ready && status.epoch == ended_epoch + 1);
  check(!sample_body_pose(now).calibration_required);
  check(!calibrate_body_pose(body_math::ecef(cv[0], cv[1], cv[2]), static_cast<float>(fov), now));
  update_taxi_button_request({2, ended_epoch + 1, 2, 3, 3}, true);
  check(get_taxi_button_request_status().failed && !get_taxi_button_request_status().pending_mask);
  flow_packet(Session::FlightStart);
  check(testing::accept_session_packet(flow.data(), flow.size()));
  check(testing::session_reconnect_required(true));
  check(!testing::session_readiness_at(now).ready);
  supply();
  status = testing::session_readiness_at(now);
  check(!status.loading && status.ready && status.epoch == ended_epoch + 1);
  check(!testing::accept_session_packet(flow.data(), flow.size()));
  check(select_aircraft_profile(1));
  const auto parked = body_math::ecef(cv[0], cv[1], cv[2]);
  bool accepted = false;
  for (unsigned sample = 0; sample < 3; ++sample) {
    now = GetTickCount64() + sample + 1;
    supply();
    check(testing::session_readiness_at(now).ready);
    CameraMatchReport near_report, far_report, latch;
    check(public_camera_matches(parked, static_cast<float>(fov), now, &near_report));
    check(!public_camera_matches(body_math::ecef(0, 0, 0), static_cast<float>(fov), now, &far_report));
    // A refusal must say how far off the candidate was, so a wrong coordinate
    // space cannot be mistaken for a camera that has not settled yet.
    check(near_report.position_valid && near_report.public_ready && near_report.geometry && near_report.horizontal_m <= 0.5);
    check(far_report.position_valid && far_report.public_ready && !far_report.geometry && far_report.horizontal_m > 1000);
    accepted = calibrate_body_pose(parked, static_cast<float>(fov), now, &latch);
    check(sample < 2 ? !accepted : accepted);
    check(latch.new_public_sample && latch.calibration_samples == sample + 1);
    if (sample == 0) {
      // A late public response defers this attempt without discarding the
      // progress already made; the candidate itself was not contradicted.
      CameraMatchReport stale;
      check(!calibrate_body_pose(parked, static_cast<float>(fov), now + 1000, &stale));
      check(!stale.public_ready && stale.calibration_samples == 1);
    }
  }
  // Issue 54: after relocating more than 10 km from the calibration origin,
  // samples refuse with outside_local_calibration_radius. Resetting clears the
  // departure lock so arrival recalibration can proceed. A short taxi move stays
  // inside the radius and remains valid without reset.
  {
    check(sample_body_pose(now).valid);
    std::array<double, 7> near_bv{51.01, -0.1, 123, 0, 0, 270, 0};
    std::array<double, 3> near_cv{51.01, -0.1, 125};
    std::memcpy(body.data() + 40, near_bv.data(), sizeof(near_bv));
    std::memcpy(camera.data() + 12, near_cv.data(), sizeof(near_cv));
    now = GetTickCount64() + 10;
    supply();
    check(sample_body_pose(now).valid);
    std::array<double, 7> far_bv{51.2, -0.1, 123, 0, 0, 270, 0};
    std::array<double, 3> far_cv{51.2, -0.1, 125};
    std::memcpy(body.data() + 40, far_bv.data(), sizeof(far_bv));
    std::memcpy(camera.data() + 12, far_cv.data(), sizeof(far_cv));
    now = GetTickCount64() + 20;
    supply();
    const auto relocated = sample_body_pose(now);
    check(!relocated.valid && !relocated.calibration_required &&
          std::strcmp(relocated.error, "outside_local_calibration_radius") == 0);
    reset_body_pose_calibration();
    const auto waiting = sample_body_pose(now);
    check(!waiting.valid && waiting.calibration_required && std::strcmp(waiting.error, "local_camera_calibration_required") == 0);
    const auto arrival = body_math::ecef(far_cv[0], far_cv[1], far_cv[2]);
    bool arrived = false;
    for (unsigned sample = 0; sample < 3; ++sample) {
      now = GetTickCount64() + 30 + sample;
      supply();
      arrived = calibrate_body_pose(arrival, static_cast<float>(fov), now);
      check(sample < 2 ? !arrived : arrived);
    }
    check(sample_body_pose(now).valid);
    // Issue 82: TAXI is usually pressed while vacating the runway, so arrival
    // recalibration must converge while the aircraft rolls. Each response
    // moves both cameras about 0.4m north (8 m/s); the altitude correction is
    // unchanged and must lock after three responses.
    reset_body_pose_calibration();
    unsigned response = 0;
    const auto roll = [&](unsigned sample, double climb) {
      std::array<double, 7> moving_bv = far_bv;
      std::array<double, 3> moving_cv = far_cv;
      moving_bv[0] += sample * 3.6e-6;
      moving_cv[0] += sample * 3.6e-6;
      moving_bv[2] += sample * climb;
      moving_cv[2] += sample * climb;
      std::memcpy(body.data() + 40, moving_bv.data(), sizeof(moving_bv));
      std::memcpy(camera.data() + 12, moving_cv.data(), sizeof(moving_cv));
      now = GetTickCount64() + 50 + ++response;
      supply();
      return body_math::ecef(moving_cv[0], moving_cv[1], moving_cv[2]);
    };
    bool rolled = false;
    for (unsigned sample = 0; sample < 3; ++sample) {
      CameraMatchReport latch;
      rolled = calibrate_body_pose(roll(sample, 0), static_cast<float>(fov), now, &latch);
      check(latch.geometry && latch.calibration_samples == sample + 1);
      check(sample < 2 ? !rolled : rolled);
    }
    check(sample_body_pose(now).valid);
    // A camera still moving vertically cannot become the altitude bias, even
    // when the public and private samples agree on every response.
    reset_body_pose_calibration();
    for (unsigned sample = 0; sample < 6; ++sample) {
      CameraMatchReport latch;
      check(!calibrate_body_pose(roll(sample, 0.05), static_cast<float>(fov), now, &latch));
      check(latch.geometry && latch.calibration_samples == 1);
    }
    check(sample_body_pose(now).calibration_required);
    // Restore the original local coordinates for the refusal fixture below.
    std::memcpy(body.data() + 40, bv.data(), sizeof(bv));
    std::memcpy(camera.data() + 12, cv.data(), sizeof(cv));
  }
  // A camera proven to be somewhere else still loses the latch.
  now = GetTickCount64() + 40;
  supply();
  CameraMatchReport wrong;
  check(!calibrate_body_pose(body_math::ecef(0, 0, 0), static_cast<float>(fov), now, &wrong));
  check(wrong.public_ready && !wrong.geometry && wrong.calibration_samples == 0);
  reset_body_pose_calibration();
  check(!sample_body_pose(now).valid);
}
int offline_tests() {
  unsigned checks = 0;
  const auto check = [&](bool good) {
    ++checks;
    if (!good) {
      std::fprintf(stderr, "Provider regression failed at check %u\n", checks);
      std::exit(1);
    }
  };
  blocked_worker_lifecycle(check);
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
  // The A340-600 adds the lower ECAM latch: three FLOAT64 values, 64 bytes.
  std::array<unsigned char, 64> sd_packet{};
  const std::array<DWORD, 10> sd_header{64, 0, 8, 3, 2883584, 3, 0, 0, 1, 3};
  for (unsigned mask = 0; mask < 8; ++mask) {
    const std::array<double, 3> values{static_cast<double>(mask & 1), static_cast<double>((mask >> 1) & 1),
                                       static_cast<double>((mask >> 2) & 1)};
    std::memcpy(sd_packet.data(), sd_header.data(), sizeof(sd_header));
    std::memcpy(sd_packet.data() + 40, values.data(), sizeof(values));
    check(testing::accept_taxi_packet(sd_packet.data(), sd_packet.size(), 10000, 3));
    check(!testing::accept_taxi_packet(sd_packet.data(), sd_packet.size(), 10000));  // Two-side layout rejects it.
    const auto taxi = testing::taxi_buttons_at(10000);
    check(taxi.valid && taxi.mask() == mask);
  }
  check(!testing::accept_taxi_packet(taxi_packet.data(), taxi_packet.size(), 10000, 3));  // Three-side layout rejects two.
  check(!testing::accept_taxi_packet(sd_packet.data(), sd_packet.size(), 10000, 4));
  // Restore the two-side fixture for the checks below.
  taxi_values = {1, 1};
  refresh_taxi();
  check(testing::accept_taxi_packet(taxi_packet.data(), taxi_packet.size(), 10000) && !testing::taxi_buttons_at(10000).sd_on);
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
  // PMDG 777 Display Select Panel: 18 L:vars on definition 3, polled on
  // request 3 and streamed on request 8 with the CHANGED flag.
  {
    constexpr DWORD bytes = 40 + 18 * 8;
    std::array<unsigned char, bytes> dsp_packet{};
    std::array<DWORD, 10> dsp_header{bytes, 0, 8, 8, 2883584, 3, 1, 0, 1, 18};
    std::array<double, 18> dsp{};
    const auto send = [&](std::uint64_t at) {
      std::memcpy(dsp_packet.data(), dsp_header.data(), sizeof(dsp_header));
      std::memcpy(dsp_packet.data() + 40, dsp.data(), sizeof(dsp));
      return testing::accept_pmdg_dsp_packet(dsp_packet.data(), bytes, at);
    };
    dsp[0] = 1.00275;  // L INBD lamp, as observed live.
    check(send(11000) && testing::taxi_buttons_at(11000).valid && testing::taxi_buttons_at(11000).mask() == 0);
    dsp[3] = 100;  // CAM pushed.
    check(send(11010));
    dsp[3] = 0;
    dsp_header[3] = 3;  // The polled request reports the release.
    dsp_header[6] = 0;
    check(send(11020));
    const auto cam = testing::taxi_buttons_at(11020);
    check(cam.valid && cam.left_on && !cam.right_on && !cam.sd_on && cam.mask() == 1);
    dsp[0] = 0;
    dsp[2] = 1.00275;  // LWR CTR selected.
    check(send(11030));
    dsp[3] = 100;
    check(send(11040));
    dsp[3] = 0;
    check(send(11050) && testing::taxi_buttons_at(11050).mask() == 5 && testing::taxi_buttons_at(11050).sd_on);
    for (unsigned field : {0u, 2u, 3u, 5u, 9u}) {
      const auto saved = dsp_header[field];
      dsp_header[field] = saved + 1;
      check(!send(11060));
      dsp_header[field] = saved;
    }
    dsp_header[6] = 2;  // Unknown request flags.
    check(!send(11060));
    dsp_header[6] = 0;
    check(!testing::accept_pmdg_dsp_packet(dsp_packet.data(), bytes - 8, 11060));
    check(!testing::accept_pmdg_dsp_packet(taxi_packet.data(), taxi_packet.size(), 11060));
    check(!testing::accept_taxi_packet(dsp_packet.data(), bytes, 11060));
    check(testing::taxi_buttons_at(11050).mask() == 5);
    dsp[3] = std::numeric_limits<double>::quiet_NaN();
    check(send(11070));
    check(!testing::taxi_buttons_at(11070).valid && std::strcmp(testing::taxi_buttons_at(11070).error, "pmdg_display_select_values") == 0);
    dsp[3] = 0;
    check(send(11080) && testing::taxi_buttons_at(11080).mask() == 5);  // The decoder kept its pages.
    refresh_taxi();
    check(testing::accept_taxi_packet(taxi_packet.data(), taxi_packet.size(), 11090) && !testing::taxi_buttons_at(11090).sd_on);
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
  session_readiness_regressions(check);
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
bool stop_live_provider() {
  const auto began = GetTickCount64();
  do {
    if (shutdown_body_pose_provider())
      return true;
    Sleep(1);
  } while (GetTickCount64() - began < 2000);
  return false;
}
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
  return stop_live_provider() && good ? 0 : 1;
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
  return stop_live_provider() && good ? 0 : 1;
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
  good = stop_live_provider() && good && !sample_body_pose(GetTickCount64()).valid;
  return good ? 0 : 1;
}
