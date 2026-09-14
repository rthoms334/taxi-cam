#pragma once

#include <d3d12.h>

#include "calibration.hpp"

namespace taxi_camera {

// Caller owns lifetime and synchronization: a direct graphics command list outside
// a native render pass, a live single-mip color RTV in RENDER_TARGET state, and
// the dimensions of that mip (not necessarily the resource's base dimensions).
// This operation does not bind state, transition resources, allocate, map or read back.
inline bool record_calibration(ID3D12GraphicsCommandList* command_list,
                               D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                               std::uint32_t width,
                               std::uint32_t height,
                               std::uint64_t frame,
                               std::uint32_t origin_x = 0) {
  if (command_list == nullptr || rtv.ptr == 0 || width < 32 || width > 16384 || origin_x > 16384 - width || height < 32 || height > 16384) {
    return false;
  }
  for (const auto& rectangle : calibration_rectangles(width, height, frame)) {
    const D3D12_RECT native_rect{rectangle.left + static_cast<LONG>(origin_x), rectangle.top, rectangle.right + static_cast<LONG>(origin_x),
                                 rectangle.bottom};
    command_list->ClearRenderTargetView(rtv, rectangle.color.data(), 1, &native_rect);
  }
  return true;
}

}  // namespace taxi_camera
