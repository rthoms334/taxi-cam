#include "../../src/graphics/cloud_merge.hpp"

#include <cstdio>

namespace {
using namespace taxi_camera::cloud_merge;
unsigned checks = 0;
bool require(bool value, const char* message) {
  ++checks;
  if (!value)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
}  // namespace

int main() {
  bool ok = true;
  // The 777 wing camera frame: camera output 15553, merge targets 15557/15218/15558.
  const Target scene{15553, DXGI_FORMAT_R11G11B10_FLOAT, 0, 368, 434, true};
  Target merge[3]{{15557, DXGI_FORMAT_R11G11B10_FLOAT, 0, 368, 434, false},
                  {15218, DXGI_FORMAT_R8_SNORM, 0, 368, 434, false},
                  {15558, DXGI_FORMAT_R8_UNORM, 0, 368, 434, false}};
  ok &= require(camera_merge_bind(scene, 3, false, merge), "The camera's cloud merge bind was not recognised");

  ok &= require(!camera_merge_bind(scene, 3, true, merge), "A bind with a depth buffer was taken for the merge");
  ok &= require(!camera_merge_bind(scene, 2, false, merge) && !camera_merge_bind(scene, 4, false, merge),
                "A bind with another target count was taken for the merge");
  ok &= require(!camera_merge_bind(scene, 3, false, nullptr), "A missing target list was accepted");

  // The main view: its scene is not a camera output, so its merge is left alone.
  Target main_scene = scene;
  main_scene.camera_output = false;
  ok &= require(!camera_merge_bind(main_scene, 3, false, merge), "The main view's merge was redirected");
  ok &= require(!camera_merge_bind(Target{}, 3, false, merge), "A merge without a previous camera bind was redirected");
  Target scene_mip = scene;
  scene_mip.mip = 1;
  ok &= require(!camera_merge_bind(scene_mip, 3, false, merge), "A camera output bound at another mip was used");
  Target scene_format = scene;
  scene_format.format = DXGI_FORMAT_R8G8B8A8_UNORM;
  ok &= require(!camera_merge_bind(scene_format, 3, false, merge), "A camera output with another format was used");

  // The merge target must be another texture of the camera's shape and formats.
  Target other[3]{merge[0], merge[1], merge[2]};
  other[0].resource = scene.resource;
  ok &= require(!camera_merge_bind(scene, 3, false, other), "A bind that already targets the camera output was redirected");
  other[0] = merge[0];
  other[0].camera_output = true;
  ok &= require(!camera_merge_bind(scene, 3, false, other), "Another camera's output was replaced");
  other[0] = merge[0];
  other[0].width = 736;
  ok &= require(!camera_merge_bind(scene, 3, false, other), "A target of another size was replaced");
  other[0] = merge[0];
  other[0].format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  ok &= require(!camera_merge_bind(scene, 3, false, other), "A colour target of another format was replaced");
  other[0] = merge[0];
  other[0].resource = 0;
  ok &= require(!camera_merge_bind(scene, 3, false, other), "An unresolved colour target was replaced");
  other[0] = merge[0];
  other[1].format = DXGI_FORMAT_R8_UNORM;
  ok &= require(!camera_merge_bind(scene, 3, false, other), "A bind with another second format was taken for the merge");
  other[1] = merge[1];
  other[2].format = DXGI_FORMAT_R8_SNORM;
  ok &= require(!camera_merge_bind(scene, 3, false, other), "A bind with another third format was taken for the merge");
  other[2] = merge[2];
  other[2].resource = 0;
  ok &= require(!camera_merge_bind(scene, 3, false, other), "A bind with an unresolved third target was taken for the merge");

  if (!ok)
    return 1;
  std::printf("PASS cloud merge: %u checks\n", checks);
  return 0;
}
