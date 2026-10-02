#pragma once

#include <windows.h>

#include <dxgiformat.h>

#include <cstdint>

namespace taxi_camera::cloud_merge {

// One bound render target as the bridge resolved it. The resource identity is
// compared, never dereferenced here.
struct Target {
  std::uint64_t resource = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // view format
  UINT mip = 0;
  std::uint64_t width = 0;
  UINT height = 0;
  bool camera_output = false;  // a current Taxi Cam camera output texture
};

// The simulator composites its clouds with ph_volumetric_clouds_merge: three
// render targets (R11G11B10 colour, R8_SNORM, R8_UNORM), no depth, premultiplied
// blending into target 0 (1.8.16.0, PIX 2026-10-02). For the main view target 0
// is the view's scene image. For a Taxi Cam camera view the engine binds a
// texture of the same shape that nothing ever reads, so the camera's clouds are
// drawn and discarded. In a camera's recording that bind directly follows the
// bind of the camera output itself as target 0, with no barrier in between.
//
// True when `current` is that merge bind and `previous` (the bind before it in
// the same recording) left the camera output bound, still a render target.
// Target 0 of the merge can then be replaced by the camera output.
inline bool camera_merge_bind(const Target& previous, UINT count, bool depth, const Target* current) noexcept {
  if (!current || count != 3 || depth)
    return false;
  if (!previous.resource || !previous.camera_output || previous.mip || previous.format != DXGI_FORMAT_R11G11B10_FLOAT)
    return false;
  const auto& color = current[0];
  return color.resource && color.resource != previous.resource && !color.camera_output && !color.mip &&
         color.format == DXGI_FORMAT_R11G11B10_FLOAT && color.width == previous.width && color.height == previous.height &&
         current[1].resource && current[1].format == DXGI_FORMAT_R8_SNORM && current[2].resource &&
         current[2].format == DXGI_FORMAT_R8_UNORM;
}

}  // namespace taxi_camera::cloud_merge
