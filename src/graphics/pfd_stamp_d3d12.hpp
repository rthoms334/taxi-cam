#pragma once
#include "pfd_stamp_state.hpp"

namespace taxi_camera {

// An immutable GPU buffer frame, produced ONLY on an owned private command
// list. The caller's completed compositor source is exactly768x763 RGBA8_UNORM
// in COPY_SOURCE. initialize allocates; record_copy records one texture-to-buffer
// copy and a transition of ONLY our buffer to PIXEL_SHADER_RESOURCE. After
// successful producer fence completion, publish() makes it stampable. No CPU
// frame readback, per-draw allocation, app-resource transitions or heap edits.
//
// All methods externally serialized. The frame is single-use and immutable;
// never reuse its pixels. Caller MUST retain it and the stamp PSO/root until
// every consumer recording is retired and every submission has completed. CPU
// reference counts alone do not prove that. Destruction/release requires that
// evidence, or abandon() deliberately quarantines objects until process exit.
class PfdStampFrame {
 public:
  static constexpr UINT Width = 768, Height = 763, RowPitch = 3072;
  static constexpr UINT64 Bytes = UINT64{RowPitch} * Height;
  PfdStampFrame() = default;
  PfdStampFrame(const PfdStampFrame&) = delete;
  PfdStampFrame& operator=(const PfdStampFrame&) = delete;
  ~PfdStampFrame() { release(); }
  HRESULT initialize(ID3D12Device*) noexcept;
  bool record_copy(ID3D12GraphicsCommandList* private_list, ID3D12Resource* owned_compositor_output) noexcept;
  // Passing true means caller verified producer completion; this API does not
  // infer a fence from recording, Submit or Present. False never publishes.
  bool publish(bool producer_completed) noexcept;
  bool ready() const noexcept { return ready_; }
  D3D12_GPU_VIRTUAL_ADDRESS address() const noexcept { return address_; }
  ID3D12Device* device() const noexcept { return device_; }
  void release() noexcept;
  void abandon() noexcept;

 private:
  ID3D12Device* device_ = nullptr;
  ID3D12Resource* buffer_ = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS address_ = 0;
  bool recorded_ = false, ready_ = false;
};

class PfdStampD3D12 {
 public:
  PfdStampD3D12() = default;
  PfdStampD3D12(const PfdStampD3D12&) = delete;
  PfdStampD3D12& operator=(const PfdStampD3D12&) = delete;
  ~PfdStampD3D12() { release(); }
  // Exact typed RTV and optional DSV formats, single sample. Initialize outside
  // draw callbacks; creates root/PSO and compiles once. RGBA/BGRA8 UNORM/sRGB;
  // DSV may be UNKNOWN (absent), D16, D24S8, D32 or D32S8. Depth/stencil testing
  // and writes remain disabled; the bound DSV is never changed by the stamp.
  HRESULT initialize(ID3D12Device*, DXGI_FORMAT target_format, DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN) noexcept;
  // PRIVATE command list only. Caller binds our owned patch RTV, no DSV. This
  // overwrites graphics state without replaying any application bindings.
  bool record_private_patch(ID3D12GraphicsCommandList*,
                            ID3D12Device* buffer_device,
                            D3D12_GPU_VIRTUAL_ADDRESS address,
                            UINT width,
                            UINT height,
                            const D3D12_RECT* destination = nullptr,
                            const D3D12_RECT* content = nullptr,
                            bool draw = true,
                            PfdStateGroup group = PfdStateGroup::all) noexcept;
  // Caller has just forwarded an actual direct draw on this exact native list,
  // with ONE bound matching RTV and the matching optional DSV, outside native pass/bundle, no pending
  // split/aliasing, exact selected bound mip extent and registered generations.
  // No guessed RT transitions: actual preceding valid draw establishes RT use.
  // Revalidate live layout/PSO generations before calling; state is synchronous.
  // Native state observers must be suppressed for internal restore OR update
  // only matching original state (graphics CBV restores its existing value).
  bool record(ID3D12GraphicsCommandList*, const PfdGraphicsState&, const PfdStampFrame&, UINT width, UINT height) noexcept;
  // Connectable owned-output adapter: exact768x763 RGBA8 byte rows at3072B pitch.
  // Address must belong to this device's live owned DEFAULT buffer, ready for
  // shader reads (COMMON promotion is valid for buffers). Producer completion
  // and exclusion of concurrent/future writes during every consumer submission
  // are caller fence contracts, not inferred from this scalar GPU address.
  // A partial state group is accepted only when draw=false.
  // Rectangles are half-open absolute target pixels. Content must be nonempty
  // and contained in destination; nullptr uses the whole destination. The
  // existing single draw maps the complete frame into content and emits opaque
  // black in the surrounding destination border. Outside destination is untouched.
  bool record_buffer(ID3D12GraphicsCommandList*,
                     const PfdGraphicsState&,
                     ID3D12Device* buffer_device,
                     D3D12_GPU_VIRTUAL_ADDRESS,
                     UINT width,
                     UINT height,
                     const D3D12_RECT* destination = nullptr,
                     const D3D12_RECT* content = nullptr,
                     bool draw = true,
                     PfdStateGroup group = PfdStateGroup::all) noexcept;
  // Terminal DIRECT-list recording only, immediately before forwarding Close.
  // Caller guarantees no later application commands in this recording, binds
  // the retained matching RTV/no DSV, and preserves pass/query/resource guards.
  // Same owned-buffer/fence contract as record_buffer. Successful recording
  // leaves our graphics state bound; application state is never replayed.
  bool record_final_buffer(ID3D12GraphicsCommandList*,
                           const PfdGraphicsState&,
                           ID3D12Device* buffer_device,
                           D3D12_GPU_VIRTUAL_ADDRESS address,
                           UINT width,
                           UINT height,
                           const D3D12_RECT* destination = nullptr,
                           const D3D12_RECT* content = nullptr) noexcept;
  DXGI_FORMAT format() const noexcept { return format_; }
  DXGI_FORMAT depth_format() const noexcept { return depth_format_; }
  void release() noexcept;
  void abandon() noexcept;

 private:
  ID3D12Device* device_ = nullptr;
  ID3D12RootSignature* root_ = nullptr;
  ID3D12PipelineState* pipeline_ = nullptr;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT depth_format_ = DXGI_FORMAT_UNKNOWN;
};
}  // namespace taxi_camera
