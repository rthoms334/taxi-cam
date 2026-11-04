#pragma once
#include <cstdint>

namespace taxi_camera {
// A tracked display texture as the routing inventory lists it.
struct PfdTargetObservation {
  std::uint64_t id = 0;
  std::uint64_t draws = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t levels = 0;
  std::uint32_t format = 0;
  // Proven RT exits observed at queue submission; not native draw counts or
  // fence-confirmed GPU completion.
  std::uint64_t submission_activity = 0;
};
}  // namespace taxi_camera
