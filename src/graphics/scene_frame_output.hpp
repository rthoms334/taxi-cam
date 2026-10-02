#pragma once

#include <d3d12.h>
#include <array>
#include <cstdint>
#include "../profiles/catalog.hpp"
#include "owned_gpu_timing.hpp"

namespace taxi_camera {
class CameraCompositorD3D12;
class PfdStampD3D12;

// Stable GPU buffers for the composed image and bounded typed PFD patches.
// All application recordings copying a patch must join the capture manager's
// per-device submission timeline. Buffers/descriptors remain allocated until
// process exit, including old profile/settings versions referenced by replay.
// The caller brackets submit with begin/end_private_submission on that SAME
// timeline; this orders prior PFD reads before writes and future reads afterward.
// No engine resources are passed here: inputs are completed owned snapshots.
class SceneFrameOutput {
 public:
  struct Patch {
    ID3D12Resource* buffer = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  };
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
  bool initialize(ID3D12Device* device, bool calibration_only = false) noexcept;
  void set_gpu_timing_enabled(bool enabled) noexcept { gpu_timing_enabled_ = enabled; }
  // Composition, output-buffer copy, all requested patch draws/copies. These
  // never include native simulator scene rendering or application PFD copies.
  std::array<GpuTimingStatistics, 3> gpu_timings() noexcept {
    gpu_timing_.poll();
    return gpu_timing_.statistics();
  }
  // Serialized with prepare/submit. Changes only the next recording; a closed
  // prepared recording must first be submitted or discarded.
  bool set_display_exposure(float ev) noexcept;
  // The simulator's tone mapping (CameraCompositorD3D12::set_tone_curve). A
  // table is accepted only while no composition is executing (false otherwise;
  // offer it again before the next prepare). Exposure 0 returns to Taxi Cam's.
  bool set_tone_curve(float exposure, const std::uint32_t* table) noexcept;
  bool tone_curve_active() const noexcept;
  // CameraCompositorD3D12::set_screen_scale; 0 turns it off.
  bool set_screen_scale(float scale, float floor) noexcept;
  bool set_light_inputs(float main_exposure, float display_scale, float ambient) noexcept;
  // Development shader reload (CameraCompositorD3D12::reload_shader).
  bool reload_shader(const char* source, std::size_t size) noexcept;
  const char* compositor_error() const noexcept;
  bool set_ground_speed(float knots, bool valid) noexcept;
  bool hide_ground_speed() noexcept;
  bool set_composition(const profiles::Composition& layout) noexcept;
  bool set_reference_guides(bool enabled) noexcept;
  bool set_patch_profile(std::uint32_t profile) noexcept;
  // Metadata-only demand, admitted against the current profile's exact shape.
  // Requests never allocate or record GPU work. Every admitted slot remains
  // refreshed on later compositions, including after profile changes, because
  // closed application recordings can replay its stable buffer indefinitely.
  bool request_patch(DXGI_FORMAT format, UINT width, UINT height, const D3D12_RECT& content) noexcept;
  std::uint32_t patch_requests() const noexcept { return patch_requests_; }
  std::uint64_t patch_draws() const noexcept { return patch_draws_; }
  // Stable process-retained buffer; consumer registration/timeline is required
  // before every recorded copy. No allocation or CPU image access here.
  Patch patch(DXGI_FORMAT format, UINT width, UINT height, const D3D12_RECT& content) const noexcept;
  float display_exposure() const noexcept;
  bool prepare(ID3D12Resource* nose, DXGI_FORMAT nose_format, ID3D12Resource* left, DXGI_FORMAT left_format,
               ID3D12Resource* right, DXGI_FORMAT right_format) noexcept;
  bool prepare(ID3D12Resource* nose, DXGI_FORMAT nose_format, ID3D12Resource* tail, DXGI_FORMAT tail_format) noexcept {
    return prepare(nose, nose_format, tail, tail_format, tail, tail_format);
  }
  // Separate retained output instance: no camera inputs or compositor shaders.
  // Uses the same private submission lifetime and manager timeline contract.
  bool prepare_calibration(std::uint64_t frame) noexcept;
  // Retained waiting page: the compositor's PLEASE WAIT image, with no camera
  // inputs, into this output's buffer and every requested patch. Same private
  // submission lifetime and manager timeline contract as prepare().
  bool prepare_waiting() noexcept;
  // A requested patch has not yet been written by a submitted recording.
  bool patches_pending() const noexcept;
  // Discard a closed prepared list which was NEVER submitted (e.g. the manager
  // refused a private transaction). Reset discards its command/descriptor uses
  // before later prepare may replace compositor input references.
  bool discard_prepared() noexcept;
  bool submit() noexcept;
  bool idle() const noexcept;
  // A closed recording is waiting for submission or discard.
  bool prepared() const noexcept { return prepared_; }
  ID3D12CommandQueue* queue() const noexcept { return queue_; }
  ID3D12Resource* buffer() const noexcept { return buffer_; }
  D3D12_GPU_VIRTUAL_ADDRESS address() const noexcept { return address_; }
  std::uint64_t submissions() const noexcept { return submitted_; }
  std::uint64_t completed_submissions() const noexcept;
  ID3D12Fence* completion_fence() const noexcept { return fence_; }
  const char* error() const noexcept { return error_; }

 private:
  bool fail(const char* error) noexcept;
  bool prepare_patches(std::uint64_t calibration_frame = 0) noexcept;
  void record_buffer_copy() noexcept;
  bool valid_patch_request(DXGI_FORMAT, UINT width, UINT height, const D3D12_RECT& content) const noexcept;
  struct PatchStorage {
    Patch view;
    ID3D12Resource* texture = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_RECT content{};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0, height = 0;
    unsigned drawer = 0;
    bool written = false, recorded = false;
  };
  ID3D12Device* device_ = nullptr;
  ID3D12CommandQueue* queue_ = nullptr;
  ID3D12CommandAllocator* allocator_ = nullptr;
  ID3D12GraphicsCommandList* list_ = nullptr;
  ID3D12Fence* fence_ = nullptr;
  ID3D12Resource* buffer_ = nullptr;
  CameraCompositorD3D12* compositor_ = nullptr;
  std::array<PfdStampD3D12*, 4> patch_drawers_{};
  std::array<PatchStorage, 8> patches_{};
  ID3D12DescriptorHeap* patch_heap_ = nullptr;
  std::uint32_t patch_profile_ = 1, patch_requests_ = 0;
  std::uint64_t patch_draws_ = 0;
  D3D12_GPU_VIRTUAL_ADDRESS address_ = 0;
  std::uint64_t submitted_ = 0;
  bool prepared_ = false;
  bool calibration_only_ = false;
  bool gpu_timing_enabled_ = false;
  OwnedGpuTiming<3> gpu_timing_;
  bool failed_ = false;
  const char* error_ = "";
};
}  // namespace taxi_camera
