#pragma once
#include <cstdint>
#include <limits>
#include "aircraft_identity.hpp"
#include "aircraft_mounts.hpp"
#include "taxi_button_command.hpp"
namespace taxi_camera::native_camera {
struct BodyPoseSnapshot {
  bool valid = false;
  bool calibration_required = false;  // Fresh public camera+aircraft ready, no calibration yet.
  std::uint64_t sample_ms = 0;
  BodyPose pose{};
  const char* error = "not_initialized";
};
struct GroundSpeedSample {
  bool valid = false;
  double knots = std::numeric_limits<double>::quiet_NaN();
  std::uint64_t sample_ms = 0;
  const char* error = "not_initialized";
};
struct OnGroundSample {
  bool valid = false;
  bool on_ground = false;
  std::uint64_t sample_ms = 0;
  const char* error = "not_initialized";
};
struct BodyTelemetryTiming {
  std::uint64_t accepted_samples = 0;
  std::uint64_t last_sample_ms = 0;
  std::uint64_t last_interval_ms = 0;
  bool fresh = false;
};
struct TaxiButtonSample {
  bool valid = false;
  bool left_on = false;
  bool right_on = false;
  std::uint64_t sample_ms = 0;
  const char* error = "not_initialized";
};
struct LightingSample {
  static constexpr std::uint64_t MaximumAgeMs = 1500;
  bool valid = false;
  double ambient = std::numeric_limits<double>::quiet_NaN();
  double brightness = std::numeric_limits<double>::quiet_NaN();
  std::uint64_t sample_ms = 0;
  const char* error = "not_initialized";
};
// Public SimConnect worker only. Call lifecycle functions outside DllMain and
// private engine callbacks. Never acquires or changes the simulator camera.
bool select_aircraft_profile(std::uint32_t id) noexcept;
AircraftIdentitySample get_aircraft_identity() noexcept;
// Changes only when the flight/aircraft session changes. Selecting the
// adapter does not itself change this epoch.
std::uint64_t get_aircraft_session_epoch() noexcept;
bool aircraft_matches_profile() noexcept;
bool initialize_body_pose_provider() noexcept;
void shutdown_body_pose_provider() noexcept;
void reset_body_pose_calibration() noexcept;
// The caller supplies a freshly validated current-camera ECEF position/FOV.
// Matches public CameraGet WORLD before accepting a local vertical correction.
// No simulator/API calls occur here or in sample_body_pose.
bool calibrate_body_pose(const Vector3& private_camera_ecef, float private_fov, std::uint64_t now_ms) noexcept;
BodyPoseSnapshot sample_body_pose(std::uint64_t now_ms) noexcept;
// Receipt timing only, not a claim of simulation timestamp or prediction.
// Counter is monotonic across worker restarts; no additional simulator reads.
BodyTelemetryTiming get_body_telemetry_timing() noexcept;
// Cached public GROUND VELOCITY in knots, independent of body calibration.
// No simulator calls; a sample older than500ms is unavailable, never zeroed.
GroundSpeedSample get_ground_speed() noexcept;
// Optional SIM ON GROUND telemetry only gates background prewarming. A stale
// sample never prevents an explicit TAXI request from using the existing path.
OnGroundSample get_on_ground() noexcept;
// Cached FCU TAXI light levels; independent of body calibration. Invalid or
// older-than-500ms telemetry must not be interpreted as an authoritative OFF.
TaxiButtonSample get_taxi_buttons() noexcept;
// Cache-only bridge API. The existing SimConnect worker owns all sends.
// Permission must be refreshed; false consumes/cancels the current request.
void update_taxi_button_request(const TaxiButtonRequest& request, bool permitted) noexcept;
TaxiButtonRequestStatus get_taxi_button_request_status() noexcept;
struct TaxiCutoffStatus {
  bool inhibited = false;
  unsigned pending_off = 0;
  const char* status = "below_speed_limit";
};
TaxiCutoffStatus get_taxi_cutoff() noexcept;
// Optional public lighting values sampled at2Hz, independent of pose/TAXI.
// AMBIENT LIGHT SENSOR is a Number, not a claimed radiometric/lux measurement.
LightingSample get_lighting() noexcept;
#ifdef TAXI_BODY_POSE_PROVIDER_TESTING
namespace body_pose_provider_testing {
bool accept_session_packet(const void* packet, std::uint32_t bytes) noexcept;
bool accept_identity_packet(const void* packet, std::uint32_t bytes, std::uint64_t sample_ms) noexcept;
bool accept_aircraft_packet(const void* packet, std::uint32_t bytes, std::uint64_t sample_ms) noexcept;
GroundSpeedSample ground_speed_at(std::uint64_t now_ms) noexcept;
bool accept_on_ground_packet(const void* packet, std::uint32_t bytes, std::uint64_t sample_ms) noexcept;
OnGroundSample on_ground_at(std::uint64_t now_ms) noexcept;
bool accept_taxi_packet(const void* packet, std::uint32_t bytes, std::uint64_t sample_ms) noexcept;
TaxiButtonSample taxi_buttons_at(std::uint64_t now_ms) noexcept;
bool accept_lighting_packet(const void* packet, std::uint32_t bytes, std::uint64_t sample_ms) noexcept;
LightingSample lighting_at(std::uint64_t now_ms) noexcept;
}  // namespace body_pose_provider_testing
#endif
}  // namespace taxi_camera::native_camera
