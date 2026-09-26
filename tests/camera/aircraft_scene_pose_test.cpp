#include "../../src/camera/aircraft_scene_pose.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include "../../src/camera/body_pose_math.hpp"

namespace {
using namespace taxi_camera::native_camera;
constexpr std::uint64_t base = 0x10000000, user = 0x20000000, control = 0x20001000, node = 0x20002000, attached = 0x20003000,
                        matrix = 0x20004000;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
struct Fixture : taxi_camera::engine_camera::MemoryReader {
  std::map<std::uint64_t, unsigned char> memory;
  std::map<std::uint64_t, unsigned> reads;
  std::uint64_t fail = 0, change = 0;
  BodyPose expected;
  template <typename T>
  void put(std::uint64_t address, const T& value) {
    const auto* data = reinterpret_cast<const unsigned char*>(&value);
    for (std::size_t i = 0; i < sizeof(value); ++i)
      memory[address + i] = data[i];
  }
  bool read(std::uint64_t address, void* out, std::size_t size) override {
    // Fields are at most 24 bytes; the only wider reads are the two spans.
    if (address == fail || (size > 24 && !(size == 32 && address == control) && !(size == 120 && address == matrix)))
      return false;
    // A change lands between the capture and the reread of whatever covers it.
    if (++reads[address] == 2 && change >= address && change - address < size)
      memory[change] ^= 1;
    for (std::size_t i = 0; i < size; ++i) {
      const auto found = memory.find(address + i);
      if (found == memory.end())
        return false;
      static_cast<unsigned char*>(out)[i] = found->second;
    }
    return true;
  }
  explicit Fixture(double heading = 58, double longitude = -3.3897) {
    expected = body_math::body({55.94415, longitude, 36.64, .245, -.02, heading}, 53.396);
    put(user, base + 133534840);
    put(user + 336, std::array<std::uint64_t, 2>{control, 7});
    for (unsigned i = 8; i < 28; ++i)
      put(control + i, std::uint8_t{0});  // Rest of the 32-byte control record.
    put(control + 28, std::uint32_t{7});
    put(control, node);
    put(user + 464, node);
    put(node, base + 134592040);
    put(node + 256, attached);
    put(attached, base + 136368168);
    put(attached + 160, std::uint16_t{5});
    put(node + 296, matrix);
    put(matrix, Vector3{-expected.right[0], -expected.right[1], -expected.right[2]});
    put(matrix + 32, expected.up);
    put(matrix + 64, expected.forward);
    put(matrix + 96, expected.origin);
    for (unsigned row = 0; row < 3; ++row)
      put(matrix + row * 32 + 24, std::uint64_t{0});  // Row stride padding.
  }
};
void fixed_mounts() {
  // Translating/turning the scene must keep each mount fixed in aircraft axes,
  // even when the independently received public packet is behind that turn.
  for (double heading : {0., 58., 90., 179., 270., 359.}) {
    Fixture fixture(heading, -3.3897 + heading * .00001);
    const auto scene = inspect_aircraft_scene_pose(fixture, user, base);
    require(scene.complete && scene.read_bytes <= 1024, "verified scene pose");
    // Ten reads and ten rereads: the control record and the four matrix rows
    // are one read each, where they were three and four.
    unsigned reads = 0;
    for (const auto& [address, count] : fixture.reads)
      reads += count;
    require(reads == 20 && fixture.reads[control] == 2 && fixture.reads[matrix] == 2 && !fixture.reads.count(control + 28) &&
                !fixture.reads.count(matrix + 96),
            "scene pose read count");
    require(scene.pose.origin == fixture.expected.origin && scene.pose.right == fixture.expected.right &&
                scene.pose.up == fixture.expected.up && scene.pose.forward == fixture.expected.forward,
            "native axis mapping");
    const auto lagged = body_math::body({55.94415, -3.3897 + heading * .00001 - .00001, 36.64, .245, -.02, heading - 1}, 53.396);
    require(scene_body_matches_public(scene.pose, lagged), "normal asynchronous public pose allowed");
    for (const auto offset : {Vector3{0, -2, 16}, Vector3{0, 10, -33}}) {
      MountedPose mount;
      require(make_mounted_pose(scene.pose, {offset, -15, 0, .62f}, mount), "mount from native scene pose");
      const Vector3 delta{mount.position[0] - scene.pose.origin[0], mount.position[1] - scene.pose.origin[1],
                          mount.position[2] - scene.pose.origin[2]};
      const Vector3 local{mount_detail::dot(delta, scene.pose.right), mount_detail::dot(delta, scene.pose.up),
                          mount_detail::dot(delta, scene.pose.forward)};
      for (unsigned i = 0; i < 3; ++i)
        require(std::abs(local[i] - offset[i]) < 1e-8, "turn/translation preserves aircraft-relative mount");
    }
  }
}
void refusal() {
  // Every exact read; the spans at control and matrix also serve control + 28
  // and the later matrix rows.
  for (const auto address : {user, user + 336, control, user + 464, node, node + 256, attached, attached + 160, node + 296, matrix}) {
    Fixture missing;
    missing.fail = address;
    const auto failed = inspect_aircraft_scene_pose(missing, user, base);
    require(!failed.complete && failed.pose.origin == Vector3{}, "unreadable field publishes no pose");
  }
  for (const auto address : {user, user + 336, control + 28, control, user + 464, node, node + 256, attached, attached + 160, node + 296,
                             matrix, matrix + 32, matrix + 64, matrix + 96}) {
    Fixture torn;
    torn.change = address;
    require(!inspect_aircraft_scene_pose(torn, user, base).complete, "changed field refuses whole snapshot");
  }
  Fixture stale;
  stale.put(control + 28, std::uint32_t{8});
  require(!inspect_aircraft_scene_pose(stale, user, base).complete, "stale generation");
  Fixture alias;
  alias.put(user + 464, node + 8);
  require(!inspect_aircraft_scene_pose(alias, user, base).complete, "node alias disagreement");
  Fixture camera;
  camera.put(attached + 160, std::uint16_t{7});
  require(!inspect_aircraft_scene_pose(camera, user, base).complete, "camera cannot replace model");
  Fixture nan;
  nan.put(matrix + 96, Vector3{std::numeric_limits<double>::quiet_NaN(), 0, 0});
  require(!inspect_aircraft_scene_pose(nan, user, base).complete, "nonfinite transform");
  Fixture scaled;
  scaled.put(matrix, Vector3{2, 0, 0});
  require(!inspect_aircraft_scene_pose(scaled, user, base).complete, "scaled transform");
  Fixture overflow;
  require(!inspect_aircraft_scene_pose(overflow, UINT64_MAX - 7, base).complete &&
              !inspect_aircraft_scene_pose(overflow, user, UINT64_MAX - 7).complete,
          "address overflow");
  const auto valid = overflow.expected;
  auto distant = valid;
  distant.origin[0] += 26;
  require(!scene_body_matches_public(valid, distant), "distant model refused");
  auto reversed = valid;
  for (auto& value : reversed.forward)
    value = -value;
  require(!scene_body_matches_public(valid, reversed), "wrong orientation refused");
}
void resolved_layout() {
  auto layout = observed_store_layout();
  layout.aircraft_controller_vtable += 0x10000;
  layout.scene_node_vtable += 0x20000;
  layout.scene_model_vtable += 0x30000;
  Fixture fixture;
  fixture.put(user, base + layout.aircraft_controller_vtable);
  fixture.put(node, base + layout.scene_node_vtable);
  fixture.put(attached, base + layout.scene_model_vtable);
  require(inspect_aircraft_scene_pose(fixture, user, base, layout).complete,
          "Resolved scene identities did not preserve the body-pose inspection");
  auto wrong = layout;
  wrong.scene_node_vtable += 8;
  require(!inspect_aircraft_scene_pose(fixture, user, base, wrong).complete, "Different resolved scene-node identity was accepted");
  for (const auto bad : {CameraImageLayout{},
                         [&] {
                           auto value = layout;
                           value.scene_model_vtable = UINT32_MAX;
                           return value;
                         }(),
                         [&] {
                           auto value = layout;
                           ++value.aircraft_controller_vtable;
                           return value;
                         }()}) {
    Fixture invalid;
    require(!inspect_aircraft_scene_pose(invalid, user, base, bad).complete && invalid.reads.empty(),
            "Malformed scene layout read objects or published a pose");
  }
}
}  // namespace
int main() {
  fixed_mounts();
  refusal();
  resolved_layout();
  std::printf("PASS: %u scene-body snapshot, lifetime and rigid mount checks.\n", checks);
}
