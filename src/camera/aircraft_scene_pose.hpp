#pragma once

#include <cstring>
#include "memory_reader.hpp"
#include "aircraft_mounts.hpp"

namespace taxi_camera::native_camera {

struct AircraftScenePose {
  bool complete = false;
  const char* error = "unavailable";
  BodyPose pose{};
  std::uint32_t read_bytes = 0;
};

// The caller supplies a freshly verified active user/controller in the current
// observer, under the full code profile including the +336 handle accessor.
// Never retains a borrowed pointer, acquires a reference, or writes the scene.
// 1.8.16.0 observations: controller+336 generation handle and +464 alias name
// the same model Node; its +296 matrix contains left/up/forward/origin rows.
// Attached model type/vtable and a complete reread are required. These checks
// do not independently identify an aircraft: graph/session ownership remains
// the caller's responsibility. Public pose is a separate plausibility guard.
inline AircraftScenePose inspect_aircraft_scene_pose(engine_camera::MemoryReader& reader,
                                                     std::uint64_t verified_user,
                                                     std::uint64_t image_base) noexcept {
  AircraftScenePose result;
  if (!verified_user || (verified_user & 7) || !image_base || image_base > UINT64_MAX - 136368168)
    return result;
  struct Field {
    std::uint64_t address = 0;
    std::array<unsigned char, 24> value{};
    std::size_t bytes = 0;
  };
  std::array<Field, 16> trace{};
  unsigned count = 0;
  auto read = [&](std::uint64_t owner, std::uint64_t offset, auto& value) {
    constexpr auto size = sizeof(value);
    static_assert(size <= 24);
    if (!owner || owner > UINT64_MAX - offset || owner + offset > UINT64_MAX - size || count >= trace.size() ||
        result.read_bytes > 1024 - size)
      return false;
    auto& field = trace[count++];
    field.address = owner + offset;
    field.bytes = size;
    result.read_bytes += size;
    if (!reader.read(field.address, field.value.data(), size))
      return false;
    std::memcpy(&value, field.value.data(), size);
    return true;
  };
  std::uint64_t user_vptr = 0, node = 0, alias = 0, node_vptr = 0, attached = 0, attached_vptr = 0, matrix = 0;
  std::array<std::uint64_t, 2> handle{};
  std::uint32_t generation = 0;
  std::uint16_t type = 0;
  std::array<Vector3, 4> rows{};
  result.error = "scene_body_identity";
  if (!read(verified_user, 0, user_vptr) || user_vptr != image_base + 133534840 || !read(verified_user, 336, handle) ||
      !read(handle[0], 28, generation) || generation != static_cast<std::uint32_t>(handle[1]) || !read(handle[0], 0, node) || !node ||
      (node & 7) || !read(verified_user, 464, alias) || alias != node || !read(node, 0, node_vptr) || node_vptr != image_base + 134592040 ||
      !read(node, 256, attached) || !attached || (attached & 7) || !read(attached, 0, attached_vptr) ||
      attached_vptr != image_base + 136368168 || !read(attached, 160, type) || type != 5 || !read(node, 296, matrix) || !matrix ||
      (matrix & 7))
    return result;
  result.error = "scene_body_matrix";
  for (unsigned row = 0; row < rows.size(); ++row)
    if (!read(matrix, row * 32, rows[row]))
      return result;
  const BodyPose candidate{rows[3], {-rows[0][0], -rows[0][1], -rows[0][2]}, rows[1], rows[2]};
  const auto radius = std::hypot(std::hypot(candidate.origin[0], candidate.origin[1]), candidate.origin[2]);
  if (!valid_body_pose(candidate) || radius < 6300000 || radius > 6500000)
    return result;
  result.error = "scene_body_changed";
  for (unsigned i = 0; i < count; ++i) {
    const auto& field = trace[i];
    std::array<unsigned char, 24> again{};
    if (result.read_bytes > 1024 - field.bytes)
      return result;
    result.read_bytes += static_cast<std::uint32_t>(field.bytes);
    if (!reader.read(field.address, again.data(), field.bytes) || std::memcmp(again.data(), field.value.data(), field.bytes))
      return result;
  }
  result.pose = candidate;
  result.complete = true;
  result.error = "";
  return result;
}

// This only rejects a wrong/stale aircraft reference. It never blends the
// asynchronous public pose into the scene pose used to position cameras.
inline bool scene_body_matches_public(const BodyPose& scene, const BodyPose& public_pose) noexcept {
  if (!valid_body_pose(scene) || !valid_body_pose(public_pose))
    return false;
  const Vector3 delta{scene.origin[0] - public_pose.origin[0], scene.origin[1] - public_pose.origin[1],
                      scene.origin[2] - public_pose.origin[2]};
  // Existing telemetry freshness is <=500ms and taxi cutoff is independent.
  // Allow render/telemetry phase differences, never accept a distant model or
  // incompatible orientation. A failure keeps both render gates closed.
  constexpr double minimum_dot = 0.9659258262890683;  // 15 degrees
  return mount_detail::dot(delta, delta) <= 25 * 25 && mount_detail::dot(scene.right, public_pose.right) >= minimum_dot &&
         mount_detail::dot(scene.up, public_pose.up) >= minimum_dot && mount_detail::dot(scene.forward, public_pose.forward) >= minimum_dot;
}

}  // namespace taxi_camera::native_camera
