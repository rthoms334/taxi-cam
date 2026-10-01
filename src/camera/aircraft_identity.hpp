#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include "../profiles/catalog.hpp"
namespace taxi_camera::native_camera {
struct AircraftIdentitySample {
  std::array<char, 256> type{};
  std::array<char, 260> path{};
  std::uint64_t sample_ms = 0;
  std::uint32_t detected_profile = 0;
  bool fresh = false;
};
// Public SimConnect lifecycle notifications. The first Sim event reports the
// current state; subscribing/reconnecting must not manufacture a new flight.
class AircraftSessionLifecycle {
 public:
  static constexpr std::uint32_t SimEvent = 100, AircraftEvent = 101, FlightEvent = 102;
  // Pause_EX1: dwData is the pause-state flag set, zero when running. Any pause
  // (the pause menu every flight end and quit goes through, or active pause)
  // takes the camera mounts off the aircraft (node_mount.hpp).
  static constexpr std::uint32_t PauseEvent = 103;
  // Public MSFS2024 SIMCONNECT_FLOW_EVENT values (DWORD, not event IDs).
  // https://docs.flightsimulator.com/msfs2024/html/6_Programming_APIs/SimConnect/API_Reference/Structures_And_Enumerations/SIMCONNECT_FLOW_EVENT.htm
  enum Flow : std::uint32_t {
    FltLoad = 1,
    FltLoaded = 2,
    TeleportStart = 3,
    TeleportDone = 4,
    BackToMainMenu = 9,
    FlightStart = 14,
    FlightEnd = 15
  };
  static constexpr std::uint32_t FlowReceiveId = 39;
  // The SDK web enum includes EVENT_EX1, shifting FLOW/CAMERA to40/41.
  // The installed MSFS1.8.16 CameraGet instead reports40, consistent with
  // FLOW39 without that enum entry. Accept these compatibility layouts only
  // with their exact, disjoint packet shapes, not an arbitrary receive ID.
  static bool flow_receive_id(std::uint32_t id) noexcept { return id == FlowReceiveId || id == 40; }
  bool accept(const void* raw, std::uint32_t bytes) noexcept {
    if (!raw || bytes < 24 || bytes > 65536)
      return false;
    std::array<std::uint32_t, 6> h{};
    std::memcpy(h.data(), raw, sizeof(h));
    if (h[0] != bytes)
      return false;
    if (flow_receive_id(h[2]) && bytes == 272 && std::memchr(static_cast<const char*>(raw) + 16, 0, 256)) {
      const auto event = h[3];
      if (event != FltLoad && event != FltLoaded && event != TeleportStart && event != TeleportDone && event != BackToMainMenu &&
          event != FlightStart && event != FlightEnd)
        return false;
      last_flow_event_ = event;
      const auto before = pending_;
      if (event == FltLoad || event == TeleportStart || event == BackToMainMenu || event == FlightEnd) {
        const unsigned bit = event == FltLoad ? 1u : event == TeleportStart ? 2u : 4u;
        pending_ |= bit;
        if (!before)
          changed();
      } else if (event == FltLoaded) {
        // A generic .flt load is not FLIGHT_START: menu/loading scenes can
        // already expose valid aircraft telemetry. Only FLIGHT_START releases
        // a previously observed end/menu latch.
        pending_ &= ~1u;
      } else if (event == TeleportDone) {
        pending_ &= ~2u;
      } else if (event == FlightStart) {
        pending_ &= ~4u;
        paused_ = false;
      }
      // Completion invalidates samples received during loading but does not
      // manufacture another session. Duplicate starts/completions are inert.
      return before != pending_;
    }
    if (h[2] == 4 && bytes == 24 && h[4] == PauseEvent) {
      paused_ = h[5] != 0;
      return false;
    }
    if (h[2] == 4 && bytes == 24 && h[4] == SimEvent && h[5] <= 1) {
      const bool transition = known_ && running_ != (h[5] != 0);
      known_ = true;
      running_ = h[5] != 0;
      if (transition && !loading())
        changed();
      return transition && !loading();
    }
    if (h[2] == 6 && bytes == 288 && (h[4] == AircraftEvent || h[4] == FlightEvent) &&
        std::memchr(static_cast<const char*>(raw) + 24, 0, 260)) {
      if (!loading()) {
        changed();
        return true;
      }
    }
    return false;
  }
  void changed() noexcept {
    if (epoch_ != UINT64_MAX)
      ++epoch_;
  }
  std::uint64_t epoch() const noexcept { return epoch_; }
  bool running() const noexcept { return !known_ || running_; }
  bool loading() const noexcept { return pending_ != 0; }
  bool paused() const noexcept { return paused_; }
  std::uint32_t last_flow_event() const noexcept { return last_flow_event_; }

 private:
  std::uint64_t epoch_ = 0;
  bool known_ = false, running_ = false, paused_ = false;
  unsigned pending_ = 0;
  std::uint32_t last_flow_event_ = 0;
};
// Cached public metadata only; owns no simulator objects and makes no API calls.
class AircraftIdentityCache {
 public:
  // Both responses must belong to this poll. Incomplete/late polls cannot
  // combine the previous aircraft's path with the next aircraft's type.
  void begin_request(std::uint32_t type_request, std::uint32_t path_request) noexcept {
    type_request_ = type_request;
    path_request_ = path_request;
    pending_ = {};
    pending_type_ms_ = pending_path_ms_ = 0;
  }
  bool accept(const void* raw, std::uint32_t bytes, std::uint64_t now) noexcept {
    if (!raw || bytes < 24 || bytes > 65536 || !now)
      return false;
    std::array<std::uint32_t, 10> h{};
    std::memcpy(h.data(), raw, bytes >= 40 ? 40 : 24);
    const bool type = bytes == 296 && h[0] == 296 && h[2] == 8 && h[3] == type_request_ && h[5] == 5 && h[6] == 0 && h[9] == 1;
    const bool path = bytes == 284 && h[0] == 284 && h[2] == 15 && h[3] == path_request_;
    if (!type && !path)
      return false;
    const auto* text = static_cast<const char*>(raw) + (type ? 40 : 24);
    if (!std::memchr(text, 0, type ? 256 : 260))
      return false;
    if (type) {
      std::memcpy(pending_.type.data(), text, 256);
      pending_type_ms_ = now;
    } else {
      std::memcpy(pending_.path.data(), text, 260);
      pending_path_ms_ = now;
    }
    if (pending_type_ms_ && pending_path_ms_) {
      value_ = pending_;
      type_ms_ = pending_type_ms_;
      path_ms_ = pending_path_ms_;
      pending_type_ms_ = pending_path_ms_ = 0;
    }
    return true;
  }
  AircraftIdentitySample sample(std::uint64_t now) const noexcept {
    auto out = value_;
    out.sample_ms = std::min(type_ms_, path_ms_);
    out.fresh = out.sample_ms && now >= out.sample_ms && now - out.sample_ms <= 3000;
    if (out.sample_ms)
      out.detected_profile = profiles::detect_aircraft(out.type.data(), out.path.data());
    return out;
  }

 private:
  AircraftIdentitySample value_, pending_;
  std::uint64_t type_ms_ = 0, path_ms_ = 0;
  std::uint64_t pending_type_ms_ = 0, pending_path_ms_ = 0;
  std::uint32_t type_request_ = 5, path_request_ = 6;
};
// Two distinct metadata samples must agree; repeated UI refreshes cannot count
// as repeated simulator observations. Missing data never selects a fallback.
class AutoProfileSelection {
 public:
  std::uint32_t observe(std::uint32_t id, std::uint64_t sample_ms) noexcept {
    if (!id || !sample_ms) {
      candidate_ = 0;
      return 0;
    }
    if (id != candidate_ || sample_ms < first_) {
      candidate_ = id;
      first_ = sample_ms;
      return 0;
    }
    return sample_ms > first_ ? id : 0;
  }

 private:
  std::uint32_t candidate_ = 0;
  std::uint64_t first_ = 0;
};
}  // namespace taxi_camera::native_camera
