// Focused native GPU regression: stamp OFF/ON must preserve dynamic depth bias.
// This is a test-owned device, command list and pixel buffer; no simulator access.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../src/graphics/pfd_stamp_d3d12.hpp"
#include "../support/gpu_queue.hpp"

namespace {
template <class T>
struct Ref {
  T* value = nullptr;
  ~Ref() {
    if (value)
      value->Release();
  }
  T* operator->() const { return value; }
  T** put() { return &value; }
};
void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
void check(HRESULT hr, const char* message) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "%s HRESULT=0x%08lx\n", message, static_cast<unsigned long>(hr));
    throw std::runtime_error(message);
  }
}
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES h{};
  h.Type = type;
  h.CreationNodeMask = h.VisibleNodeMask = 1;
  return h;
}
D3D12_RESOURCE_DESC buffer(UINT64 bytes) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = bytes;
  d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return d;
}
D3D12_RESOURCE_DESC texture(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
  auto d = buffer(64);
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Height = 64;
  d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  d.Format = format;
  d.Flags = flags;
  return d;
}
void create(ID3D12Device* device,
            const D3D12_RESOURCE_DESC& d,
            D3D12_HEAP_TYPE type,
            D3D12_RESOURCE_STATES state,
            ID3D12Resource** out,
            const D3D12_CLEAR_VALUE* clear = nullptr) {
  const auto h = heap(type);
  check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state, clear, IID_PPV_ARGS(out)), "create resource");
}
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &b);
}
void submit(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list) {
  check(list->Close(), "close");
  ID3D12CommandList* lists[]{list};
  queue->ExecuteCommandLists(1, lists);
  require(taxi_camera::drain_copy_queue(queue, device), "GPU completion");
}
struct Fixture {
  Ref<ID3D12Device> device;
  Ref<ID3D12CommandQueue> queue;
  Ref<ID3D12RootSignature> root;
  Ref<ID3D12PipelineState> pipeline;
  Ref<ID3D12Resource> pixels;
  taxi_camera::PfdStampD3D12 stamp;
  bool initialize(bool warp) {
    Ref<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "factory");
    Ref<IDXGIAdapter> adapter;
    if (warp)
      check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "WARP adapter");
    check(D3D12CreateDevice(adapter.value, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "device");
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 options{};
    const HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options, sizeof(options));
    if (FAILED(hr) || !options.DynamicDepthBiasSupported) {
      std::printf("SKIP backend=%s DynamicDepthBiasSupported=0 feature_hr=0x%08lx\n", warp ? "WARP" : "hardware",
                  static_cast<unsigned long>(hr));
      return false;
    }
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(queue.put())), "queue");
    Ref<ID3DBlob> serialized, vs, ps;
    D3D12_ROOT_SIGNATURE_DESC signature{};
    check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), nullptr), "serialize root");
    check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(root.put())), "root");
    constexpr char shader[] = R"(
float4 VS(uint id:SV_VertexID):SV_Position {
 float2 uv=float2((id<<1)&2,id&2); return float4(uv.x*2-1,1-uv.y*2,0.5,1);
}
float4 PS():SV_Target { return float4(0,1,0,1); }
)";
    check(D3DCompile(shader, sizeof(shader) - 1, "dynamic_state", nullptr, nullptr, "VS", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                     vs.put(), nullptr),
          "VS");
    check(D3DCompile(shader, sizeof(shader) - 1, "dynamic_state", nullptr, nullptr, "PS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                     ps.put(), nullptr),
          "PS");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root.value;
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
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    p.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthEnable = TRUE;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    p.DepthStencilState.FrontFace =
        p.DepthStencilState.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    // Public PSO flag bits; the pinned SDK predates these enum enumerators.
    const UINT flags = taxi_camera::d3d12_extended::DynamicDepthBias;
    static_assert(sizeof(flags) == sizeof(p.Flags));
    std::memcpy(&p.Flags, &flags, sizeof(flags));
    check(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(pipeline.put())), "dynamic PSO");
    check(stamp.initialize(device.value, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT), "stamp");
    Ref<ID3D12Resource> upload;
    create(device.value, buffer(taxi_camera::PfdStampFrame::Bytes), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
           upload.put());
    create(device.value, buffer(taxi_camera::PfdStampFrame::Bytes), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, pixels.put());
    void* data = nullptr;
    D3D12_RANGE empty{};
    check(upload->Map(0, &empty, &data), "upload map");
    auto* words = static_cast<UINT*>(data);
    for (UINT64 n = 0; n < taxi_camera::PfdStampFrame::Bytes / 4; ++n)
      words[n] = 0xffff0000u;
    upload->Unmap(0, nullptr);
    Ref<ID3D12CommandAllocator> alloc;
    Ref<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.put())), "upload allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.value, nullptr, IID_PPV_ARGS(list.put())), "upload list");
    list->CopyBufferRegion(pixels.value, 0, upload.value, 0, taxi_camera::PfdStampFrame::Bytes);
    transition(list.value, pixels.value, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    submit(device.value, queue.value, list.value);
    return true;
  }
  std::vector<UINT> run(bool overlay) {
    Ref<ID3D12CommandAllocator> alloc;
    Ref<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.put())), "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.value, nullptr, IID_PPV_ARGS(list.put())), "list");
    Ref<taxi_camera::d3d12_extended::CommandList9> list9;
    check(list->QueryInterface(taxi_camera::d3d12_extended::CommandList9Id, reinterpret_cast<void**>(list9.put())), "List9 QI");
    require(list9.value == list.value, "exact List9 identity");
    Ref<ID3D12Resource> color, depth, readback;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = .5f;
    create(device.value, texture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_HEAP_TYPE_DEFAULT,
           D3D12_RESOURCE_STATE_RENDER_TARGET, color.put());
    create(device.value, texture(DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), D3D12_HEAP_TYPE_DEFAULT,
           D3D12_RESOURCE_STATE_DEPTH_WRITE, depth.put(), &clear);
    create(device.value, buffer(64 * 64 * 4), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, readback.put());
    Ref<ID3D12DescriptorHeap> rtvs, dsvs;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 1;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(rtvs.put())), "RTV heap");
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(dsvs.put())), "DSV heap");
    const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(color.value, nullptr, rtv);
    device->CreateDepthStencilView(depth.value, nullptr, dsv);
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, .5f, 0, 0, nullptr);
    list->SetPipelineState(pipeline.value);
    list->SetGraphicsRootSignature(root.value);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT vp{0, 0, 64, 64, 0, 1};
    D3D12_RECT sc{0, 0, 64, 64};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    taxi_camera::PfdGraphicsState state;
    state.reset(1, true);
    state.bind_pipeline(pipeline.value);
    taxi_camera::PfdRootLayout layout{};
    layout.valid = true;
    state.bind_root(root.value, 1, layout, true);
    state.topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    state.viewports(0, 1, &vp);
    state.scissors(0, 1, &sc);
    list9->RSSetDepthBias(-4, 0, 0);
    state.depth_bias(list9.value, -4, 0, 0);
    require(state.can_restore(list.value), "complete dynamic replay state");
    if (overlay)
      require(stamp.record_buffer(list.value, state, device.value, pixels->GetGPUVirtualAddress(), 64, 64),
              "record actual production stamp");
    // Clear only color after the stamp. This intentionally does not rebind any
    // graphics state. The next app draw passes LESS only if its bias survived.
    constexpr FLOAT red[]{1, 0, 0, 1};
    list->ClearRenderTargetView(rtv, red, 0, nullptr);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list.value, color.value, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = color.value;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.value;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    submit(device.value, queue.value, list.value);
    void* bytes = nullptr;
    D3D12_RANGE range{0, 64 * 64 * 4};
    check(readback->Map(0, &range, &bytes), "readback map");
    std::vector<UINT> result(64 * 64);
    std::memcpy(result.data(), bytes, result.size() * 4);
    D3D12_RANGE empty{};
    readback->Unmap(0, &empty);
    return result;
  }
};
}  // namespace
int main(int argc, char** argv) {
  bool warp = false, expect_loss = false;
  for (int n = 1; n < argc; ++n) {
    if (std::strcmp(argv[n], "--warp") == 0)
      warp = true;
    else if (std::strcmp(argv[n], "--expect-loss") == 0)
      expect_loss = true;
    else
      return 2;
  }
  try {
    Fixture fixture;
    if (!fixture.initialize(warp))
      return 77;
    const auto off = fixture.run(false), on = fixture.run(true);
    UINT off_green = 0, on_green = 0, on_red = 0;
    for (UINT v : off)
      off_green += v == 0xff00ff00u;
    for (UINT v : on) {
      on_green += v == 0xff00ff00u;
      on_red += v == 0xff0000ffu;
    }
    std::printf("backend=%s OFF_green=%u ON_green=%u ON_red=%u pixels=4096\n", warp ? "WARP" : "hardware", off_green, on_green, on_red);
    require(off_green == 4096, "OFF app bias must pass all pixels");
    if (expect_loss) {
      require(on_red == 4096, "pre-fix bias loss must reject every app pixel");
      std::puts("EXPECTED_PRE_FIX_FAILURE: stamp resets dynamic bias; entire following app draw disappears.");
    } else {
      require(off == on, "stamp ON must preserve every app depth-tested pixel");
      std::puts("PASS: dynamic bias OFF==ON4096 exact pixels after production stamp and graphics replay.");
    }
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
