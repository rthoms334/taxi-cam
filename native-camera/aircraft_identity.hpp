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
// Cached public metadata only; owns no simulator objects and makes no API calls.
class AircraftIdentityCache {
 public:
  bool accept(const void* raw, std::uint32_t bytes, std::uint64_t now) noexcept {
    if (!raw || bytes < 24 || bytes > 65536 || !now)
      return false;
    std::array<std::uint32_t, 10> h{};
    std::memcpy(h.data(), raw, bytes >= 40 ? 40 : 24);
    const bool type = bytes == 296 && h[0] == 296 && h[2] == 8 && h[3] == 5 && h[5] == 5 && h[6] == 0 && h[9] == 1;
    const bool path = bytes == 284 && h[0] == 284 && h[2] == 15 && h[3] == 6;
    if (!type && !path)
      return false;
    const auto* text = static_cast<const char*>(raw) + (type ? 40 : 24);
    if (!std::memchr(text, 0, type ? 256 : 260))
      return false;
    if (type) {
      std::memcpy(value_.type.data(), text, 256);
      type_ms_ = now;
    } else {
      std::memcpy(value_.path.data(), text, 260);
      path_ms_ = now;
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
  AircraftIdentitySample value_;
  std::uint64_t type_ms_ = 0, path_ms_ = 0;
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
