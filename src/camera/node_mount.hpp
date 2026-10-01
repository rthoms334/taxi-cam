#pragma once
#include <cstdint>
#include "aircraft_mounts.hpp"
#include "memory_reader.hpp"

namespace taxi_camera::native_camera {

// Camera mount on the aircraft (camera_parent_contract.hpp). A scene Node keeps
// its position relative to its parent (Node+368); when the engine recomputes a
// Node's world transform it recomputes every child's and rebuilds the Camera
// attached to each (UpdateWorldRecursive, 1.8.16.0 RVA 0x3f3e7c0). A camera Node
// that is a child of the aircraft model Node therefore follows the aircraft in
// the frame being drawn, wherever the simulation steps it after Taxi Cam's
// update. These read-only helpers verify the links before and after the
// engine's own detach/attach calls; nothing here writes memory.
//
// Node fields, pinned by the parent contract's attach/detach bodies.
inline constexpr std::uint64_t kNodeFirstChild = 0x110, kNodeNextSibling = 0x118, kNodeWorldIndex = 0x160, kNodeParent = 0x170,
                               kNodePreviousSibling = 0x178;
// A child list longer than this is not walked; the membership is then unproven.
inline constexpr unsigned kNodeChildWalkLimit = 1024;
// With a parented Node the eye moves with the aircraft after the aim was set:
// the aim point is this far along the view direction, so the eye's movement
// within a frame (metres) turns the view by microradians.
inline constexpr double kMountAimDistance = 10000;

struct NodeLinks {
  bool valid = false;
  std::uint64_t parent = 0, first_child = 0, next = 0, previous = 0;
  std::int16_t world = -1;
};

namespace node_mount_detail {
inline bool pointer(std::uint64_t value) noexcept {
  return value && !(value & 7) && value <= UINT64_MAX - 0x200;
}
template <typename T>
bool field(engine_camera::MemoryReader& reader, std::uint64_t owner, std::uint64_t offset, T& value) noexcept {
  return reader.read(owner + offset, &value, sizeof(value));
}
}  // namespace node_mount_detail

// node_vtable: absolute scene Node vtable address. Invalid unless the object
// has that vtable and every link reads.
inline NodeLinks read_node_links(engine_camera::MemoryReader& reader, std::uint64_t node, std::uint64_t node_vtable) noexcept {
  using namespace node_mount_detail;
  NodeLinks links;
  std::uint64_t vptr = 0;
  if (!pointer(node) || !field(reader, node, 0, vptr) || vptr != node_vtable || !field(reader, node, kNodeParent, links.parent) ||
      !field(reader, node, kNodeFirstChild, links.first_child) || !field(reader, node, kNodeNextSibling, links.next) ||
      !field(reader, node, kNodePreviousSibling, links.previous) || !field(reader, node, kNodeWorldIndex, links.world))
    return {};
  links.valid = true;
  return links;
}

// node names parent and appears in parent's child list (bounded walk).
inline bool listed_child(engine_camera::MemoryReader& reader, std::uint64_t parent, std::uint64_t node) noexcept {
  using namespace node_mount_detail;
  std::uint64_t named = 0, child = 0;
  if (!pointer(parent) || !pointer(node) || !field(reader, node, kNodeParent, named) || named != parent ||
      !field(reader, parent, kNodeFirstChild, child))
    return false;
  for (unsigned i = 0; i < kNodeChildWalkLimit && pointer(child); ++i) {
    if (child == node)
      return true;
    if (!field(reader, child, kNodeNextSibling, child))
      return false;
  }
  return false;
}

// A 16-byte generation handle {control, generation}. The engine's control
// records outlive their objects: generation +28 changes when the object goes.
struct NodeHandle {
  std::uint64_t control = 0;
  std::uint32_t generation = 0;
  std::uint32_t reserved = 0;
};
static_assert(sizeof(NodeHandle) == 16);

inline bool handle_alive(engine_camera::MemoryReader& reader, const NodeHandle& handle, std::uint64_t object) noexcept {
  using namespace node_mount_detail;
  std::uint32_t generation = 0;
  std::uint64_t named = 0;
  return handle.control && object && field(reader, handle.control, 28, generation) && generation == handle.generation &&
         field(reader, handle.control, 0, named) && named == object;
}

// The handle at owner+offset, accepted only while it still resolves to object.
inline bool read_node_handle(engine_camera::MemoryReader& reader,
                             std::uint64_t owner,
                             std::uint64_t offset,
                             std::uint64_t object,
                             NodeHandle& handle) noexcept {
  handle = {};
  NodeHandle observed;
  if (!owner || owner > UINT64_MAX - offset - sizeof(observed) || !reader.read(owner + offset, &observed, sizeof(observed)))
    return false;
  observed.reserved = 0;
  if (!handle_alive(reader, observed, object))
    return false;
  handle = observed;
  return true;
}

// The aim point of a parented camera (kMountAimDistance).
inline MountedPose far_aim(const MountedPose& pose) noexcept {
  auto result = pose;
  for (unsigned i = 0; i < result.target.size(); ++i)
    result.target[i] = pose.position[i] + (pose.target[i] - pose.position[i]) * kMountAimDistance;
  return result;
}

// One feed's camera Node while Taxi Cam has it attached to the aircraft
// (observer thread). root is the Node it was taken from and is given back to
// before the entry is erased; parent_handle proves the aircraft Node is alive.
// lost: the aircraft Node went away (or the links changed) under the camera;
// it is never placed or moved again, only erased.
struct FeedMount {
  std::uint64_t entry = 0, node = 0, parent = 0, root = 0;
  NodeHandle parent_handle{};
  bool lost = false;
};

}  // namespace taxi_camera::native_camera
