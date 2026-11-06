#pragma once

#include "../../src/camera/image_inventory.hpp"

namespace taxi_camera::discovery {

inline constexpr std::uint32_t kMaximumLiteralRequests = 8;
inline constexpr std::uint32_t kMaximumLiteralBytesIncludingNul = 128;
inline constexpr std::uint32_t kMaximumLiteralReadBytes = kMaximumLiteralRequests * kMaximumLiteralBytesIncludingNul;

struct StaticLiteral {
  std::uint32_t rva = 0;
  std::string text;
  std::string error;
  std::uint32_t bytes = 0;
};

struct StaticLiteralInventory {
  bool valid = false;
  std::string error;
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
  std::vector<StaticLiteral> records;
};

StaticLiteralInventory inspect_static_literals(ImageReader& reader, const Inventory& image, const std::vector<std::uint32_t>& rvas);

}
