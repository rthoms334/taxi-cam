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
    if (address == fail || size > 24)
      return false;
    if (++reads[address] == 2 && address == change)
      memory[address] ^= 1;
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
  }
};
void fixed_mounts() {
  // Translating/turning the scene must keep each mount fixed in aircraft axes,
  // even when the independently received public packet is behind that turn.
  for (double heading : {0., 58., 90., 179., 270., 359.}) {
    Fixture fixture(heading, -3.3897 + heading * .00001);
    const auto scene = inspect_aircraft_scene_pose(fixture, user, base);
    require(scene.complete && scene.read_bytes <= 1024, "verified scene pose");
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
  for (const auto address : {user, user + 336, control + 28, control, user + 464, node, node + 256, attached, attached + 160, node + 296,
                             matrix, matrix + 32, matrix + 64, matrix + 96}) {
    Fixture missing;
    missing.fail = address;
    const auto failed = inspect_aircraft_scene_pose(missing, user, base);
    require(!failed.complete && failed.pose.origin == Vector3{}, "unreadable field publishes no pose");
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
}  // namespace
int main() {
  fixed_mounts();
  refusal();
  std::printf("PASS: %u scene-body snapshot, lifetime and rigid mount checks.\n", checks);
}
