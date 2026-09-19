#pragma once

#include "memory_reader.hpp"

#include <cstdint>

namespace taxi_camera::engine_camera {

// Read-only observation of one view's VIEWPORT_MATERIAL_ADD_DIFFUSE VP%d
// bitmap. Material slot 9, handle offset 664. This does not read slot 0
// VIEWPORT_MATERIAL_DIFFUSE (offset 520), and it never writes view flags.
// Bit 49 is reported only so a log can show the allocation gate that already
// exists. Setting that bit, or any other P+48 bit, is not this function.
struct AddDiffuseSlot {
  bool complete = false;
  bool present = false;
  bool resource_present = false;
  bool bit49 = false;
  const char* error = "";
  std::uint32_t format = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint16_t mips = 0;
  std::uint16_t layers = 0;
  // Opaque native member for graphics identity matching only. Never log it.
  std::uint64_t resource_address = 0;
};

// view_address is one pool view (primary is pool index 0). A null, stale or
// unallocated slot 9 handle completes with present=false. Malformed pointers
// refuse the result. No engine call and no flag write.
AddDiffuseSlot inspect_add_diffuse_slot(MemoryReader& reader, std::uint64_t view_address) noexcept;

}  // namespace taxi_camera::engine_camera
