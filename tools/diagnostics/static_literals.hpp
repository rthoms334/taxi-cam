#pragma once

#include "../../src/camera/image_inventory.hpp"

namespace taxi_camera::discovery {

struct StaticLiteral {
  std::uint32_t rva = 0;
  // Set only after printable ASCII and a terminating NUL were read;
  // incomplete or unreadable strings expose no text.
  std::string text;
  std::string error;
  std::uint32_t bytes = 0;
};

struct StaticLiteralInventory {
  // True only when the request metadata and every requested literal succeeded.
  bool valid = false;
  std::string error;
  // Exact read attempts, including failed read() calls; never more than 1024.
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
  std::vector<StaticLiteral> records;
};

// Inspect one to eight explicit RVAs only, with at most 128 byte-read attempts
// per request INCLUDING its terminating NUL (at most 127 printable characters).
// Reads stop at NUL and stay inside one declared readable, non-writable,
// non-executable, non-discardable image section. No pointers are followed and
// no process or platform API is used. Failed requests preserve successful peers.
StaticLiteralInventory inspect_static_literals(ImageReader& reader, const Inventory& image, const std::vector<std::uint32_t>& rvas);

}  // namespace taxi_camera::discovery
