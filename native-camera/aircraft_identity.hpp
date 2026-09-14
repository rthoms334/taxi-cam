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
  bool accept(const void* raw, std::uint32_t bytes) noexcept {
    if (!raw || bytes < 24 || bytes > 65536)
      return false;
    std::array<std::uint32_t, 6> h{};
    std::memcpy(h.data(), raw, sizeof(h));
    if (h[0] != bytes)
      return false;
    if (h[2] == 4 && bytes == 24 && h[4] == SimEvent && h[5] <= 1) {
      const bool transition = known_ && running_ != (h[5] != 0);
      known_ = true;
      running_ = h[5] != 0;
      if (transition)
        changed();
      return transition;
    }
    if (h[2] == 6 && bytes == 288 && (h[4] == AircraftEvent || h[4] == FlightEvent) &&
        std::memchr(static_cast<const char*>(raw) + 24, 0, 260)) {
      changed();
      return true;
    }
    return false;
  }
  void changed() noexcept {
    if (epoch_ != UINT64_MAX)
      ++epoch_;
  }
  std::uint64_t epoch() const noexcept { return epoch_; }
  bool running() const noexcept { return !known_ || running_; }

 private:
  std::uint64_t epoch_ = 0;
  bool known_ = false, running_ = false;
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
