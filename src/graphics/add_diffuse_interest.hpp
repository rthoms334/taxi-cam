#pragma once

#include <cstdint>

namespace taxi_camera::add_diffuse {

// Entry order matches the camera pair: 0 primary pool view, 1 first owned
// extra view, 2 second owned extra view. These are not taxi-camera feeds.
inline constexpr unsigned kRoleCount = 3;
inline constexpr unsigned kPrimaryRole = 0;

struct Interest {
  bool inspected = false;
  bool present = false;
  bool resource_present = false;
  bool bit49 = false;
  std::uint32_t format = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint16_t mips = 0;
  std::uint64_t resource = 0;
};

inline const char* role_name(unsigned role) noexcept {
  switch (role) {
    case 0:
      return "primary";
    case 1:
      return "owned0";
    case 2:
      return "owned1";
    default:
      return "unknown";
  }
}

inline const char* format_name(std::uint32_t format) noexcept {
  switch (format) {
    case 10:
      return "R16G16B16A16_FLOAT";
    case 26:
      return "R11G11B10_FLOAT";
    case 0:
      return "none";
    default:
      return "other";
  }
}

// Publish one view's slot 9 metadata. resource 0 clears that role. The pointer
// is an opaque match key, never written to the log.
void publish_interest(unsigned role, const Interest& interest) noexcept;
Interest interest(unsigned role) noexcept;
std::uint64_t interest_resource(unsigned role) noexcept;
bool interest_role(std::uint64_t resource, unsigned& role) noexcept;
void log_line(const char* text) noexcept;

}  // namespace taxi_camera::add_diffuse
