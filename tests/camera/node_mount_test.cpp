#include "../../src/camera/node_mount.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace {
using namespace taxi_camera::native_camera;
unsigned checks = 0;
bool require(bool value, const char* message) {
  ++checks;
  if (!value)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}

// Sparse little-endian memory; unmapped bytes fail the read.
struct Memory final : taxi_camera::engine_camera::MemoryReader {
  std::map<std::uint64_t, std::uint8_t> bytes;
  template <typename T>
  void put(std::uint64_t address, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i)
      bytes[address + i] = reinterpret_cast<const std::uint8_t*>(&value)[i];
  }
  bool read(std::uint64_t address, void* destination, std::size_t size) override {
    auto* out = static_cast<std::uint8_t*>(destination);
    for (std::size_t i = 0; i < size; ++i) {
      const auto found = bytes.find(address + i);
      if (found == bytes.end())
        return false;
      out[i] = found->second;
    }
    return true;
  }
};

constexpr std::uint64_t kVtable = 0x7ff700001000;

void node(Memory& memory,
          std::uint64_t at,
          std::uint64_t parent,
          std::uint64_t first,
          std::uint64_t next,
          std::uint64_t previous,
          std::int16_t world = 3,
          std::uint64_t vtable = kVtable) {
  memory.put(at, vtable);
  memory.put(at + kNodeParent, parent);
  memory.put(at + kNodeFirstChild, first);
  memory.put(at + kNodeNextSibling, next);
  memory.put(at + kNodePreviousSibling, previous);
  memory.put(at + kNodeWorldIndex, world);
}
}  // namespace

int main() {
  bool ok = true;
  Memory memory;
  // root lists camera then other; aircraft lists part.
  const std::uint64_t root = 0x10000, camera = 0x20000, other = 0x30000, aircraft = 0x40000, part = 0x50000;
  node(memory, root, 0, camera, 0, 0);
  node(memory, camera, root, 0, other, 0);
  node(memory, other, root, 0, 0, camera);
  node(memory, aircraft, root, part, 0, 0);
  node(memory, part, aircraft, 0, 0, 0);

  const auto links = read_node_links(memory, camera, kVtable);
  ok &= require(
      links.valid && links.parent == root && links.next == other && links.previous == 0 && links.first_child == 0 && links.world == 3,
      "The camera Node links were not read");
  ok &= require(!read_node_links(memory, camera, kVtable + 8).valid, "A Node with another vtable was accepted");
  ok &= require(!read_node_links(memory, camera + 4, kVtable).valid, "A misaligned Node was read");
  ok &= require(!read_node_links(memory, 0x90000, kVtable).valid, "An unreadable Node was accepted");

  ok &= require(listed_child(memory, root, camera) && listed_child(memory, root, other), "Listed children were not found");
  ok &= require(!listed_child(memory, aircraft, camera), "A Node naming another parent was accepted");
  // Names the parent but is missing from its list.
  const std::uint64_t stray = 0x60000;
  node(memory, stray, aircraft, 0, 0, 0);
  ok &= require(!listed_child(memory, aircraft, stray), "A Node absent from its parent's list was accepted");
  // A sibling cycle ends at the walk limit instead of looping.
  node(memory, part, aircraft, 0, part, 0);
  ok &= require(!listed_child(memory, aircraft, stray), "A sibling cycle did not end");
  node(memory, part, aircraft, 0, 0, 0);

  // Generation handles: control record +0 names the object, +28 its generation.
  const std::uint64_t control = 0x70000;
  memory.put(control, aircraft);
  memory.put(control + 28, std::uint32_t{41});
  const NodeHandle handle{control, 41, 0};
  ok &= require(handle_alive(memory, handle, aircraft), "A live handle was refused");
  ok &= require(!handle_alive(memory, handle, part), "A handle naming another object was accepted");
  memory.put(control + 28, std::uint32_t{42});
  ok &= require(!handle_alive(memory, handle, aircraft), "A handle of a retired object was accepted");
  memory.put(control + 28, std::uint32_t{41});
  ok &= require(!handle_alive(memory, NodeHandle{}, aircraft), "An empty handle was accepted");

  // The handle stored in an owner (the view's +104), read only while it resolves.
  const std::uint64_t view = 0x80000;
  memory.put(view + 104, control);
  memory.put(view + 112, std::uint32_t{41});
  memory.put(view + 116, std::uint32_t{0xffffffff});
  NodeHandle read;
  ok &=
      require(read_node_handle(memory, view, 104, aircraft, read) && read.control == control && read.generation == 41 && read.reserved == 0,
              "A stored live handle was not read");
  ok &= require(!read_node_handle(memory, view, 104, part, read) && read.control == 0, "A stored handle naming another object was kept");

  // far_aim keeps the eye and direction and moves the aim point out.
  MountedPose pose;
  pose.position = {4079673.7, 1425314.4, 4675524.2};
  pose.target = {pose.position[0] + 0.6, pose.position[1], pose.position[2] + 0.8};
  pose.up = {0, 1, 0};
  pose.fov = 0.8f;
  const auto aimed = far_aim(pose);
  const double dx = aimed.target[0] - pose.position[0], dz = aimed.target[2] - pose.position[2];
  ok &= require(aimed.position == pose.position && aimed.up == pose.up && aimed.fov == pose.fov &&
                    std::abs(std::hypot(dx, dz) - kMountAimDistance) < 1e-3 && std::abs(dx / dz - 0.75) < 1e-9,
                "far_aim changed the eye or direction, or the wrong distance");
  if (!ok)
    return 1;
  std::printf("PASS node mount: %u checks; links, child lists, generation handles, far aim.\n", checks);
  return 0;
}
