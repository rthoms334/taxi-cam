#include "pfd_stamp_d3d12.hpp"
#include <d3dcompiler.h>
#include "../hooks/pfd_state_observer.hpp"
#include "native_device_identity.hpp"

namespace taxi_camera {
namespace {
template <class T>
void release_pointer(T*& p) noexcept {
  if (p)
    p->Release();
  p = nullptr;
}
bool format_supported(DXGI_FORMAT f) noexcept {
  return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM ||
         f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
bool depth_format_supported(DXGI_FORMAT f) noexcept {
  return f == DXGI_FORMAT_UNKNOWN || f == DXGI_FORMAT_D16_UNORM || f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_D32_FLOAT ||
         f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}
}  // namespace
HRESULT PfdStampFrame::initialize(ID3D12Device* device) noexcept {
  if (!device || device_)
    return E_INVALIDARG;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = Bytes;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  HRESULT hr =
      device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer_));
  if (FAILED(hr))
    return hr;
  device_ = device;
  device_->AddRef();
  address_ = buffer_->GetGPUVirtualAddress();
  if (!address_) {
    release();
    return E_FAIL;
  }
  return S_OK;
}
bool PfdStampFrame::record_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* source) noexcept {
  if (!list || !source || !buffer_ || recorded_ || source == buffer_ || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return false;
  // Pinned MinGW Windows COM header explicitly lowers aggregate returns to an
  // out-parameter, matching the MSVC interface. Independent GPU host exercises it.
  const auto desc = source->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != Width || desc.Height != Height || desc.MipLevels != 1 ||
      desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 || desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
    return false;
  if (!same_native_device(source, device_))
    return false;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = source;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = buffer_;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, Width, Height, 1, RowPitch};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {buffer_, 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
  list->ResourceBarrier(1, &barrier);
  recorded_ = true;
  return true;
}
bool PfdStampFrame::publish(bool completed) noexcept {
  if (recorded_ && completed)
    ready_ = true;
  return ready_;
}
void PfdStampFrame::release() noexcept {
  release_pointer(buffer_);
  release_pointer(device_);
  address_ = 0;
  recorded_ = ready_ = false;
}
void PfdStampFrame::abandon() noexcept {
  buffer_ = nullptr;
  device_ = nullptr;
  address_ = 0;
  recorded_ = ready_ = false;
}

HRESULT PfdStampD3D12::initialize(ID3D12Device* device, DXGI_FORMAT format, DXGI_FORMAT depth_format) noexcept {
  if (!device || device_ || !format_supported(format) || !depth_format_supported(depth_format))
    return E_INVALIDARG;
  constexpr char shader[] = R"(
ByteAddressBuffer Pixels : register(t0);
cbuffer Parameters : register(b0) {
  uint TargetWidth; uint TargetHeight; uint OriginX; uint OriginY;
  uint InsetLeft; uint InsetTop; uint InsetRight; uint InsetBottom;
};
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
}
float4 ps_main(float4 position : SV_Position) : SV_Target {
  float2 local = position.xy - float2(OriginX, OriginY);
  uint2 contentEnd = uint2(TargetWidth - InsetRight, TargetHeight - InsetBottom);
  if (local.x < InsetLeft || local.y < InsetTop || local.x >= contentEnd.x || local.y >= contentEnd.y)
    return float4(0, 0, 0, 1);
  uint2 contentSize = contentEnd - uint2(InsetLeft, InsetTop);
  uint x = min((uint)((local.x - InsetLeft) * 768 / contentSize.x), 767);
  uint y = min((uint)((local.y - InsetTop) * 763 / contentSize.y), 762);
  uint rgba = Pixels.Load(y * 3072 + x * 4);
  return float4(rgba & 255, (rgba >> 8) & 255, (rgba >> 16) & 255, 255) / 255.0;
}
)";
  ID3DBlob *serialized = nullptr, *vs = nullptr, *ps = nullptr;
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].Constants.Num32BitValues = 8;
  D3D12_ROOT_SIGNATURE_DESC desc{};
  desc.NumParameters = 2;
  desc.pParameters = params;
  HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr);
  if (SUCCEEDED(hr))
    hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_));
  if (SUCCEEDED(hr))
    hr = D3DCompile(shader, sizeof(shader) - 1, "pfd_stamp", nullptr, nullptr, "vs_main", "vs_5_0",
                    D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, nullptr);
  if (SUCCEEDED(hr))
    hr = D3DCompile(shader, sizeof(shader) - 1, "pfd_stamp", nullptr, nullptr, "ps_main", "ps_5_0",
                    D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, nullptr);
  if (SUCCEEDED(hr)) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root_;
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& blend = p.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask = UINT_MAX;
    p.SampleDesc.Count = 1;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthEnable = FALSE;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.DepthStencilState.StencilEnable = FALSE;
    p.DepthStencilState.FrontFace =
        p.DepthStencilState.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = format;
    p.DSVFormat = depth_format;
    hr = device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline_));
  }
  release_pointer(serialized);
  release_pointer(vs);
  release_pointer(ps);
  if (FAILED(hr)) {
    release();
    return hr;
  }
  device_ = device;
  device_->AddRef();
  format_ = format;
  depth_format_ = depth_format;
  return S_OK;
}
bool PfdStampD3D12::record(ID3D12GraphicsCommandList* list,
                           const PfdGraphicsState& state,
                           const PfdStampFrame& frame,
                           UINT width,
                           UINT height) noexcept {
  return frame.ready() && record_buffer(list, state, frame.device(), frame.address(), width, height);
}
bool PfdStampD3D12::record_buffer(ID3D12GraphicsCommandList* list,
                                  const PfdGraphicsState& state,
                                  ID3D12Device* buffer_device,
                                  D3D12_GPU_VIRTUAL_ADDRESS address,
                                  UINT width,
                                  UINT height,
                                  const D3D12_RECT* destination,
                                  const D3D12_RECT* content,
                                  bool draw,
                                  PfdStateGroup group) noexcept {
  const engine_hook::pfd_state::ScopedBypass bypass;
  if (!list || !valid_pfd_state_group(group) || (draw && group != PfdStateGroup::all) || !state.can_restore(list))
    return false;
  d3d12_extended::CommandList9* dynamic = nullptr;
  if (includes_pfd_state_group(group, PfdStateGroup::pipeline) && (state.has_depth_bias() || state.has_strip_cut())) {
    const auto hr = list->QueryInterface(d3d12_extended::CommandList9Id, reinterpret_cast<void**>(&dynamic));
    if (FAILED(hr) || dynamic != list) {
      if (dynamic)
        dynamic->Release();
      return false;  // Refuse before changing any application graphics state.
    }
  }
  ID3D12GraphicsCommandList1* samples = nullptr;
  if (includes_pfd_state_group(group, PfdStateGroup::raster) && state.has_sample_positions()) {
    const auto hr = list->QueryInterface(IID_PPV_ARGS(&samples));
    if (FAILED(hr) || samples != list) {
      if (samples)
        samples->Release();
      if (dynamic)
        dynamic->Release();
      return false;
    }
    // The application can prepare a multisample pattern before its next target
    // bind. Our intervening draw has a single-sample PSO and needs defaults.
    samples->SetSamplePositions(0, 0, nullptr);
  }
  const bool recorded = record_private_patch(list, buffer_device, address, width, height, destination, content, draw, group);
  if (recorded)
    state.restore(list, group);
  else if (samples)
    state.restore_sample_positions(list);
  if (samples)
    samples->Release();
  if (dynamic)
    dynamic->Release();
  return recorded;
}
bool PfdStampD3D12::record_private_patch(ID3D12GraphicsCommandList* list,
                                         ID3D12Device* buffer_device,
                                         D3D12_GPU_VIRTUAL_ADDRESS address,
                                         UINT width,
                                         UINT height,
                                         const D3D12_RECT* destination,
                                         const D3D12_RECT* content,
                                         bool draw,
                                         PfdStateGroup group) noexcept {
  if (!valid_pfd_state_group(group) || (draw && group != PfdStateGroup::all) || !list || !pipeline_ || !address || address % 4 ||
      buffer_device != device_ || width < 1 || height < 2 || width > 16384 || height > 16384 ||
      list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return false;
  const UINT upper = static_cast<UINT>((static_cast<UINT64>(height) * 763) / 1024);
  if (!upper)
    return false;
  const D3D12_RECT rect = destination ? *destination : D3D12_RECT{0, 0, static_cast<LONG>(width), static_cast<LONG>(upper)};
  if (rect.left < 0 || rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top || rect.right > static_cast<LONG>(width) ||
      rect.bottom > static_cast<LONG>(height))
    return false;
  const D3D12_RECT inner = content ? *content : rect;
  if (inner.left < rect.left || inner.top < rect.top || inner.right > rect.right || inner.bottom > rect.bottom ||
      inner.right <= inner.left || inner.bottom <= inner.top)
    return false;
  const D3D12_VIEWPORT viewport{static_cast<float>(rect.left),
                                static_cast<float>(rect.top),
                                static_cast<float>(rect.right - rect.left),
                                static_cast<float>(rect.bottom - rect.top),
                                0,
                                1};
  const D3D12_RECT scissor = rect;
  const UINT constants[8]{static_cast<UINT>(rect.right - rect.left),
                          static_cast<UINT>(rect.bottom - rect.top),
                          static_cast<UINT>(rect.left),
                          static_cast<UINT>(rect.top),
                          static_cast<UINT>(inner.left - rect.left),
                          static_cast<UINT>(inner.top - rect.top),
                          static_cast<UINT>(rect.right - inner.right),
                          static_cast<UINT>(rect.bottom - inner.bottom)};
  const engine_hook::pfd_state::ScopedBypass bypass;
  if (includes_pfd_state_group(group, PfdStateGroup::pipeline))
    list->SetPipelineState(pipeline_);
  if (includes_pfd_state_group(group, PfdStateGroup::root_bindings)) {
    list->SetGraphicsRootSignature(root_);
    list->SetGraphicsRootShaderResourceView(0, address);
    list->SetGraphicsRoot32BitConstants(1, 8, constants, 0);
  }
  if (includes_pfd_state_group(group, PfdStateGroup::raster)) {
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
  }
  if (draw)
    list->DrawInstanced(3, 1, 0, 0);
  return true;
}
bool PfdStampD3D12::record_final_buffer(ID3D12GraphicsCommandList* list,
                                        const PfdGraphicsState& state,
                                        ID3D12Device* buffer_device,
                                        D3D12_GPU_VIRTUAL_ADDRESS address,
                                        UINT width,
                                        UINT height,
                                        const D3D12_RECT* destination,
                                        const D3D12_RECT* content) noexcept {
  const engine_hook::pfd_state::ScopedBypass bypass;
  if (!list || !state.can_restore(list) || !pipeline_ || !address || address % 4 || buffer_device != device_ || width < 1 || height < 2 ||
      width > 16384 || height > 16384 || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return false;
  // Preflight the same rectangles as record_private_patch before temporarily
  // normalizing sample positions. Invalid requests change no graphics state.
  const UINT upper = static_cast<UINT>((static_cast<UINT64>(height) * 763) / 1024);
  if (!upper)
    return false;
  const D3D12_RECT rect = destination ? *destination : D3D12_RECT{0, 0, static_cast<LONG>(width), static_cast<LONG>(upper)};
  if (rect.left < 0 || rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top || rect.right > static_cast<LONG>(width) ||
      rect.bottom > static_cast<LONG>(height))
    return false;
  const D3D12_RECT inner = content ? *content : rect;
  if (inner.left < rect.left || inner.top < rect.top || inner.right > rect.right || inner.bottom > rect.bottom ||
      inner.right <= inner.left || inner.bottom <= inner.top)
    return false;
  ID3D12GraphicsCommandList1* samples = nullptr;
  if (state.has_sample_positions()) {
    const auto hr = list->QueryInterface(IID_PPV_ARGS(&samples));
    if (FAILED(hr) || samples != list) {
      if (samples)
        samples->Release();
      return false;
    }
    samples->SetSamplePositions(0, 0, nullptr);
  }
  // Our PSO resets any dynamic depth-bias/strip-cut overrides. The only draw
  // follows all application work; there is no application state restoration.
  const bool recorded = record_private_patch(list, buffer_device, address, width, height, &rect, &inner);
  if (!recorded && samples)
    state.restore_sample_positions(list);
  if (samples)
    samples->Release();
  return recorded;
}
void PfdStampD3D12::release() noexcept {
  release_pointer(pipeline_);
  release_pointer(root_);
  release_pointer(device_);
  format_ = DXGI_FORMAT_UNKNOWN;
  depth_format_ = DXGI_FORMAT_UNKNOWN;
}
void PfdStampD3D12::abandon() noexcept {
  pipeline_ = nullptr;
  root_ = nullptr;
  device_ = nullptr;
  format_ = DXGI_FORMAT_UNKNOWN;
  depth_format_ = DXGI_FORMAT_UNKNOWN;
}
}  // namespace taxi_camera
