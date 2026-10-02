#pragma once

#include "../profiles/catalog.hpp"
#include "ground_speed_display.hpp"
#include "native_device_identity.hpp"

#include <d3d12.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace taxi_camera {

// GPU composition only: this class does not obtain or render simulator cameras.
//
// Caller contract:
// - Use an OPEN, caller-owned PRIVATE DIRECT list outside any render pass. This
//   class overwrites graphics bindings; it must never receive an MSFS list.
// - Serialize all methods. Producer work must finish before these input reads,
//   using queue ordering/fences as appropriate. Input before states describe
//   mip zero exactly; pending split barriers and heap aliasing are unsupported.
// - Submit recorded lists and output copies in order. Keep all objects alive
//   until GPU completion. AddRef does not synchronize access to texture contents.
// - Changing inputs, release, and destruction require completion of EVERY list
//   using the old descriptors/resources. Unsubmitted lists must be discarded.
// - output() is borrowed, remains COPY_SOURCE after record, and may only be read
//   until the next serialized record. The caller owns output-copy synchronization.
//
// initialize compiles/allocates once. set_inputs writes descriptors only when
// resources/formats change, and then (re)allocates each HDR feed's bloom
// pyramid for its size. record allocates, compiles, uploads and reads back
// nothing; it restores both input mip-zero states and leaves output COPY_SOURCE.
// RGB is sampled as declared by the typed SRV (sRGB views decode to linear).
// R11G11B10_FLOAT feeds receive the simulator's own bloom (see BloomShader),
// then either the simulator's own exposure and tone curve (set_tone_curve) or
// Taxi Cam's exposure with per-channel Reinhard compression, and sRGB encoding
// for this SDR display. Other formats retain their sampled RGB.
// This explicit display conversion is not simulator exposure/color calibration.
// Each feed stretches over its full region. Output alpha is a per-pixel encoding
// flag for the PFD stamp, not coverage: 1 marks camera pixels (display-referred
// codes), 0 marks overlays authored like aircraft UI colours. Displays always
// receive alpha 1.
class CameraCompositorD3D12 {
 public:
  static constexpr UINT Width = 768;
  static constexpr UINT Height = 763;
  static constexpr UINT NoseHeight = 255;
  static constexpr UINT DividerHeight = 4;
  // Paint over four existing pixels at each pane edge without changing camera
  // resource dimensions, sampling coordinates or the 763-row output contract.
  static constexpr UINT VisibleDividerTop = 251;
  static constexpr UINT VisibleDividerHeight = 12;
  static constexpr UINT TailHeight = 504;
  // PLEASE WAIT is the GS stroke font at this scale, centred in the working
  // image. Keep equal to WaitingScale in the shader; cells stay pixel-aligned.
  static constexpr float WaitingTextScale = 1.5f;
  static constexpr UINT WaitingTextWidth = static_cast<UINT>((16 * 10 + 12) * WaitingTextScale);
  static constexpr UINT WaitingTextHeight = static_cast<UINT>(20 * WaitingTextScale);
  static_assert(16 * WaitingTextScale == static_cast<UINT>(16 * WaitingTextScale) &&
                12 * WaitingTextScale == static_cast<UINT>(12 * WaitingTextScale));
  static constexpr float MinimumExposureEv = -16;
  static constexpr float MaximumExposureEv = 4;
  // User-approved starting point, still adjustable for the current scene.
  static constexpr float DefaultExposureEv = -8.8f;

  struct Statistics {
    std::uint64_t shader_compiles = 0;
    std::uint64_t descriptor_writes = 0;
    std::uint64_t input_changes = 0;
    std::uint64_t recordings = 0;
  };

  CameraCompositorD3D12() = default;
  CameraCompositorD3D12(const CameraCompositorD3D12&) = delete;
  CameraCompositorD3D12& operator=(const CameraCompositorD3D12&) = delete;
  ~CameraCompositorD3D12() { release(); }

  ID3D12Resource* output() const noexcept { return output_.get(); }
  const Statistics& statistics() const noexcept { return statistics_; }
  const char* last_error() const noexcept { return error_.data(); }
  float display_exposure() const noexcept { return exposure_ev_; }
  void set_composition(const profiles::Composition& layout) noexcept { composition_ = layout; }
  bool reference_guides() const noexcept { return reference_guides_; }
  void set_reference_guides(bool enabled) noexcept { reference_guides_ = enabled; }
  void set_ground_speed(float knots, bool valid) noexcept {
    const auto display = ground_speed_display(knots, valid);
    ground_speed_hidden_ = false;
    ground_speed_valid_ = display.valid;
    ground_speed_ = display.knots;
  }
  // Keep the default GS panel geometry for font fixtures. When hidden, the
  // overlay is skipped entirely so no black rectangle appears on the ND.
  void hide_ground_speed() noexcept {
    ground_speed_hidden_ = true;
    ground_speed_valid_ = false;
    ground_speed_ = 0;
  }
  // Recorded root constants capture this value. No resource/descriptors change.
  bool set_display_exposure(float ev) noexcept {
    if (!std::isfinite(ev))
      return false;
    exposure_ev_ = std::clamp(ev, MinimumExposureEv, MaximumExposureEv);
    return true;
  }
  // The simulator's main-view tone mapping for HDR feeds (ToneShader notes):
  // its eye-adaptation exposure (the 1 x 1 ph_lumadaptation value) and its
  // 64^3 tone-curve table (R10G10B10A2 texels, x fastest, then y, then z).
  // A null table keeps the last one; the curve applies once a table exists.
  // Exposure is a recorded root constant. A table is copied into the upload
  // buffer at once, so like set_inputs it requires that no recording made by
  // this compositor is still executing. Non-finite or non-positive exposure
  // is refused and the Taxi Cam exposure applies until clear or a valid call.
  static constexpr UINT ToneTexels = 64;
  static constexpr std::size_t ToneTableTexels = std::size_t{ToneTexels} * ToneTexels * ToneTexels;
  bool set_tone_curve(float exposure, const std::uint32_t* table) noexcept {
    if (!std::isfinite(exposure) || exposure <= 0 || exposure > 1e6f || !tone_mapped_) {
      tone_exposure_ = 0;
      return false;
    }
    tone_exposure_ = exposure;
    if (table) {
      for (UINT slice = 0; slice < ToneTexels * ToneTexels; ++slice)
        std::memcpy(tone_mapped_ + std::size_t{slice} * ToneRowPitch, table + std::size_t{slice} * ToneTexels, ToneTexels * 4);
      tone_pending_ = true;
    }
    return true;
  }
  void clear_tone_curve() noexcept { tone_exposure_ = 0; }
  static const char* built_in_shader() noexcept { return Shader; }
  // Scene light for an aircraft display (ScreenShader notes): the camera's HDR
  // texels times `scale`, so that the display's own emissive conversion gives
  // back the camera's scene light and the simulator exposes and tonemaps it
  // once, like the world outside. scale is 1 / (display light at full code in
  // texel units). Takes precedence over the tone curve; 0 turns it off.
  // floor: light already falling on the display, as a fraction of its
  // full-code light; it is subtracted so the display adds it back.
  bool set_screen_scale(float scale, float floor = 0) noexcept {
    if (!std::isfinite(scale) || scale < 0 || scale > 1e6f || !std::isfinite(floor) || floor < 0 || floor >= 1) {
      screen_scale_ = 0;
      return false;
    }
    screen_scale_ = scale;
    screen_floor_ = floor;
    return true;
  }
  float screen_scale() const noexcept { return screen_scale_; }
  // Live lighting inputs for every pass of the output shader, whichever mode
  // is selected: the simulator's main-view exposure (0: none), the decoded
  // display's camera-texel-to-code scale (0: none) and A:AMBIENT LIGHT SENSOR.
  void set_light_inputs(float main_exposure, float display_scale, float ambient) noexcept {
    main_exposure_ = std::isfinite(main_exposure) && main_exposure > 0 ? main_exposure : 0;
    display_scale_ = std::isfinite(display_scale) && display_scale > 0 ? display_scale : 0;
    ambient_ = std::isfinite(ambient) ? ambient : -1;
  }
  // Development: replaces the output shader with `source` (same entry points,
  // bindings and constants as Shader). The previous pipeline is retained
  // until release, so recordings that still use it stay valid. A failed
  // compile keeps the current pipeline; last_error() has the compiler text.
  HRESULT reload_shader(const char* source, std::size_t size) noexcept {
    if (!pipeline_.get() || !source || !size)
      return fail(E_INVALIDARG, "A compiled compositor and shader source are required.");
    if (retired_count_ == retired_pipelines_.size())
      return fail(E_OUTOFMEMORY, "Too many shader reloads; restart the simulator to reload again.");
    Reference<ID3DBlob> vertex;
    Reference<ID3DBlob> pixel;
    HRESULT status = compile("vs_main", "vs_5_0", vertex.put(), false, source, size);
    if (FAILED(status) || FAILED(status = compile("ps_main", "ps_5_0", pixel.put(), false, source, size)))
      return status;
    auto pipeline = output_pipeline_description(vertex.get(), pixel.get());
    ID3D12PipelineState* created = nullptr;
    status = device_->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&created));
    if (FAILED(status))
      return fail(status, "Creating the reloaded compositor pipeline failed.");
    retired_pipelines_[retired_count_++] = pipeline_.get();
    *pipeline_.put() = created;
    error_[0] = '\0';
    return S_OK;
  }
  bool tone_curve_active() const noexcept { return tone_exposure_ > 0 && (tone_ready_ || tone_pending_); }

  HRESULT initialize(ID3D12Device* device) noexcept {
    if (!device)
      return fail(E_INVALIDARG, "A native D3D12 device is required.");
    if (device_.get())
      return same_object(device_.get(), device) ? S_FALSE : fail(E_INVALIDARG, "The compositor already belongs to another device.");
    device_.retain(device);
    HRESULT status = initialize_pipeline();
    if (FAILED(status)) {
      release();
      return status;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = SrvCount;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    status = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(srv_heap_.put()));
    if (FAILED(status))
      return initialization_failed(status, "Creating the source and bloom SRV heap failed.");
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = RtvCount;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    status = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(rtv_heap_.put()));
    if (FAILED(status))
      return initialization_failed(status, "Creating the output RTV heap failed.");

    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    properties.CreationNodeMask = properties.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = Width;
    texture.Height = Height;
    texture.DepthOrArraySize = texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    status = device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                             IID_PPV_ARGS(output_.put()));
    if (FAILED(status))
      return initialization_failed(status, "Creating the compositor output texture failed.");
    rtv_ = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(output_.get(), nullptr, rtv_);
    ++statistics_.descriptor_writes;
    status = initialize_tone_curve();
    if (FAILED(status))
      return initialization_failed(status, "Creating the tone-curve table failed.");
    error_[0] = '\0';
    return S_OK;
  }

  // Nose, bottom-left (or full-width tail), and bottom-right. Non-split layouts
  // may pass the same resource for both bottom inputs; split_bottom requires three
  // distinct captures so each wing has its own mount.
  // descriptor_writes counts feed bindings: exactly two when the bottoms alias
  // (t2 still gets CreateShaderResourceView for the same tail so the three-SRV
  // root table stays valid; that fill is not a third feed), exactly three when
  // the bottoms are distinct. Never CopyDescriptorsSimple from this shader-visible
  // heap — it is CPU-write-only as a copy source.
  HRESULT set_inputs(ID3D12Resource* nose, DXGI_FORMAT nose_format, ID3D12Resource* tail_left, DXGI_FORMAT left_format,
                     ID3D12Resource* tail_right, DXGI_FORMAT right_format) noexcept {
    if (!output_.get())
      return fail(E_UNEXPECTED, "Initialize the compositor before binding inputs.");
    if (!nose || !tail_left || !tail_right || same_object(nose, tail_left) || same_object(nose, tail_right) ||
        same_object(nose, output_.get()) || same_object(tail_left, output_.get()) || same_object(tail_right, output_.get()))
      return fail(E_INVALIDARG, "Nose and bottom inputs must be non-null and must not alias the compositor output.");
    const bool shared_bottom = same_object(tail_left, tail_right);
    if (composition_.split_bottom != 0 && shared_bottom)
      return fail(E_INVALIDARG, "Split-bottom layouts need distinct left and right bottom captures.");
    if (shared_bottom && left_format != right_format)
      return fail(E_INVALIDARG, "Aliased bottom inputs must share the same typed SRV format.");
    if (same_object(nose, inputs_[0].get()) && same_object(tail_left, inputs_[1].get()) && same_object(tail_right, inputs_[2].get()) &&
        nose_format == formats_[0] && left_format == formats_[1] && right_format == formats_[2]) {
      error_[0] = '\0';
      return S_FALSE;
    }
    std::array<D3D12_RESOURCE_DESC, 3> descriptions{};
    if (!validate_input(nose, nose_format, descriptions[0]) || !validate_input(tail_left, left_format, descriptions[1]) ||
        !validate_input(tail_right, right_format, descriptions[2]))
      return fail(E_INVALIDARG, "An input has an unsupported device, texture description, heap or typed SRV format.");
    const std::array<ID3D12Resource*, 3> resources{nose, tail_left, tail_right};
    const std::array<DXGI_FORMAT, 3> formats{nose_format, left_format, right_format};
    auto handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    const UINT stride = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (std::size_t index = 0; index < resources.size(); ++index) {
      D3D12_SHADER_RESOURCE_VIEW_DESC view{};
      view.Format = formats[index];
      view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      view.Texture2D.MipLevels = 1;
      device_->CreateShaderResourceView(resources[index], &view, handle);
      inputs_[index].retain(resources[index]);
      handle.ptr += stride;
    }
    formats_ = formats;
    input_descriptions_ = descriptions;
    for (std::size_t index = 0; index < resources.size(); ++index) {
      const bool hdr = formats[index] == DXGI_FORMAT_R11G11B10_FLOAT;
      if (index == 2 && shared_bottom) {
        bloom_view(2, hdr ? pyramids_[1].bloom.get() : nullptr);
        continue;
      }
      if (!hdr) {
        pyramids_[index].reset();
        bloom_view(index, nullptr);
        continue;
      }
      if (FAILED(prepare_pyramid(index, descriptions[index]))) {
        pyramids_[index].reset();
        bloom_view(index, nullptr);
      }
    }
    statistics_.descriptor_writes += shared_bottom ? 2u : 3u;
    ++statistics_.input_changes;
    error_[0] = '\0';
    return S_OK;
  }

  // Convenience for full-width two-feed profiles: both bottom SRVs sample the same tail.
  HRESULT set_inputs(ID3D12Resource* nose, DXGI_FORMAT nose_format, ID3D12Resource* tail, DXGI_FORMAT tail_format) noexcept {
    return set_inputs(nose, nose_format, tail, tail_format, tail, tail_format);
  }

  // Supported exact before states: COMMON, RENDER_TARGET, UNORDERED_ACCESS,
  // COPY_DEST, or any nonempty combination of PIXEL_SHADER_RESOURCE,
  // NON_PIXEL_SHADER_RESOURCE and COPY_SOURCE. Other state sets are refused.
  // Non-split layouts may bind the same tail resource to both bottom SRVs; each
  // distinct resource is transitioned once so aliased bottoms do not double-barrier.
  HRESULT record(ID3D12GraphicsCommandList* private_list, D3D12_RESOURCE_STATES nose_before, D3D12_RESOURCE_STATES left_before,
                 D3D12_RESOURCE_STATES right_before) noexcept {
    if (!output_.get() || !inputs_[0].get() || !inputs_[1].get() || !inputs_[2].get() || !private_list ||
        private_list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !same_device(private_list) ||
        !valid_state(nose_before, input_descriptions_[0]) || !valid_state(left_before, input_descriptions_[1]) ||
        !valid_state(right_before, input_descriptions_[2]))
      return fail(E_INVALIDARG, "A private direct list, bound inputs and supported exact mip-zero states are required.");
    const bool shared_bottom = same_object(inputs_[1].get(), inputs_[2].get());
    if (shared_bottom && left_before != right_before)
      return fail(E_INVALIDARG, "Aliased bottom inputs must share the same before state.");
    const std::array<D3D12_RESOURCE_STATES, 3> states{nose_before, left_before, right_before};
    for (std::size_t index = 0; index < inputs_.size(); ++index) {
      if (index == 2 && shared_bottom)
        continue;
      transition(private_list, inputs_[index].get(), states[index], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    UINT hdr = (formats_[0] == DXGI_FORMAT_R11G11B10_FLOAT ? 1u : 0u) | (formats_[1] == DXGI_FORMAT_R11G11B10_FLOAT ? 2u : 0u) |
               (formats_[2] == DXGI_FORMAT_R11G11B10_FLOAT ? 4u : 0u);
    for (UINT index = 0; index < 3; ++index) {
      const UINT source = index == 2 && shared_bottom ? 1u : index;
      if (!(hdr & (1u << index)) || pyramids_[source].levels < 2)
        continue;
      if (index == source)
        record_bloom(private_list, index);
      hdr |= 16u << index;
    }
    if (tone_pending_) {
      transition(private_list, tone_table_.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION destination{}, source{};
      destination.pResource = tone_table_.get();
      destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      source.pResource = tone_upload_.get();
      source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      source.PlacedFootprint.Footprint = {ToneFormat, ToneTexels, ToneTexels, ToneTexels, ToneRowPitch};
      private_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
      transition(private_list, tone_table_.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      tone_pending_ = false;
      tone_ready_ = true;
    }
    if (screen_scale_ > 0)
      hdr |= ScreenBit;
    else if (tone_exposure_ > 0 && tone_ready_)
      hdr |= ToneBit;
    transition(private_list, output_.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    draw_output(private_list, hdr, ground_speed_hidden_ ? 2u : (ground_speed_valid_ ? 1u : 0u));
    transition(private_list, output_.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (std::size_t index = 0; index < inputs_.size(); ++index) {
      if (index == 2 && shared_bottom)
        continue;
      transition(private_list, inputs_[index].get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, states[index]);
    }
    ++statistics_.recordings;
    error_[0] = '\0';
    return S_OK;
  }

  HRESULT record(ID3D12GraphicsCommandList* private_list, D3D12_RESOURCE_STATES nose_before, D3D12_RESOURCE_STATES tail_before) noexcept {
    return record(private_list, nose_before, tail_before, tail_before);
  }

  // Black page with PLEASE WAIT centred in the GS font and speed colour. No
  // input is sampled or transitioned. A compositor that has never had inputs
  // writes null SRVs once, before its first submission, so the root table
  // stays valid. Leaves output COPY_SOURCE, as record() does.
  HRESULT record_waiting(ID3D12GraphicsCommandList* private_list) noexcept {
    if (!output_.get() || !private_list || private_list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !same_device(private_list))
      return fail(E_INVALIDARG, "A private direct list is required for the waiting page.");
    if (!inputs_[0].get() && !null_inputs_) {
      auto handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
      const UINT stride = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      D3D12_SHADER_RESOURCE_VIEW_DESC view{};
      view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      view.Texture2D.MipLevels = 1;
      for (std::size_t index = 0; index < inputs_.size(); ++index, handle.ptr += stride)
        device_->CreateShaderResourceView(nullptr, &view, handle);
      for (std::size_t index = 0; index < pyramids_.size(); ++index)
        bloom_view(index, nullptr);
      null_inputs_ = true;
    }
    transition(private_list, output_.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    draw_output(private_list, 0, WaitingPageMode);
    transition(private_list, output_.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ++statistics_.recordings;
    error_[0] = '\0';
    return S_OK;
  }

  // Call only after checked GPU completion, including downstream output copies.
  void release() noexcept {
    for (auto& input : inputs_)
      input.reset();
    for (auto& pyramid : pyramids_)
      pyramid.reset();
    output_.reset();
    srv_heap_.reset();
    rtv_heap_.reset();
    pipeline_.reset();
    for (std::size_t index = 0; index < retired_count_; ++index)
      retired_pipelines_[index]->Release();
    retired_count_ = 0;
    root_signature_.reset();
    down_pipeline_.reset();
    up_pipeline_.reset();
    bloom_signature_.reset();
    if (tone_mapped_ && tone_upload_.get())
      tone_upload_->Unmap(0, nullptr);
    tone_mapped_ = nullptr;
    tone_upload_.reset();
    tone_table_.reset();
    tone_exposure_ = 0;
    tone_pending_ = tone_ready_ = false;
    device_.reset();
    formats_ = {};
    input_descriptions_ = {};
    rtv_ = {};
  }

  // Exceptional shutdown only when completion cannot be established. Retain
  // native references until process termination instead of freeing in-flight GPU
  // resources. This intentionally leaks; it is not normal resource management.
  void abandon() noexcept {
    for (auto& input : inputs_)
      input.abandon();
    for (auto& pyramid : pyramids_) {
      pyramid.scene.abandon();
      pyramid.bloom.abandon();
    }
    output_.abandon();
    srv_heap_.abandon();
    rtv_heap_.abandon();
    pipeline_.abandon();
    retired_count_ = 0;
    root_signature_.abandon();
    down_pipeline_.abandon();
    up_pipeline_.abandon();
    bloom_signature_.abandon();
    tone_mapped_ = nullptr;
    tone_upload_.abandon();
    tone_table_.abandon();
    device_.abandon();
  }

 private:
  template <typename T>
  class Reference {
   public:
    Reference() = default;
    Reference(const Reference&) = delete;
    Reference& operator=(const Reference&) = delete;
    ~Reference() { reset(); }
    T* get() const noexcept { return value_; }
    T* operator->() const noexcept { return value_; }
    T** put() noexcept { return &value_; }
    void reset() noexcept {
      if (value_)
        value_->Release();
      value_ = nullptr;
    }
    void retain(T* value) noexcept {
      if (value)
        value->AddRef();
      reset();
      value_ = value;
    }
    void abandon() noexcept { value_ = nullptr; }

   private:
    T* value_ = nullptr;
  };

  static bool same_object(IUnknown* first, IUnknown* second) noexcept {
    if (!first || !second)
      return false;
    Reference<IUnknown> first_identity;
    Reference<IUnknown> second_identity;
    return SUCCEEDED(first->QueryInterface(IID_PPV_ARGS(first_identity.put()))) &&
           SUCCEEDED(second->QueryInterface(IID_PPV_ARGS(second_identity.put()))) && first_identity.get() == second_identity.get();
  }

  bool same_device(ID3D12DeviceChild* child) const noexcept { return same_native_device(child, device_.get()); }

  // The simulator's bloom, per HDR feed (BloomShader): image levels 1..L
  // (scene: mip k holds image level k+1) and the bloom chain (bloom: mip k at
  // the same size). Both start at half the input size and go down to 1 x 1.
  struct Pyramid {
    Reference<ID3D12Resource> scene;
    Reference<ID3D12Resource> bloom;
    UINT width = 0, height = 0, levels = 0;
    void reset() noexcept {
      scene.reset();
      bloom.reset();
      width = height = levels = 0;
    }
  };
  // SRVs: three inputs, the output shader's three bloom views, then per feed
  // MaxLevels scene-mip and MaxLevels bloom-mip views. RTVs: the output, then
  // the same per-feed mip layout.
  static constexpr UINT MaxLevels = 14;
  static constexpr UINT ToneSlot = 6 + 3 * 2 * MaxLevels;
  static constexpr UINT SrvCount = ToneSlot + 1;
  static constexpr UINT RtvCount = 1 + 3 * 2 * MaxLevels;
  static constexpr DXGI_FORMAT BloomFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
  static constexpr UINT pyramid_slot(std::size_t feed, bool bloom, UINT level) noexcept {
    return static_cast<UINT>(feed) * 2 * MaxLevels + (bloom ? MaxLevels : 0) + level;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(UINT index) const noexcept {
    auto handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T{index} * device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return handle;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu(UINT index) const noexcept {
    auto handle = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += UINT64{index} * device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return handle;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu(UINT index) const noexcept {
    auto handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T{index} * device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return handle;
  }
  static UINT mip_size(UINT base, UINT level) noexcept { return (std::max)(1u, base >> level); }

  // The output shader's bloom view of feed `index` (t3..t5); null when absent.
  void bloom_view(std::size_t index, ID3D12Resource* bloom) noexcept {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = BloomFormat;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(bloom, &view, srv_cpu(3 + static_cast<UINT>(index)));
  }

  // Allocates feed `index`'s pyramid when its size changes and writes its views.
  HRESULT prepare_pyramid(std::size_t index, const D3D12_RESOURCE_DESC& input) noexcept {
    auto& pyramid = pyramids_[index];
    const UINT width = (std::max)(1u, static_cast<UINT>(input.Width) >> 1);
    const UINT height = (std::max)(1u, input.Height >> 1);
    UINT levels = 1;
    while (levels < MaxLevels && ((std::max)(width, height) >> levels) > 0)
      ++levels;
    if (pyramid.scene.get() && pyramid.width == width && pyramid.height == height) {
      bloom_view(index, pyramid.bloom.get());
      return S_OK;
    }
    pyramid.reset();
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    properties.CreationNodeMask = properties.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = width;
    texture.Height = height;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = static_cast<UINT16>(levels);
    texture.Format = BloomFormat;
    texture.SampleDesc.Count = 1;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (auto* target : {&pyramid.scene, &pyramid.bloom}) {
      const HRESULT status = device_->CreateCommittedResource(
          &properties, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(target->put()));
      if (FAILED(status)) {
        pyramid.reset();
        return status;
      }
    }
    for (UINT level = 0; level < levels; ++level) {
      for (const bool bloom : {false, true}) {
        auto* resource = bloom ? pyramid.bloom.get() : pyramid.scene.get();
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = BloomFormat;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MostDetailedMip = level;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(resource, &view, srv_cpu(6 + pyramid_slot(index, bloom, level)));
        D3D12_RENDER_TARGET_VIEW_DESC target{};
        target.Format = BloomFormat;
        target.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        target.Texture2D.MipSlice = level;
        device_->CreateRenderTargetView(resource, &target, rtv_cpu(1 + pyramid_slot(index, bloom, level)));
      }
    }
    pyramid.width = width;
    pyramid.height = height;
    pyramid.levels = levels;
    bloom_view(index, pyramid.bloom.get());
    return S_OK;
  }

  struct BloomPass {
    float target_texel[2];
    float source_texel[2];
    float previous_size[2];
    float tent_weight;
    float previous_weight;
  };
  void bloom_pass(ID3D12GraphicsCommandList* list,
                  ID3D12Resource* target,
                  UINT rtv,
                  UINT level,
                  UINT width,
                  UINT height,
                  UINT source,
                  UINT previous,
                  const BloomPass& constants) noexcept {
    transition(list, target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET, level);
    list->SetGraphicsRootDescriptorTable(0, srv_gpu(source));
    list->SetGraphicsRootDescriptorTable(1, srv_gpu(previous));
    list->SetGraphicsRoot32BitConstants(2, 8, &constants, 0);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    const auto handle = rtv_cpu(rtv);
    list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, level);
  }

  // ch_generate_mips_filtered for image levels 1..L, then ch_bloom's chain from
  // the coarsest level down to the half-size bloom the output shader samples.
  void record_bloom(ID3D12GraphicsCommandList* list, std::size_t index) noexcept {
    const auto& pyramid = pyramids_[index];
    const UINT levels = pyramid.levels;
    list->SetGraphicsRootSignature(bloom_signature_.get());
    ID3D12DescriptorHeap* heaps[]{srv_heap_.get()};
    list->SetDescriptorHeaps(1, heaps);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->SetPipelineState(down_pipeline_.get());
    UINT source_width = static_cast<UINT>(input_descriptions_[index].Width);
    UINT source_height = input_descriptions_[index].Height;
    for (UINT level = 0; level < levels; ++level) {
      const UINT width = mip_size(pyramid.width, level), height = mip_size(pyramid.height, level);
      const BloomPass constants{{1.f / width, 1.f / height}, {1.f / source_width, 1.f / source_height}, {}, 0, 0};
      const UINT source = level ? 6 + pyramid_slot(index, false, level - 1) : static_cast<UINT>(index);
      bloom_pass(list, pyramid.scene.get(), 1 + pyramid_slot(index, false, level), level, width, height, source, source, constants);
      source_width = width;
      source_height = height;
    }
    // bloom[k] = c/(k+1) tent(image level k+1) + B-spline(previous), with
    // c = 1/H(L) so the level weights 1/m sum to one. The coarsest step's
    // previous is image level L itself, weighted c/L.
    double harmonic = 0;
    for (UINT m = 1; m <= levels; ++m)
      harmonic += 1.0 / m;
    const double c = 1.0 / harmonic;
    list->SetPipelineState(up_pipeline_.get());
    for (UINT level = levels - 1; level-- > 0;) {
      const bool first = level == levels - 2;
      const UINT width = mip_size(pyramid.width, level), height = mip_size(pyramid.height, level);
      const UINT previous_width = mip_size(pyramid.width, level + 1), previous_height = mip_size(pyramid.height, level + 1);
      const BloomPass constants{{1.f / width, 1.f / height},
                                {1.f / previous_width, 1.f / previous_height},
                                {static_cast<float>(previous_width), static_cast<float>(previous_height)},
                                static_cast<float>(c / (level + 1)),
                                first ? static_cast<float>(c / levels) : 1.f};
      bloom_pass(list, pyramid.bloom.get(), 1 + pyramid_slot(index, true, level), level, width, height,
                 6 + pyramid_slot(index, false, level), 6 + pyramid_slot(index, !first, level + 1), constants);
    }
  }

  static constexpr DXGI_FORMAT ToneFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  static constexpr UINT ToneRowPitch = 256;  // 64 texels x 4 bytes, already D3D12-aligned
  static constexpr UINT ToneBit = 256;
  static constexpr UINT RootConstants = 39;
  static constexpr UINT ScreenBit = 512;
  HRESULT initialize_tone_curve() noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    properties.CreationNodeMask = properties.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    texture.Width = texture.Height = ToneTexels;
    texture.DepthOrArraySize = ToneTexels;
    texture.MipLevels = 1;
    texture.Format = ToneFormat;
    texture.SampleDesc.Count = 1;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    HRESULT status = device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &texture,
                                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(tone_table_.put()));
    if (FAILED(status))
      return status;
    properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = UINT64{ToneRowPitch} * ToneTexels * ToneTexels;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    status = device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(tone_upload_.put()));
    if (FAILED(status))
      return status;
    const D3D12_RANGE none{0, 0};
    void* mapped = nullptr;
    status = tone_upload_->Map(0, &none, &mapped);
    if (FAILED(status))
      return status;
    tone_mapped_ = static_cast<unsigned char*>(mapped);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = ToneFormat;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture3D.MipLevels = 1;
    device_->CreateShaderResourceView(tone_table_.get(), &view, srv_cpu(ToneSlot));
    return S_OK;
  }

  // Ground-speed modes in the shader constants: 0 unavailable, 1 valid,
  // 2 hidden, and 3 the waiting page.
  static constexpr UINT WaitingPageMode = 3;
  void draw_output(ID3D12GraphicsCommandList* private_list, UINT hdr, UINT ground_speed_mode) noexcept {
    private_list->SetGraphicsRootSignature(root_signature_.get());
    private_list->SetPipelineState(pipeline_.get());
    ID3D12DescriptorHeap* heaps[]{srv_heap_.get()};
    private_list->SetDescriptorHeaps(1, heaps);
    private_list->SetGraphicsRootDescriptorTable(0, srv_heap_->GetGPUDescriptorHandleForHeapStart());
    private_list->SetGraphicsRootDescriptorTable(2, srv_gpu(ToneSlot));
    // The simulator's exposure chain: scene units (16 x texel), its adapted
    // exposure, and its fixed 11190.6 and 300/10^4 scales (ToneShader notes).
    const float exposure = (hdr & ScreenBit) ? screen_scale_
                           : (hdr & ToneBit) ? static_cast<float>(11190.6 * 16 * 300e-4) * tone_exposure_
                                             : std::exp2(exposure_ev_);
    const struct {
      UINT hdr_mask;
      float exposure;
      UINT guides;
      UINT ground_speed;
      UINT ground_speed_valid;
      profiles::Composition composition;
      float main_exposure;
      float display_scale;
      float ambient;
      float taxi_exposure;
      float screen_floor;
    } display{hdr,
              exposure,
              reference_guides_ ? 1u : 0u,
              ground_speed_,
              ground_speed_mode,
              composition_,
              static_cast<float>(11190.6 * 16 * 300e-4) * main_exposure_,
              display_scale_,
              ambient_,
              std::exp2(exposure_ev_),
              screen_floor_};
    static_assert(sizeof(display) == RootConstants * sizeof(UINT));
    private_list->SetGraphicsRoot32BitConstants(1, RootConstants, &display, 0);
    private_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(Width), static_cast<float>(Height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height)};
    private_list->RSSetViewports(1, &viewport);
    private_list->RSSetScissorRects(1, &scissor);
    private_list->OMSetRenderTargets(1, &rtv_, FALSE, nullptr);
    private_list->DrawInstanced(3, 1, 0, 0);
  }

  static DXGI_FORMAT typeless_family(DXGI_FORMAT format) noexcept {
    switch (format) {
      case DXGI_FORMAT_R11G11B10_FLOAT:
        // This packed format has no typeless alias. Admit the exact typed
        // resource/SRV only. The display shader applies its explicit HDR path.
        return DXGI_FORMAT_R11G11B10_FLOAT;
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
      default:
        return DXGI_FORMAT_UNKNOWN;
    }
  }

  bool validate_input(ID3D12Resource* resource, DXGI_FORMAT view_format, D3D12_RESOURCE_DESC& description) const noexcept {
    if (!same_device(resource) || typeless_family(view_format) == DXGI_FORMAT_UNKNOWN)
      return false;
    // MinGW's COM headers expose the Microsoft aggregate-return ABI explicitly.
#if defined(__MINGW32__)
#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#error This MinGW build requires explicit Microsoft COM aggregate-return wrappers.
#endif
    resource->GetDesc(&description);
#else
    description = resource->GetDesc();
#endif
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.Width == 0 || description.Width > 16384 ||
        description.Height == 0 || description.Height > 16384 || description.DepthOrArraySize != 1 || description.MipLevels == 0 ||
        description.SampleDesc.Count != 1 || description.SampleDesc.Quality != 0 ||
        (description.Flags & (D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0 ||
        (description.Format != view_format && description.Format != typeless_family(view_format)))
      return false;
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_HEAP_FLAGS flags{};
    if (FAILED(resource->GetHeapProperties(&heap, &flags)) || heap.Type != D3D12_HEAP_TYPE_DEFAULT)
      return false;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{view_format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
    const auto required = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
    return SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
           (support.Support1 & required) == required;
  }

  static bool valid_state(D3D12_RESOURCE_STATES state, const D3D12_RESOURCE_DESC& input) noexcept {
    if (state == D3D12_RESOURCE_STATE_COMMON || state == D3D12_RESOURCE_STATE_COPY_DEST)
      return true;
    if (state == D3D12_RESOURCE_STATE_RENDER_TARGET)
      return (input.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
      return (input.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    constexpr UINT ReadStates =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
    return (static_cast<UINT>(state) & ~ReadStates) == 0;
  }

  static void transition(ID3D12GraphicsCommandList* list,
                         ID3D12Resource* texture,
                         D3D12_RESOURCE_STATES before,
                         D3D12_RESOURCE_STATES after,
                         UINT subresource = 0) noexcept {
    if (before == after)
      return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = subresource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
  }

  HRESULT fail(HRESULT status, const char* message) noexcept {
    const auto size = (std::min)(std::strlen(message), error_.size() - 1);
    std::memcpy(error_.data(), message, size);
    error_[size] = '\0';
    return status;
  }

  HRESULT initialization_failed(HRESULT status, const char* message) noexcept {
    fail(status, message);
    release();
    return status;
  }

  HRESULT compile(const char* entry,
                  const char* profile,
                  ID3DBlob** bytecode,
                  bool bloom = false,
                  const char* replacement = nullptr,
                  std::size_t replacement_size = 0) noexcept {
    Reference<ID3DBlob> diagnostics;
    const char* source = replacement ? replacement : bloom ? BloomShader : Shader;
    const std::size_t size = replacement ? replacement_size : bloom ? sizeof(BloomShader) - 1 : sizeof(Shader) - 1;
    const HRESULT status = D3DCompile(source, size, bloom ? "camera_bloom" : "camera_compositor", nullptr, nullptr, entry, profile,
                                      D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0,
                                      bytecode, diagnostics.put());
    ++statistics_.shader_compiles;
    if (FAILED(status)) {
      if (diagnostics.get()) {
        const auto size = (std::min)(diagnostics->GetBufferSize(), error_.size() - 1);
        std::memcpy(error_.data(), diagnostics->GetBufferPointer(), size);
        error_[size] = '\0';
      } else {
        fail(status, "Compiling the compositor shader failed.");
      }
    }
    return status;
  }

  HRESULT initialize_pipeline() noexcept {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 6;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = RootConstants;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_DESCRIPTOR_RANGE tone_range{};
    tone_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    tone_range.NumDescriptors = 1;
    tone_range.BaseShaderRegister = 6;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &tone_range;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = static_cast<UINT>(parameters.size());
    signature.pParameters = parameters.data();
    signature.NumStaticSamplers = 1;
    signature.pStaticSamplers = &sampler;
    Reference<ID3DBlob> serialized;
    Reference<ID3DBlob> diagnostics;
    HRESULT status = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), diagnostics.put());
    if (FAILED(status))
      return fail(status, "Serializing the compositor root signature failed.");
    status =
        device_->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(root_signature_.put()));
    if (FAILED(status))
      return fail(status, "Creating the compositor root signature failed.");
    Reference<ID3DBlob> vertex;
    Reference<ID3DBlob> pixel;
    status = compile("vs_main", "vs_5_0", vertex.put());
    if (FAILED(status))
      return status;
    status = compile("ps_main", "ps_5_0", pixel.put());
    if (FAILED(status))
      return status;
    auto pipeline = output_pipeline_description(vertex.get(), pixel.get());
    status = device_->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(pipeline_.put()));
    if (FAILED(status))
      return fail(status, "Creating the compositor pipeline failed.");
    return initialize_bloom_pipelines(pipeline);
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC output_pipeline_description(ID3DBlob* vertex, ID3DBlob* pixel) const noexcept {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root_signature_.get();
    pipeline.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    pipeline.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    auto& blend = pipeline.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.SampleMask = UINT_MAX;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline.DepthStencilState.StencilReadMask = pipeline.DepthStencilState.StencilWriteMask = 0xff;
    pipeline.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                            D3D12_COMPARISON_FUNC_ALWAYS};
    pipeline.DepthStencilState.BackFace = pipeline.DepthStencilState.FrontFace;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipeline.SampleDesc.Count = 1;
    return pipeline;
  }

  // Two single-SRV tables (t0 source, t1 previous level) and eight constants.
  HRESULT initialize_bloom_pipelines(D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline) noexcept {
    std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{};
    std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
    for (UINT index = 0; index < 2; ++index) {
      ranges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      ranges[index].NumDescriptors = 1;
      ranges[index].BaseShaderRegister = index;
      parameters[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameters[index].DescriptorTable.NumDescriptorRanges = 1;
      parameters[index].DescriptorTable.pDescriptorRanges = &ranges[index];
      parameters[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[2].Constants.Num32BitValues = 8;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = static_cast<UINT>(parameters.size());
    signature.pParameters = parameters.data();
    signature.NumStaticSamplers = 1;
    signature.pStaticSamplers = &sampler;
    Reference<ID3DBlob> serialized;
    Reference<ID3DBlob> diagnostics;
    HRESULT status = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), diagnostics.put());
    if (FAILED(status))
      return fail(status, "Serializing the bloom root signature failed.");
    status =
        device_->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(bloom_signature_.put()));
    if (FAILED(status))
      return fail(status, "Creating the bloom root signature failed.");
    Reference<ID3DBlob> vertex;
    Reference<ID3DBlob> down;
    Reference<ID3DBlob> up;
    if (FAILED(status = compile("vs_main", "vs_5_0", vertex.put(), true)) ||
        FAILED(status = compile("ps_down", "ps_5_0", down.put(), true)) || FAILED(status = compile("ps_up", "ps_5_0", up.put(), true)))
      return status;
    pipeline.pRootSignature = bloom_signature_.get();
    pipeline.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    pipeline.RTVFormats[0] = BloomFormat;
    pipeline.PS = {down->GetBufferPointer(), down->GetBufferSize()};
    status = device_->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(down_pipeline_.put()));
    if (FAILED(status))
      return fail(status, "Creating the bloom downsample pipeline failed.");
    pipeline.PS = {up->GetBufferPointer(), up->GetBufferSize()};
    status = device_->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(up_pipeline_.put()));
    return FAILED(status) ? fail(status, "Creating the bloom upsample pipeline failed.") : S_OK;
  }

  // The simulator's bloom (1.8.16.0, PIX 2026-10-02, main view). The HDR image
  // is box-filtered into mips (ch_generate_mips_filtered; its Reinhard
  // weighting is off). ch_bloom then walks up from the coarsest mip; each step
  // adds a 3 x 3 tent of the image mip at its size, weighted c/m, to a cubic
  // B-spline upsample of the step before. The result is the sum over m of
  // (1/m)/H(L) times image level m, with no threshold and no exposure, and the
  // tonemapper mixes 10% of it into the image before exposure (BloomMix in
  // Shader). The camera views run no post-processing, so Taxi Cam runs the same
  // chain on each camera image at that image's own size.
  static constexpr char BloomShader[] = R"(
Texture2D<float4> Source : register(t0);
Texture2D<float4> Previous : register(t1);
SamplerState LinearClamp : register(s0);
cbuffer Level : register(b0) { float2 TargetTexel; float2 SourceTexel; float2 PreviousSize; float TentWeight; float PreviousWeight; };
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
}
// A non-finite camera texel must not spread through every level.
float3 finite_tap(float2 uv) {
  float3 tap = Source.SampleLevel(LinearClamp, uv, 0).rgb;
  return all(isfinite(tap)) ? clamp(tap, 0, 65504) : 0;
}
// Four bilinear taps half a source texel around the target texel's centre.
float4 ps_down(float4 position : SV_Position) : SV_Target {
  float2 uv = position.xy * TargetTexel;
  float2 h = 0.5 * SourceTexel;
  return float4((finite_tap(uv + float2(h.x, h.y)) + finite_tap(uv + float2(-h.x, h.y)) + finite_tap(uv + float2(h.x, -h.y)) +
                 finite_tap(uv - h)) * 0.25, 1);
}
// Tent: four bilinear taps on the target texel's corners. Previous: cubic
// B-spline from four bilinear taps; SourceTexel holds its texel size here.
float4 ps_up(float4 position : SV_Position) : SV_Target {
  float2 corner = (position.xy - 0.5) * TargetTexel;
  float3 tent = (Source.SampleLevel(LinearClamp, corner, 0).rgb + Source.SampleLevel(LinearClamp, corner + float2(TargetTexel.x, 0), 0).rgb +
                 Source.SampleLevel(LinearClamp, corner + float2(0, TargetTexel.y), 0).rgb +
                 Source.SampleLevel(LinearClamp, corner + TargetTexel, 0).rgb) * 0.25;
  float2 p = position.xy * TargetTexel * PreviousSize - 0.5;
  float2 f = frac(p);
  float2 i = floor(p);
  float2 f2 = f * f;
  float2 f3 = f2 * f;
  float2 w0 = (1 - 3 * f + 3 * f2 - f3) / 6;
  float2 w1 = (4 - 6 * f2 + 3 * f3) / 6;
  float2 w2 = (1 + 3 * f + 3 * f2 - 3 * f3) / 6;
  float2 w3 = f3 / 6;
  float2 g0 = w0 + w1;
  float2 g1 = w2 + w3;
  float2 h0 = (i - 0.5 + w1 / g0) * SourceTexel;
  float2 h1 = (i + 1.5 + w3 / g1) * SourceTexel;
  float3 cubic = g0.y * (g0.x * Previous.SampleLevel(LinearClamp, float2(h0.x, h0.y), 0).rgb +
                         g1.x * Previous.SampleLevel(LinearClamp, float2(h1.x, h0.y), 0).rgb) +
                 g1.y * (g0.x * Previous.SampleLevel(LinearClamp, float2(h0.x, h1.y), 0).rgb +
                         g1.x * Previous.SampleLevel(LinearClamp, float2(h1.x, h1.y), 0).rgb);
  return float4(TentWeight * tent + PreviousWeight * cubic, 1);
}
)";

  static constexpr char Shader[] = R"(
Texture2D<float4> Nose : register(t0);
Texture2D<float4> TailLeft : register(t1);
Texture2D<float4> TailRight : register(t2);
Texture2D<float4> NoseBloom : register(t3);
Texture2D<float4> TailLeftBloom : register(t4);
Texture2D<float4> TailRightBloom : register(t5);
Texture3D<float4> ToneCurve : register(t6);
SamplerState LinearClamp : register(s0);
cbuffer Display : register(b0) { uint HdrMask; float Exposure; uint ReferenceGuides; uint GroundSpeed; uint GroundSpeedValid;
 float NoseHeight; float TailTop; float DividerTop; float DividerBottom;
 float NoseDotX; float NoseDotY; float TailCornerX; float TailCornerY;
 float TailUpperX; float TailUpperY; float TailInnerX; float TailInnerY;
 float GuideRed; float GuideGreen; float GuideBlue;
 float SpeedRed; float SpeedGreen; float SpeedBlue;
 float SpeedLeft; float SpeedTop; float SpeedPaddingX; float SpeedPaddingY; float SpeedMinimumWidth; float SpeedMinimumHeight;
 float SquareNoseMarkers; float SplitBottom; float BottomGap; float BottomPaneHeight; float FrameBorder;
 // Live lighting inputs (set_light_inputs): the simulator's main-view exposure
 // times its 11190.6 x 16 x 0.03 scale (0: none), the decoded display's scale
 // (0: none), A:AMBIENT LIGHT SENSOR (-1: none) and Taxi Cam's exposure.
 float MainExposure; float DisplayScale; float Ambient; float TaxiExposure;
 // Light already falling on the display, as a fraction of its full-code light.
 float ScreenFloor; };
// Alpha 0 flags an overlay colour for the PFD stamp; see camera_pixel.
float4 ui_pixel(float3 rgb) { return float4(rgb, 0); }
float segment_distance(float2 sample_position, float2 first, float2 last) {
  float2 delta = last - first;
  return length(sample_position - (first + saturate(dot(sample_position - first, delta) / dot(delta, delta)) * delta));
}
// Original stroke lettering, defined in a 12 x 20 pixel cell. Chamfered turns
// and 1.8 pixel strokes keep the small readout legible without enlarged bitmap
// blocks. Indices 0..9 are digits, followed by G, S and the unavailable dash,
// then P, L, E, A, W, I and T for the waiting page. Each glyph is one path of
// at most 16 vertices; E, A, I and T retrace a stroke to reach their bars.
static const uint2 GlyphPaths[20] = {
  uint2(0, 9), uint2(9, 3), uint2(12, 7), uint2(19, 9), uint2(28, 4), uint2(32, 9), uint2(41, 11), uint2(52, 3), uint2(55, 16),
  uint2(71, 11), uint2(82, 10), uint2(92, 12), uint2(104, 2), uint2(106, 7), uint2(113, 3), uint2(116, 7), uint2(123, 8),
  uint2(131, 5), uint2(136, 6), uint2(142, 4)
};
static const float2 GlyphVertices[146] = {
  // 0
  float2(3.5, 1.5), float2(8.5, 1.5), float2(10.5, 3.5), float2(10.5, 16.5), float2(8.5, 18.5), float2(3.5, 18.5), float2(1.5, 16.5),
  float2(1.5, 3.5), float2(3.5, 1.5),
  // 1
  float2(3.5, 5.5), float2(6, 1.5), float2(6, 18.5),
  // 2
  float2(1.5, 4), float2(3.5, 1.5), float2(8.5, 1.5), float2(10.5, 3.5), float2(10.5, 7), float2(1.5, 18.5), float2(10.5, 18.5),
  // 3
  float2(1.5, 1.5), float2(8.5, 1.5), float2(10.5, 3.5), float2(10.5, 7.5), float2(7.5, 10), float2(10.5, 12.5), float2(10.5, 16.5),
  float2(8.5, 18.5), float2(1.5, 18.5),
  // 4
  float2(8.5, 18.5), float2(8.5, 1.5), float2(1.5, 12.5), float2(10.5, 12.5),
  // 5
  float2(10.5, 1.5), float2(1.5, 1.5), float2(1.5, 9.5), float2(8.5, 9.5), float2(10.5, 11.5), float2(10.5, 16.5), float2(8.5, 18.5),
  float2(3.5, 18.5), float2(1.5, 16.5),
  // 6
  float2(10.5, 3.5), float2(8.5, 1.5), float2(3.5, 1.5), float2(1.5, 3.5), float2(1.5, 16.5), float2(3.5, 18.5), float2(8.5, 18.5),
  float2(10.5, 16.5), float2(10.5, 11.5), float2(8.5, 9.5), float2(1.5, 9.5),
  // 7
  float2(1.5, 1.5), float2(10.5, 1.5), float2(4.5, 18.5),
  // 8
  float2(3.5, 10), float2(1.5, 7.5), float2(1.5, 3.5), float2(3.5, 1.5), float2(8.5, 1.5), float2(10.5, 3.5), float2(10.5, 7.5),
  float2(8.5, 10), float2(3.5, 10), float2(1.5, 12.5), float2(1.5, 16.5), float2(3.5, 18.5), float2(8.5, 18.5), float2(10.5, 16.5),
  float2(10.5, 12.5), float2(8.5, 10),
  // 9
  float2(1.5, 16.5), float2(3.5, 18.5), float2(8.5, 18.5), float2(10.5, 16.5), float2(10.5, 3.5), float2(8.5, 1.5), float2(3.5, 1.5),
  float2(1.5, 3.5), float2(1.5, 8.5), float2(3.5, 10.5), float2(10.5, 10.5),
  // G
  float2(10.5, 4), float2(8.5, 1.5), float2(3.5, 1.5), float2(1.5, 3.5), float2(1.5, 16.5), float2(3.5, 18.5), float2(8.5, 18.5),
  float2(10.5, 16.5), float2(10.5, 10.5), float2(6.5, 10.5),
  // S
  float2(10.5, 4), float2(8.5, 1.5), float2(3.5, 1.5), float2(1.5, 3.5), float2(1.5, 7.5), float2(3.5, 9.5), float2(8.5, 10.5),
  float2(10.5, 12.5), float2(10.5, 16.5), float2(8.5, 18.5), float2(3.5, 18.5), float2(1.5, 16),
  // dash
  float2(1.5, 10), float2(10.5, 10),
  // P
  float2(1.5, 18.5), float2(1.5, 1.5), float2(8.5, 1.5), float2(10.5, 3.5), float2(10.5, 7.5), float2(8.5, 9.5), float2(1.5, 9.5),
  // L
  float2(1.5, 1.5), float2(1.5, 18.5), float2(10.5, 18.5),
  // E
  float2(10.5, 1.5), float2(1.5, 1.5), float2(1.5, 9.5), float2(8, 9.5), float2(1.5, 9.5), float2(1.5, 18.5), float2(10.5, 18.5),
  // A
  float2(1.5, 18.5), float2(1.5, 5.5), float2(5.5, 1.5), float2(6.5, 1.5), float2(10.5, 5.5), float2(10.5, 18.5), float2(10.5, 11.5),
  float2(1.5, 11.5),
  // W
  float2(1.5, 1.5), float2(3, 18.5), float2(6, 9), float2(9, 18.5), float2(10.5, 1.5),
  // I
  float2(3, 1.5), float2(9, 1.5), float2(6, 1.5), float2(6, 18.5), float2(3, 18.5), float2(9, 18.5),
  // T
  float2(1.5, 1.5), float2(10.5, 1.5), float2(6, 1.5), float2(6, 18.5)
};
// Distance in cell units from a 12 x 20 cell position to the glyph strokes.
float glyph_distance(float2 local, uint glyph) {
  uint2 path = GlyphPaths[glyph];
  float distance = 100;
  // A fixed unroll also covers the one-segment dash without the shader
  // compiler's single-iteration warning. Short paths repeat their last segment.
  [unroll] for (uint n = 1; n < 16; ++n) {
    uint last = path.x + min(n, path.y - 1);
    distance = min(distance, segment_distance(local, GlyphVertices[last - 1], GlyphVertices[last]));
  }
  // The one has a short base; the three has a distinct middle bar.
  if (glyph == 1) distance = min(distance, segment_distance(local, float2(1.5, 18.5), float2(10.5, 18.5)));
  if (glyph == 3) distance = min(distance, segment_distance(local, float2(4.5, 10), float2(7.5, 10)));
  return distance;
}
float glyph_coverage(float2 position, float2 origin, uint glyph) {
  float2 local = position - origin;
  if (any(local < 0) || any(local >= float2(12, 20))) return 0;
  // One pixel of edge coverage around the 0.9 pixel stroke radius. The box
  // remains opaque: coverage scales the text colour, never its output alpha.
  return saturate(1.4 - glyph_distance(local, glyph));
}
// PLEASE WAIT in cell order; 20 is the space. The GS font drawn at 1.5 times
// its size keeps a one-pixel edge ramp, so the text is sharp rather than blurred.
static const uint WaitingText[11] = { 13, 14, 15, 16, 11, 15, 20, 17, 16, 18, 19 };
static const float WaitingScale = 1.5;
float4 waiting_pixel(float2 position) {
  float2 size = float2(16 * 10 + 12, 20) * WaitingScale;
  float2 origin = floor((float2(768, 763) - size) * 0.5);
  float2 local = (position - origin) / WaitingScale;
  if (any(local < 0) || local.y >= 20 || local.x >= 16 * 11) return ui_pixel(float3(0, 0, 0));
  uint cell = min((uint)(local.x / 16), 10);
  uint glyph = WaitingText[cell];
  float2 inner = local - float2(16 * cell, 0);
  if (glyph == 20 || inner.x >= 12) return ui_pixel(float3(0, 0, 0));
  float coverage = saturate(WaitingScale * (0.9 - glyph_distance(inner, glyph)) + 0.5);
  return ui_pixel(float3(SpeedRed, SpeedGreen, SpeedBlue) * coverage);
}
uint ground_speed_digits() {
  // Mode 1 is a live reading. Unavailable (0) keeps the two-digit panel width.
  return GroundSpeedValid == 1 && GroundSpeed < 10 ? 1 : 2;
}
float2 ground_speed_extent() {
  return float2(max(SpeedMinimumWidth, SpeedPaddingX * 2 + 64 + 16 * ground_speed_digits()), max(SpeedMinimumHeight, SpeedPaddingY * 2 + 20));
}
float4 ground_speed_pixel(float2 position) {
  position -= float2(SpeedPaddingX, SpeedPaddingY);
  float label = max(glyph_coverage(position, float2(0, 0), 10), glyph_coverage(position, float2(16, 0), 11));
  float speed = 0;
  if (GroundSpeedValid == 0) {
    speed = max(glyph_coverage(position, float2(64, 0), 12), glyph_coverage(position, float2(80, 0), 12));
  } else {
    uint count = ground_speed_digits();
    uint divisor = count == 2 ? 10 : 1;
    for (uint n = 0; n < count; ++n) {
      speed = max(speed, glyph_coverage(position, float2(64 + 16 * n, 0), (GroundSpeed / divisor) % 10));
      divisor /= 10;
    }
  }
  return ui_pixel(label.xxx + float3(SpeedRed, SpeedGreen, SpeedBlue) * speed);
}
// Screen-space references matched to the supplied ETACS photograph. These
// marks do not claim metric clearance after mount, attitude or FOV changes.
bool reference_guide(float2 position, bool nose) {
  if (nose) {
    float2 local = float2(min(position.x, 768 - position.x), position.y);
    float2 delta = local - float2(NoseDotX * 768, NoseDotY * NoseHeight);
    // Profiles use 14px squares; retain 12px circle support for custom layouts.
    return SquareNoseMarkers != 0 ? all(abs(delta) < 7) : length(delta) <= 6;
  }
  // Split panes mirror inside the pane. SplitBottom 0 keeps the 768-wide tail.
  float span = 768;
  float x = position.x;
  if (SplitBottom != 0) {
    float pane = (768 - BottomGap) * 0.5;
    if (position.x >= pane && position.x < pane + BottomGap)
      return false;
    x = position.x < pane ? position.x : position.x - pane - BottomGap;
    span = pane;
  }
  float2 local = float2(min(x, span - x), position.y - TailTop);
  // The reference bracket's bounding-box centre is near (0.335, 0.69);
  // its outside lower corner is farther out and below that centre.
  float2 corner = float2(TailCornerX * span, TailCornerY * (763 - TailTop));
  float2 upper = float2(TailUpperX * span, TailUpperY * (763 - TailTop));
  float2 inner = float2(TailInnerX * span, TailInnerY * (763 - TailTop));
  return min(segment_distance(local, upper, corner), segment_distance(local, corner, inner)) <= 2;
}
float hdr_channel(float value) {
  if (!isfinite(value)) return value > 0 ? 1 : 0;
  float exposed = max(value, 0) * Exposure;
  float mapped = exposed / (1 + exposed);
  return mapped <= 0.0031308 ? 12.92 * mapped : 1.055 * pow(max(mapped, 0), 1.0 / 2.4) - 0.055;
}
float srgb_code(float value) {
  return value <= 0.0031308 ? 12.92 * value : 1.055 * pow(max(value, 0), 1.0 / 2.4) - 0.055;
}
// ToneShader notes. The simulator's tonemapper (1.8.16.0, PIX 2026-10-02):
// x = scene light x adapted exposure (Exposure holds 11190.6 x 16 x E x 0.03),
// its 64^3 table read at log2(1 + x) / 13.4501 (texel centres), decoded back
// to display light, a C1 shoulder from 0.22, then sRGB. Its +-0.5/255 dither
// is not reproduced. Non-finite channels keep the Reinhard path's handling.
float3 simulator_rgb(float3 rgb) {
  float3 x = max(rgb, 0) * Exposure;
  float3 u = log2(1 + x) / 13.4501 * (63.0 / 64) + 0.5 / 64;
  float3 y = (exp2(ToneCurve.SampleLevel(LinearClamp, u, 0).rgb * 13.4501) - 1) / 11190.6 * (1e4 / 300);
  y = y < 0.22 ? y : 0.22 + 0.78 * (1 - exp2(-1.84961 * (y - 0.22)));
  y = saturate(y);
  return float3(srgb_code(y.r), srgb_code(y.g), srgb_code(y.b));
}
// ScreenShader notes. The aircraft display turns each stored code into light
// (sRGB decode times its own brightness) and the simulator then exposes and
// tonemaps the cockpit, display included. Writing the camera's scene light
// divided by the display's full-code light (Exposure) makes that conversion
// return the scene light, so the camera image gets the main view's exposure,
// tone curve and bloom exactly once. Light above the display's maximum clips.
// The +-0.5/255 dither, like the simulator's own, keeps dark night gradients,
// which use only the lowest codes, from banding.
static float2 PixelPosition;
float3 screen_rgb(float3 rgb) {
  float3 linear_light = saturate(max(rgb, 0) * Exposure - ScreenFloor);
  float noise = frac(52.9829189 * frac(dot(PixelPosition, float2(0.06711056, 0.00583715)))) - 0.5;
  return saturate(float3(srgb_code(linear_light.r), srgb_code(linear_light.g), srgb_code(linear_light.b)) + noise / 255);
}
float3 display_rgb(float3 rgb, uint feed) {
  if ((HdrMask & (1u << feed)) == 0) return rgb;
  if ((HdrMask & 512u) != 0 && all(isfinite(rgb))) return screen_rgb(rgb);
  if ((HdrMask & 256u) != 0 && all(isfinite(rgb))) return simulator_rgb(rgb);
  return float3(hdr_channel(rgb.r), hdr_channel(rgb.g), hdr_channel(rgb.b));
}
// The simulator's tonemapper mixes 10% of its bloom into the HDR image before
// exposure: lerp(image, bloom, 0.1) (1.8.16.0, PIX 2026-10-02; lens dirt off).
// The bloom of each HDR feed comes from BloomShader; HdrMask bits 4..6 say it
// was recorded. A uniform image is unchanged.
static const float BloomMix = 0.1;
float3 feed_rgb(Texture2D<float4> image, Texture2D<float4> bloom, float2 uv, uint feed) {
  float3 rgb = image.SampleLevel(LinearClamp, uv, 0).rgb;
  if ((HdrMask & (17u << feed)) != (17u << feed) || !all(isfinite(rgb))) return rgb;
  return lerp(rgb, bloom.SampleLevel(LinearClamp, uv, 0).rgb, BloomMix);
}
// Alpha 1 flags a display-referred camera code: the PFD stamp stores this byte
// on UNORM and sRGB views alike. Overlays use ui_pixel.
float4 camera_pixel(float3 rgb, uint feed) { return float4(display_rgb(rgb, feed), 1); }
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
}
// Signed distance to a rounded rect. y grows downward; radii are
// top-right, bottom-right, bottom-left, top-left. Negative is inside.
float sd_round_rect(float2 p, float2 bmin, float2 bmax, float4 radii) {
  float2 center = 0.5 * (bmin + bmax);
  float2 half_size = 0.5 * (bmax - bmin);
  float2 q = p - center;
  float r = q.x > 0 ? (q.y > 0 ? radii.y : radii.x) : (q.y > 0 ? radii.z : radii.w);
  float2 d = abs(q) - half_size + r;
  return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
}
// Split-bottom pane height; 0 keeps the square half-width pane.
float split_pane_height(float pane) { return BottomPaneHeight > 0 ? BottomPaneHeight : pane; }
float4 split_bottom_t() {
  // #1C1B22 is authored like aircraft UI: an overlay colour, not a camera code.
  // An sRGB view stores it as about #5D5C66, as it does PMDG's own UI tapes.
  return ui_pixel(float3(28.0 / 255.0, 27.0 / 255.0, 34.0 / 255.0));
}
float4 ps_main(float4 position : SV_Position) : SV_Target {
  PixelPosition = position.xy;
  // Mode 3 is the waiting page: no camera input is sampled.
  if (GroundSpeedValid == 3) return waiting_pixel(position.xy);
  // Mode 2 hides the overlay entirely (no glyphs, no black panel). Modes 0/1
  // keep the existing GS panel so font fixtures stay unchanged.
  if (GroundSpeedValid != 2) {
    float2 speed_position = position.xy - float2(SpeedLeft, SpeedTop);
    if (all(speed_position >= 0) && all(speed_position < ground_speed_extent())) return ground_speed_pixel(speed_position);
  }
  if (position.y >= DividerTop && position.y < DividerBottom) {
    if (SplitBottom != 0) {
      // Horizontal T bar: same black frame width as the other edges, only at
      // the left and right ends — not a black strip along the whole bar.
      if (position.x < FrameBorder || position.x >= 768.0 - FrameBorder) return ui_pixel(float3(0, 0, 0));
      return split_bottom_t();
    }
    return ui_pixel(float3(0, 0, 0));
  }
  if (SplitBottom != 0 && position.y >= TailTop) {
    float pane = (768 - BottomGap) * 0.5;
    // Leftover rows under the panes stay black.
    float pane_h = split_pane_height(pane);
    if (position.y >= TailTop + pane_h) return ui_pixel(float3(0, 0, 0));
    if (position.x >= pane && position.x < pane + BottomGap) return split_bottom_t();
    // Black frame on top + sides only (no bottom border). One 24 px round:
    // left pane top-right, right pane top-left. Other three corners stay square.
    const float pane_border = FrameBorder;
    const float radius = 24;
    const float inner_radius = max(radius - pane_border, 0);
    const bool left_pane = position.x < pane;
    float local_x = left_pane ? position.x : position.x - pane - BottomGap;
    float local_y = position.y - TailTop;
    float2 local = float2(local_x, local_y);
    float2 outer_min = float2(0, 0);
    float2 outer_max = float2(pane, pane_h);
    // No bottom inset: picture meets the lower edge of the square.
    float2 inner_min = float2(pane_border, pane_border);
    float2 inner_max = float2(pane - pane_border, pane_h);
    // radii: TR, BR, BL, TL
    float4 radii = left_pane ? float4(radius, 0, 0, 0) : float4(0, 0, 0, radius);
    float4 inner_radii = left_pane ? float4(inner_radius, 0, 0, 0) : float4(0, 0, 0, inner_radius);
    float d_outer = sd_round_rect(local, outer_min, outer_max, radii);
    if (d_outer > 0) {
      // Only the T-junction round can sit outside the outer shape → T grey.
      return split_bottom_t();
    }
    if (sd_round_rect(local, inner_min, inner_max, inner_radii) > 0) return ui_pixel(float3(0, 0, 0));
  }
  if (ReferenceGuides != 0 && reference_guide(position.xy, position.y < NoseHeight))
    return ui_pixel(float3(GuideRed, GuideGreen, GuideBlue));
  if (position.y < NoseHeight) {
    // Split-bottom nose frame: bottom edge always; left/right only when GS is
    // hidden so the font fixture's padded-panel surroundings stay camera pixels.
    const float nose_border = FrameBorder;
    const bool side_borders = SplitBottom != 0 && GroundSpeedValid == 2;
    if (SplitBottom != 0 && position.y >= NoseHeight - nose_border) return ui_pixel(float3(0, 0, 0));
    if (side_borders && (position.x < nose_border || position.x >= 768.0 - nose_border))
      return ui_pixel(float3(0, 0, 0));
    float left = side_borders ? nose_border : 0;
    float right = side_borders ? 768.0 - nose_border : 768.0;
    float nose_h = SplitBottom != 0 ? max(NoseHeight - nose_border, 1) : NoseHeight;
    float2 uv = float2((position.x - left) / max(right - left, 1), position.y / nose_h);
    return camera_pixel(feed_rgb(Nose, NoseBloom, uv, 0), 0);
  }
  if (position.y < TailTop) return ui_pixel(float3(0, 0, 0));
  if (SplitBottom != 0) {
    float pane = (768 - BottomGap) * 0.5;
    const float pane_border = FrameBorder;
    float pane_h = split_pane_height(pane);
    if (position.y >= TailTop + pane_h) return ui_pixel(float3(0, 0, 0));
    float local_x = position.x < pane ? position.x : position.x - pane - BottomGap;
    float local_y = position.y - TailTop;
    float2 content_min = float2(pane_border, pane_border);
    float2 content_max = float2(pane - pane_border, pane_h);
    float2 uv = float2((local_x - content_min.x) / (content_max.x - content_min.x),
                       (local_y - content_min.y) / (content_max.y - content_min.y));
    if (position.x < pane)
      return camera_pixel(feed_rgb(TailLeft, TailLeftBloom, uv, 1), 1);
    return camera_pixel(feed_rgb(TailRight, TailRightBloom, uv, 2), 2);
  }
  float2 uv = float2(position.x / 768, (position.y - TailTop) / (763 - TailTop));
  return camera_pixel(feed_rgb(TailLeft, TailLeftBloom, uv, 1), 1);
}
)";

  Reference<ID3D12Device> device_;
  Reference<ID3D12RootSignature> root_signature_;
  Reference<ID3D12PipelineState> pipeline_;
  Reference<ID3D12RootSignature> bloom_signature_;
  Reference<ID3D12PipelineState> down_pipeline_;
  Reference<ID3D12PipelineState> up_pipeline_;
  std::array<Pyramid, 3> pyramids_;
  Reference<ID3D12Resource> tone_table_;
  Reference<ID3D12Resource> tone_upload_;
  unsigned char* tone_mapped_ = nullptr;
  float tone_exposure_ = 0;
  float screen_scale_ = 0;
  float screen_floor_ = 0;
  float main_exposure_ = 0;
  float display_scale_ = 0;
  float ambient_ = -1;
  std::array<ID3D12PipelineState*, 256> retired_pipelines_{};
  std::size_t retired_count_ = 0;
  bool tone_pending_ = false;
  bool tone_ready_ = false;
  Reference<ID3D12DescriptorHeap> srv_heap_;
  Reference<ID3D12DescriptorHeap> rtv_heap_;
  Reference<ID3D12Resource> output_;
  std::array<Reference<ID3D12Resource>, 3> inputs_;
  std::array<DXGI_FORMAT, 3> formats_{};
  std::array<D3D12_RESOURCE_DESC, 3> input_descriptions_{};
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_{};
  Statistics statistics_;
  float exposure_ev_ = DefaultExposureEv;
  bool reference_guides_ = true;
  profiles::Composition composition_{};
  UINT ground_speed_ = 0;
  bool ground_speed_valid_ = false;
  bool ground_speed_hidden_ = false;
  bool null_inputs_ = false;
  std::array<char, 1024> error_{};
};

}  // namespace taxi_camera
