#pragma once
#include <cmath>
#include <cstdint>
#include "aircraft_mounts.hpp"
#include "memory_reader.hpp"

namespace taxi_camera::native_camera {

// Diagnostics only (0.9.64), read-only: does the simulator attach its main
// view to the aircraft? The decoded Node set_position (66324928,
// docs/camera-entry-contract.md) stores its input at Node+112 directly when
// Node+368 is null, and otherwise transforms it by a matrix from the +368
// object and walks the +368 chain setting dirty flags: a parent link. Node+296
// points at the world matrix (translation at +96). A node attached to the
// aircraft would show a non-null +368 related to the aircraft and a stored
// position far smaller than its world position. Only numbers and flags are
// reported; no address is kept or logged, and nothing is written.
inline constexpr std::uint64_t kNodeStoredPositionOffset = 112, kNodeParentOffset = 368, kNodeWorldMatrixOffset = 296;

struct NodeLinkReport {
  bool sampled = false;                  // A node was available in this update.
  bool read = false;                     // All reads below succeeded.
  bool parent = false;                   // Node+368 non-null.
  bool parent_is_node = false;           // The +368 object has the scene Node vtable.
  bool parent_is_aircraft = false;       // +368 is the aircraft model Node.
  bool parent_is_controller = false;     // +368 is the aircraft controller.
  bool grandparent_is_aircraft = false;  // (+368)+368 is the aircraft model Node.
  double stored_norm = 0;                // |Node+112..135| (metres).
  double world_norm = 0;                 // |world translation| (metres; about 6.4e6 on Earth).
  double stored_to_world = 0;            // |stored - world| (metres).
};

// node, aircraft_node, controller: addresses validated in the caller's current
// update. node_vtable: absolute scene Node vtable address.
inline NodeLinkReport inspect_node_link(engine_camera::MemoryReader& reader,
                                        std::uint64_t node,
                                        std::uint64_t aircraft_node,
                                        std::uint64_t controller,
                                        std::uint64_t node_vtable) noexcept {
  NodeLinkReport report;
  if (!node || (node & 7) || node > UINT64_MAX - 512)
    return report;
  report.sampled = true;
  std::uint64_t parent = 0, matrix = 0;
  Vector3 stored{}, world{};
  if (!reader.read(node + kNodeParentOffset, &parent, sizeof(parent)) ||
      !reader.read(node + kNodeStoredPositionOffset, stored.data(), sizeof(stored)) ||
      !reader.read(node + kNodeWorldMatrixOffset, &matrix, sizeof(matrix)) || !matrix || (matrix & 7) || matrix > UINT64_MAX - 128 ||
      !reader.read(matrix + 96, world.data(), sizeof(world)))
    return report;
  report.parent = parent != 0;
  if (parent && !(parent & 7) && parent <= UINT64_MAX - 512) {
    std::uint64_t vptr = 0, grandparent = 0;
    report.parent_is_node = reader.read(parent, &vptr, sizeof(vptr)) && vptr == node_vtable;
    report.parent_is_aircraft = aircraft_node && parent == aircraft_node;
    report.parent_is_controller = controller && parent == controller;
    report.grandparent_is_aircraft =
        aircraft_node && reader.read(parent + kNodeParentOffset, &grandparent, sizeof(grandparent)) && grandparent == aircraft_node;
  }
  const auto norm = [](const Vector3& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
  report.stored_norm = norm(stored);
  report.world_norm = norm(world);
  report.stored_to_world = norm(Vector3{stored[0] - world[0], stored[1] - world[1], stored[2] - world[2]});
  report.read = std::isfinite(report.stored_norm) && std::isfinite(report.world_norm);
  return report;
}

}  // namespace taxi_camera::native_camera
