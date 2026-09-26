#include "../../src/camera/pose_source_cache.hpp"

#include <cstdio>

namespace {
unsigned checks = 0;
bool require(bool value, const char* message) {
  ++checks;
  if (!value)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
}  // namespace

int main() {
  using taxi_camera::native_camera::PoseSourceCache;
  constexpr std::uint64_t User = 0x20000000, Epoch = 3, Resets = 1, Proven = 50000;
  bool ok = true;
  PoseSourceCache cache;
  ok &= require(!cache.reuse(Proven, Epoch, Resets), "An empty cache offered a controller");
  cache.prove(User, Proven, Epoch, Resets);
  ok &=
      require(cache.reuse(Proven, Epoch, Resets) == User && cache.reuse(Proven + PoseSourceCache::kMaximumAgeMs - 1, Epoch, Resets) == User,
              "A controller proven in this session was not reused within a second");
  ok &= require(!cache.reuse(Proven + PoseSourceCache::kMaximumAgeMs, Epoch, Resets),
                "A controller was reused a second or more after it was proven");
  ok &= require(!cache.reuse(Proven - 1, Epoch, Resets), "A clock earlier than the proof reused the controller");
  ok &= require(!cache.reuse(Proven + 1, Epoch + 1, Resets), "A new aircraft session epoch reused the old controller");
  ok &= require(!cache.reuse(Proven + 1, Epoch, Resets + 1), "A session reset did not retire the controller");
  cache.forget();
  ok &= require(!cache.reuse(Proven + 1, Epoch, Resets), "A forgotten controller was reused");
  cache.prove(User, Proven, 0, Resets);
  ok &= require(!cache.reuse(Proven + 1, 0, Resets), "A controller proven before session readiness was reused");
  cache.prove(0, Proven, Epoch, Resets);
  ok &= require(!cache.reuse(Proven + 1, Epoch, Resets), "A null controller was offered");
  cache.prove(User + 8, Proven + 2000, Epoch, Resets);
  ok &= require(cache.reuse(Proven + 2500, Epoch, Resets) == User + 8, "Proving again did not replace the controller and its time");
  if (!ok)
    return 1;
  std::printf("PASS pose source cache: %u checks; one-second reuse within a session epoch and reset generation.\n", checks);
  return 0;
}
