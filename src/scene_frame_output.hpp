#pragma once

#include <d3d12.h>
#include <cstdint>
#include "../profiles/catalog.hpp"

namespace taxi_camera {
class CameraCompositorD3D12;

// One stable GPU buffer for the PFD root-SRV stamp. All application recordings
// referencing it must join the capture manager's per-device submission timeline.
// The caller brackets submit with begin/end_private_submission on that SAME
// timeline; this orders prior PFD reads before writes and future reads afterward.
// No engine resources are passed here: inputs are completed owned snapshots.
class SceneFrameOutput {
 public:
  static constexpr UINT Width = 768;
  static constexpr UINT Height = 763;
  static constexpr UINT RowPitch = Width * 4;
  static constexpr UINT64 BufferBytes = UINT64(RowPitch) * Height;
  SceneFrameOutput() = default;
  SceneFrameOutput(const SceneFrameOutput&) = delete;
  SceneFrameOutput& operator=(const SceneFrameOutput&) = delete;
  // Intentionally retain GPU objects after publication: application recordings
  // may replay the stable buffer until process exit, beyond our own fence.
  ~SceneFrameOutput() = default;
  bool initialize(ID3D12Device* device) noexcept;
  // Serialized with prepare/submit. Changes only the next recording; a closed
  // prepared recording must first be submitted or discarded.
  bool set_display_exposure(float ev) noexcept;
  bool set_ground_speed(float knots, bool valid) noexcept;
  bool set_composition(const profiles::Composition& layout) noexcept;
  float display_exposure() const noexcept;
  bool prepare(ID3D12Resource* nose, DXGI_FORMAT nose_format, ID3D12Resource* tail, DXGI_FORMAT tail_format) noexcept;
  // Discard a closed prepared list which was NEVER submitted (e.g. the manager
  // refused a private transaction). Reset discards its command/descriptor uses
  // before later prepare may replace compositor input references.
  bool discard_prepared() noexcept;
  bool submit() noexcept;
  bool idle() const noexcept;
  ID3D12CommandQueue* queue() const noexcept { return queue_; }
  ID3D12Resource* buffer() const noexcept { return buffer_; }
  D3D12_GPU_VIRTUAL_ADDRESS address() const noexcept { return address_; }
  std::uint64_t submissions() const noexcept { return submitted_; }
  ID3D12Fence* completion_fence() const noexcept { return fence_; }
  const char* error() const noexcept { return error_; }

 private:
  bool fail(const char* error) noexcept;
  ID3D12Device* device_ = nullptr;
  ID3D12CommandQueue* queue_ = nullptr;
  ID3D12CommandAllocator* allocator_ = nullptr;
  ID3D12GraphicsCommandList* list_ = nullptr;
  ID3D12Fence* fence_ = nullptr;
  ID3D12Resource* buffer_ = nullptr;
  CameraCompositorD3D12* compositor_ = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS address_ = 0;
  std::uint64_t submitted_ = 0;
  bool prepared_ = false;
  bool failed_ = false;
  const char* error_ = "";
};
}  // namespace taxi_camera
