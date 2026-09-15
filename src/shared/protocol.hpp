#pragma once
#include <windows.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include "../profiles/catalog.hpp"
#include "version.hpp"

namespace taxi_camera::standalone {
constexpr std::uint32_t ProtocolMagic = 0x54415849, ProtocolVersion = 8;
constexpr const wchar_t* Version = TAXI_CAM_VERSION_WIDE;
struct Settings {
  std::uint32_t enabled = 1, camera_rate = 15, automatic_exposure = 1;
  float exposure = profiles::A380.exposure, night_boost = 4.f;
  std::uint64_t route_request{}, left_id{}, right_id{};
  std::uint64_t profile_request{};         // Session-only: selecting the same profile is an explicit retry.
  std::uint64_t aircraft_session_epoch{};  // Scope manual previews and texture IDs to the observed flight.
  std::uint64_t taxi_request{};            // Session-only desired aircraft button state from a shortcut.
  std::uint32_t taxi_selected_mask{}, taxi_desired_mask{};
  std::uint32_t auto_profile = 1;
  std::array<float, 3> speed_color = profiles::A380.composition.speed_color;
  // Normalized left-side guide positions; the right side mirrors X. These are
  // visual alignment settings, not calibrated ground-clearance measurements.
  std::array<float, 2> nose_dot = profiles::A380.composition.nose_dot;
  std::array<float, 2> tail_upper = profiles::A380.composition.tail_upper;
  std::array<float, 2> tail_corner = profiles::A380.composition.tail_corner;
  std::array<float, 2> tail_inner = profiles::A380.composition.tail_inner;
  std::uint32_t profile = 1, follow_taxi = 1, auto_detect = 1, single_camera = 0, manual_mask = 0, calibration_mask = 0,
                calibration_budget = 4096, scene_test = 0;
  std::array<std::array<double, 6>, 2> mounts = profiles::A380.mounts;
};
inline void reset_guide_settings(Settings& settings, const profiles::AircraftProfile& profile) noexcept {
  settings.nose_dot = profile.composition.nose_dot;
  settings.tail_upper = profile.composition.tail_upper;
  settings.tail_corner = profile.composition.tail_corner;
  settings.tail_inner = profile.composition.tail_inner;
}
struct Candidate {
  std::uint64_t id{}, draws{};
  std::uint32_t width{}, height{}, mips{}, format{};
};
struct Status {
  std::uint64_t heartbeat{}, captures{}, composed{}, stamps{}, left_id{}, right_id{}, hook_failures{};
  std::uint32_t graphics_ready{}, scene_ready{}, taxi_mask{}, speed_inhibited{}, candidate_count{};
  std::uint32_t active_profile{}, detected_profile{};
  std::uint64_t identity_sample_ms{}, aircraft_session_epoch{};
  std::uint64_t taxi_buttons_sample_ms{}, taxi_request_seen{}, taxi_request_retired{};
  std::uint32_t taxi_buttons_valid{}, taxi_buttons_mask{}, taxi_request_pending{}, taxi_request_failed{};
  char aircraft_type[256]{}, aircraft_path[260]{};
  float speed{}, exposure{};
  double probe_cpu_ms{}, probe_max_ms{};
  std::array<double, 10> stage_ms{};
  Candidate candidates[16]{};
  char message[384]{};
};
struct Shared {
  std::uint32_t magic{}, version{}, bytes{}, owner_pid{};
  std::uint64_t owner_heartbeat{};
  Settings settings;
  Status status;
};
inline bool valid_settings(const Settings& s) noexcept {
  for (const auto& position : {s.nose_dot, s.tail_upper, s.tail_corner, s.tail_inner})
    if (!std::isfinite(position[0]) || !std::isfinite(position[1]) || position[0] < 0 || position[0] > 0.5f || position[1] < 0 ||
        position[1] > 1)
      return false;
  for (const float c : s.speed_color)
    if (!std::isfinite(c) || c < 0 || c > 1)
      return false;
  if (s.auto_profile > 1 || !profiles::find(s.profile) || s.follow_taxi > 1 || s.auto_detect > 1 || s.single_camera > 1 ||
      s.scene_test > 1 || s.manual_mask > 3 || s.calibration_mask > 3 || s.calibration_budget < 64 || s.calibration_budget > 16384)
    return false;
  if (s.taxi_selected_mask > 3 || (s.taxi_desired_mask & ~s.taxi_selected_mask) || (!s.taxi_request && s.taxi_selected_mask))
    return false;
  for (const auto& m : s.mounts) {
    for (const double v : m)
      if (!std::isfinite(v))
        return false;
    if (std::abs(m[0]) > 500 || std::abs(m[1]) > 500 || std::abs(m[2]) > 500 || std::abs(m[3]) > 89 || std::abs(m[4]) > 180 ||
        m[5] < 0.05 || m[5] > 1.55)
      return false;
  }
  return s.enabled <= 1 && s.camera_rate >= 15 && s.camera_rate <= 60 && s.automatic_exposure <= 1 && std::isfinite(s.exposure) &&
         s.exposure >= -16 && s.exposure <= 4 && std::isfinite(s.night_boost) && s.night_boost >= 0 && s.night_boost <= 8;
}
class Mailbox {
 public:
  ~Mailbox() { close(); }
  Mailbox() = default;
  Mailbox(const Mailbox&) = delete;
  Mailbox& operator=(const Mailbox&) = delete;
  bool open(DWORD pid, bool create) {
    close();
    wchar_t name[128];
    // Keep IPC names stable across branding changes; ProtocolVersion guards layout compatibility.
    std::swprintf(name, 128, L"Local\\380TaxiCamera.Control.%lu", pid);
    mutex_ = create ? CreateMutexW(nullptr, FALSE, name) : OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
    if (!mutex_)
      return false;
    std::swprintf(name, 128, L"Local\\380TaxiCamera.Data.%lu", pid);
    mapping_ = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name)
                      : OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    const bool fresh = create && GetLastError() != ERROR_ALREADY_EXISTS;
    if (!mapping_) {
      close();
      return false;
    }
    memory_ = static_cast<Shared*>(MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Shared)));
    if (!memory_ || !lock(1000)) {
      close();
      return false;
    }
    if (fresh) {
      *memory_ = {};
      memory_->magic = ProtocolMagic;
      memory_->version = ProtocolVersion;
      memory_->bytes = sizeof(Shared);
    }
    const bool valid = memory_->magic == ProtocolMagic && memory_->version == ProtocolVersion && memory_->bytes == sizeof(Shared);
    unlock();
    if (!valid)
      close();
    return valid;
  }
  enum class LockResult { acquired, busy, invalid };
  bool lock(DWORD timeout = 0) noexcept { return try_lock(timeout) == LockResult::acquired; }
  LockResult try_lock(DWORD timeout = 0) noexcept {
    if (!mutex_)
      return LockResult::invalid;
    const auto result = WaitForSingleObject(mutex_, timeout);
    if (result == WAIT_ABANDONED) {
      // The previous writer died; don't consume its possibly partial message.
      if (memory_) {
        memory_->settings.enabled = 0;
        memory_->owner_heartbeat = 0;
      }
      ReleaseMutex(mutex_);
      return LockResult::invalid;
    }
    return result == WAIT_OBJECT_0 ? LockResult::acquired : result == WAIT_TIMEOUT ? LockResult::busy : LockResult::invalid;
  }
  void unlock() noexcept { ReleaseMutex(mutex_); }
  Shared* data() const noexcept { return memory_; }
  void close() noexcept {
    if (memory_)
      UnmapViewOfFile(memory_);
    if (mapping_)
      CloseHandle(mapping_);
    if (mutex_)
      CloseHandle(mutex_);
    memory_ = nullptr;
    mapping_ = mutex_ = nullptr;
  }

 private:
  HANDLE mutex_{}, mapping_{};
  Shared* memory_{};
};
}  // namespace taxi_camera::standalone
