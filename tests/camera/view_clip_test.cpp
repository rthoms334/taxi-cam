#include "../../src/camera/view_clip.hpp"
#include "../../src/camera/local_memory.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <limits>

namespace {
using namespace taxi_camera::native_camera;
unsigned checks = 0;
bool require(bool value, const char* message) {
  ++checks;
  if (!value)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
void store(unsigned char* camera, float near_plane, float far_plane, float default_far, float next = 7.0f) {
  const float values[]{near_plane, far_plane, default_far, next};
  std::memcpy(camera + kCameraNearOffset, values, sizeof(values));
}
}  // namespace

int main() {
  bool ok = true;
  const CameraClip own{0.05f, 1000, 1000}, main_view{0.1f, 80000, 120000};
  ok &= require(plausible_camera_clip(own) && plausible_camera_clip(main_view), "Observed camera clips were judged implausible");
  ok &= require(plausible_camera_clip({0.05f, -1, 1000}), "A negative culling far (falls back to the default) was refused");
  ok &= require(!plausible_camera_clip({}) && !plausible_camera_clip({0, 1000, 1000}) && !plausible_camera_clip({0.05f, 1000, 0.01f}) &&
                    !plausible_camera_clip({0.05f, 0.01f, 1000}) &&
                    !plausible_camera_clip({0.05f, std::numeric_limits<float>::quiet_NaN(), 1000}) &&
                    !plausible_camera_clip({0.05f, 1000, std::numeric_limits<float>::infinity()}),
                "An implausible camera clip was accepted");

  CameraClip target;
  ok &= require(camera_far_target(own, main_view, target) && target.near_plane == own.near_plane && target.far_plane == 80000 &&
                    target.default_far == 120000,
                "A known main view did not give the camera its far pair with the camera's own near plane");
  ok &= require(camera_far_target(own, {}, target) && target == own, "An unknown main view did not leave the camera's own far");
  ok &= require(!camera_far_target(own, {0.01f, 0.04f, 1.0f}, target), "A main far inside the camera's near plane was accepted");
  ok &= require(!camera_far_target({0, 1000, 1000}, main_view, target), "An implausible own camera produced a target");

  // A private read-write page stands in for the camera object.
  auto* camera = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!require(camera != nullptr && VirtualLock(camera, 0x1000) != FALSE, "Test camera allocation failed"))
    return 1;
  const auto address = reinterpret_cast<std::uint64_t>(camera);
  store(camera, own.near_plane, own.far_plane, own.default_far);
  CameraClip read;
  ok &= require(read_camera_clip(address, read) && read == own, "The camera clip read back wrong");
  ok &= require(!read_camera_clip(0, read) && !read_camera_clip(address + 4, read), "A null or misaligned camera was read");

  auto result = apply_camera_far(address, own);
  ok &= require(result.complete && !result.write_attempted && result.after == own, "A camera already at its target was written");

  camera_far_target(own, main_view, target);
  LocalMemoryMetrics metrics;
  {
    ScopedLocalMemoryMetrics measured(metrics);
    result = apply_camera_far(address, target);
  }
  // Two fresh O(1) proofs (the 12-byte field, then the store's own 8 bytes),
  // one timed store and the two exact reads; no region scan.
  ok &= require(
      metrics.write_calls == 1 && metrics.read_calls == 2 && metrics.query_fallback_calls == 0 && metrics.query_allocation_calls == 2,
      "The far write or its proofs were untimed or scanned the region");
  float next = 0;
  std::memcpy(&next, camera + kCameraNearOffset + 12, sizeof(next));
  ok &= require(result.complete && result.write_attempted && result.before == own && result.after == target && read_camera_clip(address, read) &&
                    read == target && next == 7.0f,
                "Following the main view did not write exactly the two far values");
  result = apply_camera_far(address, target);
  ok &= require(result.complete && !result.write_attempted, "A camera already following the main view was written again");

  result = apply_camera_far(address, own);
  ok &= require(result.complete && result.write_attempted && read_camera_clip(address, read) && read == own,
                "Returning to the camera's own far did not restore it");

  store(camera, 0, 1000, 1000);
  result = apply_camera_far(address, target);
  ok &= require(!result.complete && !result.write_attempted && read_camera_clip(address, read) && read == CameraClip{0, 1000, 1000},
                "An implausible camera was written");
  store(camera, own.near_plane, own.far_plane, own.default_far);
  result = apply_camera_far(address, {own.near_plane, 0.01f, 0.02f});
  ok &= require(!result.complete && !result.write_attempted && read_camera_clip(address, read) && read == own,
                "A target inside the camera's near plane was written");

  DWORD old = 0;
  VirtualProtect(camera, 0x1000, PAGE_READONLY, &old);
  result = apply_camera_far(address, target);
  ok &= require(!result.complete && !result.write_attempted && read_camera_clip(address, read) && read == own,
                "A read-only camera was written");
  VirtualFree(camera, 0, MEM_RELEASE);

  if (!ok)
    return 1;
  std::printf("PASS view clip: %u checks; private test page only, no engine memory.\n", checks);
  return 0;
}
