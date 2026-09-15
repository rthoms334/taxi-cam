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
// resources/formats change. record allocates, compiles, uploads and reads back
// nothing; it restores both input mip-zero states and leaves output COPY_SOURCE.
// RGB is sampled as declared by the typed SRV (sRGB views decode to linear).
// R11G11B10_FLOAT feeds receive exposure, per-channel Reinhard compression and
// sRGB encoding for this SDR display. Other formats retain their sampled RGB.
// This explicit display conversion is not simulator exposure/color calibration.
// Each feed stretches over its full region.
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
    ground_speed_valid_ = display.valid;
    ground_speed_ = display.knots;
  }
  // Recorded root constants capture this value. No resource/descriptors change.
  bool set_display_exposure(float ev) noexcept {
    if (!std::isfinite(ev))
      return false;
    exposure_ev_ = std::clamp(ev, MinimumExposureEv, MaximumExposureEv);
    return true;
  }

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
    heap.NumDescriptors = 2;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    status = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(srv_heap_.put()));
    if (FAILED(status))
      return initialization_failed(status, "Creating the two-source SRV heap failed.");
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = 1;
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
    error_[0] = '\0';
    return S_OK;
  }

  // Only single-layer, non-MSAA default-heap 2D textures are supported. Mip zero
  // is sampled; other mips are not transitioned. Typed RGBA/BGRA UNORM, sRGB or
  // RGBA16_FLOAT views must match a typed resource or its typeless family.
  // R11G11B10_FLOAT requires an exact typed resource and view.
  HRESULT set_inputs(ID3D12Resource* nose, DXGI_FORMAT nose_format, ID3D12Resource* tail, DXGI_FORMAT tail_format) noexcept {
    if (!output_.get())
      return fail(E_UNEXPECTED, "Initialize the compositor before binding inputs.");
    if (!nose || !tail || same_object(nose, tail) || same_object(nose, output_.get()) || same_object(tail, output_.get()))
      return fail(E_INVALIDARG, "Two distinct non-null inputs must not alias the compositor output.");
    if (same_object(nose, inputs_[0].get()) && same_object(tail, inputs_[1].get()) && nose_format == formats_[0] &&
        tail_format == formats_[1]) {
      error_[0] = '\0';
      return S_FALSE;
    }
    std::array<D3D12_RESOURCE_DESC, 2> descriptions{};
    if (!validate_input(nose, nose_format, descriptions[0]) || !validate_input(tail, tail_format, descriptions[1]))
      return fail(E_INVALIDARG, "An input has an unsupported device, texture description, heap or typed SRV format.");
    const std::array<ID3D12Resource*, 2> resources{nose, tail};
    const std::array<DXGI_FORMAT, 2> formats{nose_format, tail_format};
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
    statistics_.descriptor_writes += 2;
    ++statistics_.input_changes;
    error_[0] = '\0';
    return S_OK;
  }

  // Supported exact before states: COMMON, RENDER_TARGET, UNORDERED_ACCESS,
  // COPY_DEST, or any nonempty combination of PIXEL_SHADER_RESOURCE,
  // NON_PIXEL_SHADER_RESOURCE and COPY_SOURCE. Other state sets are refused.
  HRESULT record(ID3D12GraphicsCommandList* private_list, D3D12_RESOURCE_STATES nose_before, D3D12_RESOURCE_STATES tail_before) noexcept {
    if (!output_.get() || !inputs_[0].get() || !inputs_[1].get() || !private_list ||
        private_list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !same_device(private_list) ||
        !valid_state(nose_before, input_descriptions_[0]) || !valid_state(tail_before, input_descriptions_[1]))
      return fail(E_INVALIDARG, "A private direct list, bound inputs and supported exact mip-zero states are required.");
    const std::array<D3D12_RESOURCE_STATES, 2> states{nose_before, tail_before};
    for (std::size_t index = 0; index < inputs_.size(); ++index)
      transition(private_list, inputs_[index].get(), states[index], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(private_list, output_.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    private_list->SetGraphicsRootSignature(root_signature_.get());
    private_list->SetPipelineState(pipeline_.get());
    ID3D12DescriptorHeap* heaps[]{srv_heap_.get()};
    private_list->SetDescriptorHeaps(1, heaps);
    private_list->SetGraphicsRootDescriptorTable(0, srv_heap_->GetGPUDescriptorHandleForHeapStart());
    const struct {
      UINT hdr_mask;
      float exposure;
      UINT guides;
      UINT ground_speed;
      UINT ground_speed_valid;
      profiles::Composition composition;
    } display{(formats_[0] == DXGI_FORMAT_R11G11B10_FLOAT ? 1u : 0u) | (formats_[1] == DXGI_FORMAT_R11G11B10_FLOAT ? 2u : 0u),
              std::exp2(exposure_ev_),
              reference_guides_ ? 1u : 0u,
              ground_speed_,
              ground_speed_valid_ ? 1u : 0u,
              composition_};
    static_assert(sizeof(display) == 30 * sizeof(UINT));
    private_list->SetGraphicsRoot32BitConstants(1, 30, &display, 0);
    private_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(Width), static_cast<float>(Height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height)};
    private_list->RSSetViewports(1, &viewport);
    private_list->RSSetScissorRects(1, &scissor);
    private_list->OMSetRenderTargets(1, &rtv_, FALSE, nullptr);
    private_list->DrawInstanced(3, 1, 0, 0);

    transition(private_list, output_.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (std::size_t index = 0; index < inputs_.size(); ++index)
      transition(private_list, inputs_[index].get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, states[index]);
    ++statistics_.recordings;
    error_[0] = '\0';
    return S_OK;
  }

  // Call only after checked GPU completion, including downstream output copies.
  void release() noexcept {
    for (auto& input : inputs_)
      input.reset();
    output_.reset();
    srv_heap_.reset();
    rtv_heap_.reset();
    pipeline_.reset();
    root_signature_.reset();
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
    output_.abandon();
    srv_heap_.abandon();
    rtv_heap_.abandon();
    pipeline_.abandon();
    root_signature_.abandon();
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
                         D3D12_RESOURCE_STATES after) noexcept {
    if (before == after)
      return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = 0;
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

  HRESULT compile(const char* entry, const char* profile, ID3DBlob** bytecode) noexcept {
    Reference<ID3DBlob> diagnostics;
    const HRESULT status = D3DCompile(Shader, sizeof(Shader) - 1, "camera_compositor", nullptr, nullptr, entry, profile,
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
    range.NumDescriptors = 2;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = 30;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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
    status = device_->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(pipeline_.put()));
    return FAILED(status) ? fail(status, "Creating the compositor pipeline failed.") : S_OK;
  }

  static constexpr char Shader[] = R"(
Texture2D<float4> Nose : register(t0);
Texture2D<float4> Tail : register(t1);
SamplerState LinearClamp : register(s0);
cbuffer Display : register(b0) { uint HdrMask; float Exposure; uint ReferenceGuides; uint GroundSpeed; uint GroundSpeedValid;
 float NoseHeight; float TailTop; float DividerTop; float DividerBottom;
 float NoseDotX; float NoseDotY; float TailCornerX; float TailCornerY;
 float TailUpperX; float TailUpperY; float TailInnerX; float TailInnerY;
 float GuideRed; float GuideGreen; float GuideBlue;
 float SpeedRed; float SpeedGreen; float SpeedBlue;
 float SpeedLeft; float SpeedTop; float SpeedPaddingX; float SpeedPaddingY; float SpeedMinimumWidth; float SpeedMinimumHeight;
 float SquareNoseMarkers; };
float segment_distance(float2 sample_position, float2 first, float2 last) {
  float2 delta = last - first;
  return length(sample_position - (first + saturate(dot(sample_position - first, delta) / dot(delta, delta)) * delta));
}
// Original stroke lettering, defined in a 12 x 20 pixel cell. Chamfered turns
// and 1.8 pixel strokes keep the small readout legible without enlarged bitmap
// blocks. Indices 0..9 are digits, followed by G, S and the unavailable dash.
static const uint2 GlyphPaths[13] = {
  uint2(0, 9), uint2(9, 3), uint2(12, 7), uint2(19, 9), uint2(28, 4), uint2(32, 9), uint2(41, 11), uint2(52, 3), uint2(55, 16),
  uint2(71, 11), uint2(82, 10), uint2(92, 12), uint2(104, 2)
};
static const float2 GlyphVertices[106] = {
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
  float2(1.5, 10), float2(10.5, 10)
};
float glyph_coverage(float2 position, float2 origin, uint glyph) {
  float2 local = position - origin;
  if (any(local < 0) || any(local >= float2(12, 20))) return 0;
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
  // One pixel of edge coverage around the 0.9 pixel stroke radius. The box
  // remains opaque: coverage scales the text colour, never its output alpha.
  return saturate(1.4 - distance);
}
uint ground_speed_digits() {
  return GroundSpeedValid == 0 || GroundSpeed >= 10 ? 2 : 1;
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
  return float4(label.xxx + float3(SpeedRed, SpeedGreen, SpeedBlue) * speed, 1);
}
// Screen-space references matched to the supplied ETACS photograph. These
// marks do not claim metric clearance after mount, attitude or FOV changes.
bool reference_guide(float2 position, bool nose) {
  float2 local = float2(min(position.x, 768 - position.x), nose ? position.y : position.y - TailTop);
  if (nose) {
    float2 delta = local - float2(NoseDotX * 768, NoseDotY * NoseHeight);
    // Profile-specific references: 14px A380 squares and 12px A350 circles.
    return SquareNoseMarkers != 0 ? all(abs(delta) < 7) : length(delta) <= 6;
  }
  // The reference bracket's bounding-box centre is near (0.335, 0.69);
  // its outside lower corner is farther out and below that centre.
  float2 corner = float2(TailCornerX * 768, TailCornerY * (763 - TailTop));
  float2 upper = float2(TailUpperX * 768, TailUpperY * (763 - TailTop));
  float2 inner = float2(TailInnerX * 768, TailInnerY * (763 - TailTop));
  return min(segment_distance(local, upper, corner), segment_distance(local, corner, inner)) <= 2;
}
float hdr_channel(float value) {
  if (!isfinite(value)) return value > 0 ? 1 : 0;
  float exposed = max(value, 0) * Exposure;
  float mapped = exposed / (1 + exposed);
  return mapped <= 0.0031308 ? 12.92 * mapped : 1.055 * pow(max(mapped, 0), 1.0 / 2.4) - 0.055;
}
float3 display_rgb(float3 rgb, uint feed) {
  if ((HdrMask & (1u << feed)) == 0) return rgb;
  return float3(hdr_channel(rgb.r), hdr_channel(rgb.g), hdr_channel(rgb.b));
}
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
}
float4 ps_main(float4 position : SV_Position) : SV_Target {
  float2 speed_position = position.xy - float2(SpeedLeft, SpeedTop);
  if (all(speed_position >= 0) && all(speed_position < ground_speed_extent())) return ground_speed_pixel(speed_position);
  if (position.y >= DividerTop && position.y < DividerBottom) return float4(0, 0, 0, 1);
  if (ReferenceGuides != 0 && reference_guide(position.xy, position.y < NoseHeight)) return float4(GuideRed, GuideGreen, GuideBlue, 1);
  if (position.y < NoseHeight) {
    float2 uv = float2(position.x / 768, position.y / NoseHeight);
    return float4(display_rgb(Nose.SampleLevel(LinearClamp, uv, 0).rgb, 0), 1);
  }
  if (position.y < TailTop) return float4(0, 0, 0, 1);
  float2 uv = float2(position.x / 768, (position.y - TailTop) / (763 - TailTop));
  return float4(display_rgb(Tail.SampleLevel(LinearClamp, uv, 0).rgb, 1), 1);
}
)";

  Reference<ID3D12Device> device_;
  Reference<ID3D12RootSignature> root_signature_;
  Reference<ID3D12PipelineState> pipeline_;
  Reference<ID3D12DescriptorHeap> srv_heap_;
  Reference<ID3D12DescriptorHeap> rtv_heap_;
  Reference<ID3D12Resource> output_;
  std::array<Reference<ID3D12Resource>, 2> inputs_;
  std::array<DXGI_FORMAT, 2> formats_{};
  std::array<D3D12_RESOURCE_DESC, 2> input_descriptions_{};
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_{};
  Statistics statistics_;
  float exposure_ev_ = DefaultExposureEv;
  bool reference_guides_ = true;
  profiles::Composition composition_{};
  UINT ground_speed_ = 0;
  bool ground_speed_valid_ = false;
  std::array<char, 1024> error_{};
};

}  // namespace taxi_camera
