#pragma once

#include <cstdint>

namespace taxi_camera::native_camera {

// Remember the owned extra-view addresses from the current inspection. Zero
// means that view was not ready. Does not read engine memory.
void stage_add_diffuse_owned(std::uint64_t owned0, std::uint64_t owned1) noexcept;

// Rate-limited read of primary pool index 0 and, when staged, the owned views.
// Read-only. Does not publish taxi-camera feeds and does not write view flags.
void flush_add_diffuse(std::uint64_t renderer) noexcept;

}  // namespace taxi_camera::native_camera
