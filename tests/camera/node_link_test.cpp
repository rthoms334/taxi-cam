#include "../../src/camera/node_link.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using taxi_camera::engine_camera::MemoryReader;
using taxi_camera::native_camera::inspect_node_link;
unsigned checks = 0;
bool require(bool value, const char* message) {
  ++checks;
  if (!value)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
// A flat fake address space from Base; reads outside it fail.
class FakeMemory final : public MemoryReader {
 public:
  static constexpr std::uint64_t Base = 0x10000;
  std::vector<unsigned char> bytes = std::vector<unsigned char>(0x4000);
  bool read(std::uint64_t address, void* destination, std::size_t size) override {
    if (address < Base || address - Base + size > bytes.size())
      return false;
    std::memcpy(destination, bytes.data() + (address - Base), size);
    return true;
  }
  template <typename T>
  void put(std::uint64_t address, const T& value) {
    std::memcpy(bytes.data() + (address - Base), &value, sizeof(T));
  }
};
constexpr std::uint64_t NodeVtable = 0x7000, Aircraft = FakeMemory::Base + 0x1000, Controller = FakeMemory::Base + 0x1800;
void node_at(FakeMemory& m, std::uint64_t node, std::uint64_t parent, std::array<double, 3> stored, std::array<double, 3> world) {
  const std::uint64_t matrix = node + 0x400;
  m.put(node, NodeVtable);
  m.put(node + 368, parent);
  m.put(node + 112, stored);
  m.put(node + 296, matrix);
  m.put(matrix + 96, world);
}
}  // namespace

int main() {
  bool ok = true;
  FakeMemory memory;
  const std::array<double, 3> world{4079673.7, 1425314.4, 4675524.2};
  // The aircraft model node itself (its own parent is null).
  node_at(memory, Aircraft, 0, world, world);
  // A free node: no parent, stored == world (Taxi Cam's own cameras).
  const std::uint64_t free_node = FakeMemory::Base + 0x100;
  node_at(memory, free_node, 0, world, world);
  auto r = inspect_node_link(memory, free_node, Aircraft, Controller, NodeVtable);
  ok &= require(r.sampled && r.read && !r.parent && !r.parent_is_aircraft && std::abs(r.stored_to_world) < 1e-6 && r.world_norm > 6.3e6,
                "A free node was not reported as unparented with stored == world");
  // A node parented to the aircraft: stored is a small local offset.
  const std::uint64_t child = FakeMemory::Base + 0x2000;
  node_at(memory, child, Aircraft, {0.5, 3.2, 28.0}, world);
  r = inspect_node_link(memory, child, Aircraft, Controller, NodeVtable);
  ok &= require(r.read && r.parent && r.parent_is_node && r.parent_is_aircraft && !r.parent_is_controller && r.stored_norm < 30 &&
                    r.stored_to_world > 6.3e6,
                "A node parented to the aircraft was not reported");
  // A node parented through an intermediate node whose parent is the aircraft.
  const std::uint64_t middle = FakeMemory::Base + 0x2800, grandchild = FakeMemory::Base + 0x3000;
  node_at(memory, middle, Aircraft, {0, 0, 0}, world);
  node_at(memory, grandchild, middle, {1, 1, 1}, world);
  r = inspect_node_link(memory, grandchild, Aircraft, Controller, NodeVtable);
  ok &= require(r.parent && r.parent_is_node && !r.parent_is_aircraft && r.grandparent_is_aircraft, "A grandparent link was not reported");
  // A parent that is the controller (not a Node).
  memory.put(child + 368, Controller);
  r = inspect_node_link(memory, child, Aircraft, Controller, NodeVtable);
  ok &= require(r.parent && r.parent_is_controller && !r.parent_is_node && !r.parent_is_aircraft, "A controller parent was not reported");
  // Null, misaligned or unreadable nodes are not sampled or not read.
  ok &= require(!inspect_node_link(memory, 0, Aircraft, Controller, NodeVtable).sampled, "A null node was sampled");
  ok &= require(!inspect_node_link(memory, free_node + 4, Aircraft, Controller, NodeVtable).sampled, "A misaligned node was sampled");
  r = inspect_node_link(memory, 0x8, Aircraft, Controller, NodeVtable);
  ok &= require(r.sampled && !r.read, "An unreadable node was reported as read");
  // A null world-matrix pointer is not read.
  memory.put(free_node + 296, std::uint64_t{0});
  r = inspect_node_link(memory, free_node, Aircraft, Controller, NodeVtable);
  ok &= require(r.sampled && !r.read, "A node without a world matrix was reported as read");
  if (!ok)
    return 1;
  std::printf("PASS node link: %u checks; free, parented, grandparent, controller and unreadable nodes, read-only.\n", checks);
  return 0;
}
