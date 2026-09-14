#undef WIN32_LEAN_AND_MEAN
#undef NOMINMAX
// Existing independent gradient and pixel oracles; its main is not called.
#define wmain compositor_test_not_called
#include "../validation/compositor_main.cpp"
#undef wmain
#include <d3d11on12.h>
#include "d3d12_bridge.hpp"
#include "native_hooks.hpp"
namespace {
namespace win = taxi_camera::standalone;
namespace runtime = taxi_camera::scene_runtime;
struct ClearStatePipeline {
  Reference<ID3D12RootSignature> root;
  Reference<ID3D12PipelineState> pipeline;
  explicit ClearStatePipeline(ID3D12Device* device, bool descriptor_case = false, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
    // Match the gradient's root layout, but make pipeline loss visible as
    // different GPU pixels rather than relying only on intercepted-call counts.
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 4;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0}};
    for (UINT i = 0; i < 2; ++i) {
      parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameters[i + 1].DescriptorTable = {1, &ranges[i]};
      parameters[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[3].Descriptor.ShaderRegister = 1;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_ROOT_SIGNATURE_DESC signature{descriptor_case ? 4u : 1u, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Reference<ID3DBlob> serialized, vertex, pixel;
    check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), nullptr),
          "Serialize ClearState signature");
    check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(root.put())),
          "Create ClearState signature");
    constexpr char shader[] = R"(
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
}
cbuffer Parameters : register(b0) { uint Width; uint Height; uint Mode; uint Frame; };
#ifdef DESCRIPTOR_CASE
Texture2D<float4> Texture : register(t0);
SamplerState Sample : register(s0);
cbuffer Colour : register(b1) { float4 Tint; };
#endif
float4 ps_main() : SV_Target {
  float4 value = float4(Mode != 0 ? 1 : 0, Width == 64 && Height == 64 ? 1 : 0, Frame != 0 ? 1 : 0, 1);
#ifdef DESCRIPTOR_CASE
  value *= Texture.SampleLevel(Sample, float2(.5,.5), 0) * Tint;
#endif
  return value;
}
)";
    const D3D_SHADER_MACRO macros[]{{"DESCRIPTOR_CASE", "1"}, {nullptr, nullptr}};
    check(D3DCompile(shader, sizeof(shader) - 1, "clear_state_pipeline", descriptor_case ? macros : nullptr, nullptr, "vs_main", "vs_5_0",
                     D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vertex.put(), nullptr),
          "Compile ClearState vertex shader");
    check(D3DCompile(shader, sizeof(shader) - 1, "clear_state_pipeline", descriptor_case ? macros : nullptr, nullptr, "ps_main", "ps_5_0",
                     D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, pixel.put(), nullptr),
          "Compile ClearState pixel shader");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root.get();
    desc.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    desc.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    auto& blend = desc.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                                                          D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.SampleDesc.Count = 1;
    desc.RTVFormats[0] = format;
    check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pipeline.put())), "Create ClearState pipeline");
  }
};
void copy_with_11on12(ID3D12Device* device, ID3D12CommandQueue* queue) {
  const auto module = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  require(module != nullptr, "Load system D3D11 for capture interop regression");
  const auto create = reinterpret_cast<PFN_D3D11ON12_CREATE_DEVICE>(GetProcAddress(module, "D3D11On12CreateDevice"));
  require(create != nullptr, "D3D11On12CreateDevice");
  Reference<ID3D11Device> device11;
  Reference<ID3D11DeviceContext> context;
  IUnknown* queues[]{queue};
  check(create(device, 0, nullptr, 0, queues, 1, 0, device11.put(), context.put(), nullptr), "Create capture interop device");
  Reference<ID3D11On12Device> interop;
  check(device11->QueryInterface(IID_PPV_ARGS(interop.put())), "Interop device");
  // Capture wraps a real swap-chain buffer. A plain committed texture lacks
  // DXGI's compatibility metadata and triggers ReflectSharedProperties errors
  // when the D3D12 debug layer validates the D3D11On12 resource open.
  struct HiddenWindow {
    HWND handle = CreateWindowExW(0,
                                  L"STATIC",
                                  L"Taxi Cam capture validation",
                                  WS_OVERLAPPEDWINDOW,
                                  0,
                                  0,
                                  1920,
                                  1080,
                                  nullptr,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  nullptr);
    ~HiddenWindow() {
      if (handle)
        DestroyWindow(handle);
    }
  } window;
  require(window.handle != nullptr, "Create hidden capture window");
  Reference<IDXGIFactory2> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "Capture swap-chain factory");
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = 1920;
  desc.Height = 1080;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  Reference<IDXGISwapChain1> swap_chain;
  check(factory->CreateSwapChainForHwnd(queue, window.handle, &desc, nullptr, nullptr, swap_chain.put()), "Capture swap chain");
  Reference<ID3D12Resource> source;
  check(swap_chain->GetBuffer(0, IID_PPV_ARGS(source.put())), "Capture backbuffer");
  Reference<ID3D11Resource> wrapped;
  D3D11_RESOURCE_FLAGS flags{};
  check(interop->CreateWrappedResource(source.get(), &flags, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
                                       IID_PPV_ARGS(wrapped.put())),
        "Wrap unrelated backbuffer");
  D3D11_TEXTURE2D_DESC output{};
  output.Width = 1920;
  output.Height = 1080;
  output.MipLevels = output.ArraySize = output.SampleDesc.Count = 1;
  output.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  output.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  output.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  Reference<ID3D11Texture2D> destination;
  check(device11->CreateTexture2D(&output, nullptr, destination.put()), "Capture shared texture");
  auto* input = wrapped.get();
  interop->AcquireWrappedResources(&input, 1);
  context->CopyResource(destination.get(), input);
  interop->ReleaseWrappedResources(&input, 1);
  context->Flush();
  require(taxi_camera::drain_copy_queue(queue, device), "Interop capture completion");
}

void native_case(bool warp, bool a350) {
  const auto& profile = a350 ? taxi_camera::profiles::A359 : taxi_camera::profiles::A380;
  const UINT pane_width = profile.camera_panes[0][0], display_width = profile.width;
  const UINT nose_height = profile.camera_panes[0][1], tail_height = profile.camera_panes[1][1];
  Reference<ID3D12Debug> debug;
  const bool debug_enabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())));
  if (debug_enabled)
    debug->EnableDebugLayer();
  Reference<IDXGIFactory4> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "Factory");
  Reference<IDXGIAdapter> adapter;
  if (warp)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "WARP");
  Reference<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.put())), "Device");
  Reference<ID3D12InfoQueue> messages;
  if (debug_enabled)
    device->QueryInterface(IID_PPV_ARGS(messages.put()));
  // These common graphics objects predate exe.xml companion startup.
  GradientGenerator generator(device.get());
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  Reference<ID3D12CommandQueue> queue;
  Reference<ID3D12CommandAllocator> allocator;
  Reference<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.put())), "Pre-existing application queue");
  check(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(allocator.put())), "Pre-existing allocator");
  check(device->CreateCommandList(0, qd.Type, allocator.get(), nullptr, IID_PPV_ARGS(list.put())), "Pre-existing list");
  check(list->Close(), "Close pre-existing list");
  require(win::initialize_graphics(device.get()), win::graphics_status().error);
  win::set_aircraft_profile(profile.id);
  check(list->Reset(allocator.get(), nullptr), "Observe first actual Reset of pre-existing list");
  const auto key = win::graphics_status().device;
  require(runtime::prepare(key), "Native compositor prepare");
  runtime::set_composition(key, profile.composition);
  runtime::manager().begin_source_tracking();
  runtime::manager().set_source_rate(60);
  std::array<Reference<ID3D12Resource>, 4> textures;
  for (UINT i = 0; i < 4; ++i) {
    auto d = texture_description(i < 2 ? pane_width : display_width,
                                 i == 0   ? nose_height
                                 : i == 1 ? tail_height
                                          : 1024,
                                 DXGI_FORMAT_R8G8B8A8_UNORM);
    if (i > 1)
      d.MipLevels = a350 ? 1 : 5;
    create_texture(device.get(), d, textures[i].put());
  }
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = 8;
  Reference<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(heap.put())), "RTV heap");
  const auto stride = device->GetDescriptorHandleIncrementSize(hd.Type);
  const auto base = heap->GetCPUDescriptorHandleForHeapStart();
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> rtvs;
  for (UINT i = 0; i < 4; ++i) {
    D3D12_RENDER_TARGET_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvs[i] = {base.ptr + SIZE_T{i} * stride};
    device->CreateRenderTargetView(textures[i].get(), &d, rtvs[i]);
  }
  // Exercise the two public descriptor copy routes, not just creation.
  device->CopyDescriptorsSimple(1, {base.ptr + 4 * stride}, rtvs[2], hd.Type);
  const D3D12_CPU_DESCRIPTOR_HANDLE dest{base.ptr + 5 * stride};
  device->CopyDescriptors(1, &dest, nullptr, 1, &rtvs[3], nullptr, hd.Type);
  rtvs[2] = {base.ptr + 4 * stride};
  rtvs[3] = dest;
  auto& handoff = taxi_camera::scene_handoff();
  handoff.begin_scene();
  require(handoff.publish(handoff.begin_capture(), {501, 1}, {701, 702},
                          {reinterpret_cast<std::uint64_t>(textures[0].get()), reinterpret_cast<std::uint64_t>(textures[1].get())}),
          "Native source creation identities published");
  const auto inventory = win::pfd_inventory();
  require(inventory.size() == 2, "Two native PFD candidates");
  // IDs are monotonically assigned at creation, but unordered inventory order
  // is not a side label. Bind the first created PFD to the left for this oracle.
  const auto first = std::min(inventory[0].id, inventory[1].id);
  const auto second = std::max(inventory[0].id, inventory[1].id);
  require(win::assign_targets(first, second), "Explicit PFD pair");
  require(!win::assign_targets(inventory[0].id, inventory[0].id), "Reject duplicate PFD identity");

  auto submit = [&] {
    check(list->Close(), "Close application recording");
    ID3D12CommandList* batch[]{list.get()};
    queue->ExecuteCommandLists(1, batch);
    require(taxi_camera::drain_copy_queue(queue.get(), device.get()), "Application GPU completion");
  };
  auto reset = [&] {
    check(allocator->Reset(), "Allocator Reset");
    check(list->Reset(allocator.get(), nullptr), "Observed native Reset");
  };
  generator.record(list.get(), rtvs[0], pane_width, nose_height, false, 0, 0);
  generator.record(list.get(), rtvs[1], pane_width, tail_height, false, 0, 1);
  submit();
  reset();
  const auto deadline = GetTickCount64() + 10000;
  while (!runtime::snapshot(key).output && GetTickCount64() < deadline) {
    runtime::service();
    Sleep(1);
  }
  auto capture = runtime::snapshot(key);
  std::printf("native capture=%llu completed=%llu composed=%llu source_draws=%llu tail=%s\n",
              static_cast<unsigned long long>(capture.capture.captures), static_cast<unsigned long long>(capture.capture.completed),
              static_cast<unsigned long long>(capture.frames), static_cast<unsigned long long>(capture.capture.source_draws),
              capture.capture.tail_status);
  require(capture.output && capture.frames, "Actual native draw -> queue -> capture -> composition");
  // OBS Game Capture uses D3D11On12 to copy the swap-chain backbuffer on the
  // application's queue. Exercise that API sequence without launching OBS.
  const auto before_interop = runtime::manager().statistics();
  copy_with_11on12(device.get(), queue.get());
  const auto after_interop = runtime::manager().statistics();
  std::printf("interop invalid recordings: %llu -> %llu; unknown lists: %llu -> %llu\n", before_interop.invalid_source_recordings,
              after_interop.invalid_source_recordings, before_interop.unknown_submitted_lists, after_interop.unknown_submitted_lists);
  Sleep(20);  // Next permitted 60-Hz capture opportunity.
  const auto frames_before_interop = runtime::snapshot(key).frames;
  generator.record(list.get(), rtvs[0], pane_width, nose_height, false, 0, 0);
  generator.record(list.get(), rtvs[1], pane_width, tail_height, false, 0, 1);
  submit();
  reset();
  const auto interop_deadline = GetTickCount64() + 1000;
  while (runtime::snapshot(key).frames == frames_before_interop && GetTickCount64() < interop_deadline) {
    runtime::service();
    Sleep(1);
  }
  require(runtime::snapshot(key).frames > frames_before_interop, "Camera capture survives unrelated D3D11On12 Game Capture copy");
  win::set_target_mask(3);
  Reference<ID3D12GraphicsCommandList7> enhanced;
  check(list->QueryInterface(IID_PPV_ARGS(enhanced.put())), "Enhanced command list");
  const auto enhanced_transition = [&](ID3D12Resource* target, D3D12_BARRIER_LAYOUT before, D3D12_BARRIER_LAYOUT after,
                                       D3D12_BARRIER_ACCESS access_before, D3D12_BARRIER_ACCESS access_after) {
    D3D12_TEXTURE_BARRIER b{};
    b.SyncBefore = b.SyncAfter = D3D12_BARRIER_SYNC_ALL;
    b.AccessBefore = access_before;
    b.AccessAfter = access_after;
    b.LayoutBefore = before;
    b.LayoutAfter = after;
    b.pResource = target;
    b.Subresources = {0, 1, 0, 1, 0, 1};
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1;
    group.pTextureBarriers = &b;
    enhanced->Barrier(1, &group);
  };
  for (UINT side = 0; side < 2; ++side) {
    if (side) {
      win::set_target_mask(0);  // The test changes this resource to the enhanced model before drawing.
      transition(list.get(), textures[3].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
      enhanced_transition(textures[3].get(), D3D12_BARRIER_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_ACCESS_COMMON,
                          D3D12_BARRIER_ACCESS_RENDER_TARGET);
      win::set_target_mask(3);
    }
    generator.record(list.get(), rtvs[2 + side], display_width, 1024, false, 0, 0);
    if (a350) {
      const float grey[]{0.25f, 0.25f, 0.25f, 1};
      const D3D12_RECT gutter{806, 0, 838, 763};
      list->ClearRenderTargetView(rtvs[2 + side], grey, 1, &gutter);
    }
  }
  // Only change root constants and scissor. Original pipeline, signature,
  // topology and viewport must have survived the injected PFD draw.
  // Update one word only: previously set words must survive our root change.
  list->SetGraphicsRoot32BitConstant(0, 1, 2);
  const D3D12_RECT lower{0, 763, static_cast<LONG>(display_width), 1024};
  list->RSSetScissorRects(1, &lower);
  for (unsigned draw = 0; draw < 1000; ++draw)
    list->DrawInstanced(3, 1, 0, 0);
  require(runtime::snapshot(key).stamps == 0, "Target switch supplies no barrier proof; repeated glyph draws stay deferred");
  std::array<Reference<ID3D12Resource>, 2> readbacks;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes{};
  const auto pd = textures[2]->GetDesc();
  device->GetCopyableFootprints(&pd, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto bh = heap_properties(D3D12_HEAP_TYPE_READBACK);
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = bytes;
  bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  // MSFS submits batches above seven thousand barriers. An unrelated RT and
  // thousands of UAV barriers must not hide the selected PFD exit near the end.
  Reference<ID3D12Resource> ordinary_target, ordinary_readback;
  const auto ordinary_desc = texture_description(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM);
  create_texture(device.get(), ordinary_desc, ordinary_target.put());
  const D3D12_CPU_DESCRIPTOR_HANDLE ordinary_rtv{base.ptr + 6 * stride};
  device->CreateRenderTargetView(ordinary_target.get(), nullptr, ordinary_rtv);
  const float ordinary_color[]{.25f, .5f, .75f, 1};
  list->ClearRenderTargetView(ordinary_rtv, ordinary_color, 0, nullptr);
  auto ordinary_buffer = bd;
  ordinary_buffer.Width = 256 * 64;
  check(device->CreateCommittedResource(&bh, D3D12_HEAP_FLAG_NONE, &ordinary_buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(ordinary_readback.put())),
        "Unselected RT readback");
  for (UINT i = 0; i < 2; ++i) {
    check(device->CreateCommittedResource(&bh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(readbacks[i].put())),
          "Test-only readback");
    if (i)
      enhanced_transition(textures[3].get(), D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_COPY_SOURCE,
                          D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_COPY_SOURCE);
    else {
      std::vector<D3D12_RESOURCE_BARRIER> barriers(7105);
      for (auto& barrier : barriers)
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barriers[0].Transition = {ordinary_target.get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
      barriers.back().Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barriers.back().Transition = {textures[2].get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
      const auto before = runtime::snapshot(key).stamps;
      list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
      std::printf("7105-barrier selected PFD copies: %llu -> %llu\n", before, runtime::snapshot(key).stamps);
      require(runtime::snapshot(key).stamps == before + 1, "Selected PFD near end of7105barriers receives exactly one copy");
      D3D12_TEXTURE_COPY_LOCATION ordinary_source{}, ordinary_destination{};
      ordinary_source.pResource = ordinary_target.get();
      ordinary_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      ordinary_destination.pResource = ordinary_readback.get();
      ordinary_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      ordinary_destination.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256};
      list->CopyTextureRegion(&ordinary_destination, 0, 0, 0, &ordinary_source, nullptr);
      transition(list.get(), ordinary_target.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = textures[i + 2].get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readbacks[i].get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (i) {
      enhanced_transition(textures[3].get(), D3D12_BARRIER_LAYOUT_COPY_SOURCE, D3D12_BARRIER_LAYOUT_COMMON,
                          D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_ACCESS_COMMON);
      transition(list.get(), textures[3].get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else
      transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  submit();
  require(runtime::snapshot(key).stamps == 2, "One private patch per PFD: legacy and enhanced RT-exit boundaries");
  ID3D12CommandList* replay[]{list.get()};
  queue->ExecuteCommandLists(1, replay);
  require(taxi_camera::drain_copy_queue(queue.get(), device.get()), "Recorded private-patch copies replay with stable buffers");
  reset();
  std::uint64_t pixels = 0;
  std::vector<unsigned char> reference_patch;
  void* ordinary_pixels{};
  const D3D12_RANGE ordinary_range{0, 256 * 64}, ordinary_none{};
  check(ordinary_readback->Map(0, &ordinary_range, &ordinary_pixels), "Map unselected RT pixels");
  for (UINT i = 0; i < 64 * 64; ++i) {
    const auto* pixel = static_cast<const unsigned char*>(ordinary_pixels) + i * 4;
    require(pixel[0] >= 63 && pixel[0] <= 64 && pixel[1] >= 127 && pixel[1] <= 128 && pixel[2] >= 191 && pixel[2] <= 192 && pixel[3] == 255,
            "Large-batch private patch leaves unrelated RT unchanged");
    ++pixels;
  }
  ordinary_readback->Unmap(0, &ordinary_none);
  for (UINT side = 0; side < 2; ++side) {
    void* mapped{};
    const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    check(readbacks[side]->Map(0, &range, &mapped), "Map verification readback");
    const auto* data = static_cast<const unsigned char*>(mapped);
    if (!side)
      reference_patch.assign(data, data + bytes);
    std::uint64_t border_pixels = 0, gs_padding_pixels = 0, gs_label_pixels = 0, guide_pixels = 0, tail_guide_pixels = 0;
    for (UINT y = 0; y < 1024; ++y)
      for (UINT x = 0; x < display_width; ++x) {
        const auto* pixel = data + SIZE_T{y} * footprint.Footprint.RowPitch + 4 * x;
        // Expected bounds are independent of the production profile helper.
        // The outer rectangle still excludes the A35032px grey gutter and ND.
        const UINT outer_left = a350 && side ? 838u : 0u;
        const UINT outer_width = a350 ? 806u : 768u;
        const UINT inner_left = outer_left + 16, inner_width = outer_width - 32;
        constexpr UINT inner_top = 12, inner_height = 751;
        const bool outer = x >= outer_left && x < outer_left + outer_width && y < 763;
        const bool inner = x >= inner_left && x < inner_left + inner_width && y >= inner_top && y < 763;
        if (outer && !inner) {
          // Every border pixel was seeded by a nonblack application gradient.
          require(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255,
                  "Opaque black top/left/right camera border on both displays");
          ++border_pixels;
        }
        if (a350 && !outer && y < 763) {
          if (x >= 806 && x < 838)
            require(pixel[0] >= 63 && pixel[0] <= 65 && pixel[1] >= 63 && pixel[1] <= 65 && pixel[2] >= 63 && pixel[2] <= 65,
                    "A350 central grey separator preserved on both sides");
          else
            require(pixel[2] >= 50 && pixel[2] <= 52, "A350 navigation area preserved");
        }
        if (inner) {
          const auto working_x = static_cast<unsigned>((x - inner_left + .5) * 768 / inner_width);
          const auto working_y = static_cast<unsigned>((y - inner_top + .5) * 763 / inner_height);
          require(working_x < 768 && working_y < 763, "Inset working-image mapping remains bounded");
          // Independent broad regions deliberately exclude reference marks and GS.
          if (working_x >= 350 && working_x < 400 && working_y >= 100 && working_y < 150)
            require(pixel[2] >= 49 && pixel[2] <= 53, "Nose frame within inset PFD content");
          if (working_x >= 350 && working_x < 400 && working_y >= 400 && working_y < 450)
            require(pixel[2] >= 202 && pixel[2] <= 206, "Tail frame within inset PFD content");
          if (working_y >= 251 && working_y < 263)
            require(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0, "Black divider scales with the complete working image");
          if (working_x >= 350 && working_x < 400 && working_y >= 245 && working_y < 251)
            require(pixel[2] >= 49 && pixel[2] <= 53, "Thinner divider exposes the restored nose edge");
          if (working_x >= 350 && working_x < 400 && working_y >= 263 && working_y < 269)
            require(pixel[2] >= 202 && pixel[2] <= 206, "Thinner divider exposes the restored tail edge");
          // Both coordinates move with the inner rectangle, including the GS
          // panel's own independent padding. Its default unavailable value is--.
          const bool camera_margin = (working_x == 0 && working_y == 0) || (working_x == 4 && working_y == 30) ||
                                     (working_x == 40 && working_y == 4) || (working_x == 108 && working_y == 30) ||
                                     (working_x == 40 && working_y == 52);
          if (camera_margin)
            require(pixel[2] >= 50 && pixel[2] <= 52, "Camera image around inset GS panel");
          if (working_x == 20 && working_y == 16) {
            require(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0, "GS panel internal padding");
            ++gs_padding_pixels;
          }
          if (working_x == 26 && working_y == 21) {
            require(pixel[0] > 250 && pixel[1] > 250 && pixel[2] > 250, "GS label scales with inset content in both axes");
            ++gs_label_pixels;
          }
          // These pixels verify configured coordinates, not alignment to aircraft geometry.
          if (a350 && (working_x == 207 || working_x == 560) && working_y == 728) {
            require(pixel[0] > 250 && pixel[1] > 135 && pixel[1] < 145 && pixel[2] < 3,
                    "Configured A350 lower bracket corners render on both sides");
            ++tail_guide_pixels;
          }
          if (a350 && ((working_x == 234 && working_y == 637) || (working_x == 207 && working_y == 682)))
            require(pixel[2] >= 202 && pixel[2] <= 206, "Old A350 bracket position is camera imagery");
          if (working_x >= 106 && working_x <= 109 && working_y >= 121 && working_y <= 123) {
            require(pixel[0] > 250 && (a350 ? pixel[1] > 135 && pixel[1] < 145 && pixel[2] < 3 : pixel[2] > 250),
                    "Aircraft reference marker scales with inset content in both axes");
            ++guide_pixels;
          }
        }
        if (y >= 763) {
          const UINT blue = side ? 153 : 51;
          require(pixel[2] >= blue - 1 && pixel[2] <= blue + 1, "Lower trim and application graphics state preserved");
        }
        ++pixels;
      }
    require(border_pixels == (a350 ? 33704u : 33248u), "Exact black border coverage on each PFD");
    require(gs_padding_pixels && gs_label_pixels && guide_pixels && (!a350 || tail_guide_pixels >= 2),
            "Inset GS/guide pixel checks were not exercised");
    const D3D12_RANGE none{0, 0};
    readbacks[side]->Unmap(0, &none);
  }
  // A draw can be submitted in a different recording from its final RT exit.
  // An empty transition-only recording remains unsafe; a later recording with
  // independently observed outside-pass work supplies the required pass proof.
  generator.record(list.get(), rtvs[2], display_width, 1024, false, 0, 0);
  submit();
  reset();
  const auto empty_before = runtime::snapshot(key).stamps;
  transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  require(runtime::snapshot(key).stamps == empty_before, "Empty RT-exit recording cannot admit a private copy");
  transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  submit();
  reset();
  generator.record(list.get(), ordinary_rtv, 64, 64, false, 0, 0);
  const auto cross_before = win::graphics_status();
  const auto cross_stamps = runtime::snapshot(key).stamps;
  transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  const auto cross_after = win::graphics_status();
  std::printf("Cross-recording selected PFD copies: %llu -> %llu; pending matches: %llu -> %llu\n", cross_stamps,
              runtime::snapshot(key).stamps, cross_before.selected_pending_matches, cross_after.selected_pending_matches);
  require(runtime::snapshot(key).stamps == cross_stamps + 1 &&
              cross_after.selected_pending_matches == cross_before.selected_pending_matches &&
              cross_after.selected_view_resolved == cross_before.selected_view_resolved + 1,
          "Safe cross-recording RT exit resolves unique observed typed view without pending draw");
  D3D12_TEXTURE_COPY_LOCATION cross_source{}, cross_destination{};
  cross_source.pResource = textures[2].get();
  cross_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  cross_destination.pResource = readbacks[0].get();
  cross_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  cross_destination.PlacedFootprint = footprint;
  list->CopyTextureRegion(&cross_destination, 0, 0, 0, &cross_source, nullptr);
  transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  submit();
  reset();
  void* cross_pixels{};
  const D3D12_RANGE cross_range{0, static_cast<SIZE_T>(bytes)}, cross_none{};
  check(readbacks[0]->Map(0, &cross_range, &cross_pixels), "Cross-recording pixels");
  for (UINT y = 0; y < 763; ++y)
    for (UINT x = 0; x < (a350 ? 806u : 768u); ++x) {
      const auto offset = SIZE_T{y} * footprint.Footprint.RowPitch + 4 * x;
      require(std::memcmp(static_cast<const unsigned char*>(cross_pixels) + offset, reference_patch.data() + offset, 4) == 0,
              "Cross-recording selected rectangle matches independent verified patch");
      ++pixels;
    }
  readbacks[0]->Unmap(0, &cross_none);
  if (a350) {
    Reference<ID3D12Resource> ambiguous;
    auto desc = pd;
    desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    create_texture(device.get(), desc, ambiguous.put());
    std::uint64_t selected = 0;
    for (const auto& candidate : win::pfd_inventory())
      selected = std::max(selected, candidate.id);
    require(selected && win::assign_targets(selected, second), "Select typeless target for typed-evidence lifecycle regression");
    Reference<ID3D12DescriptorHeap> views;
    auto view_heap = hd;
    view_heap.NumDescriptors = 2;
    check(device->CreateDescriptorHeap(&view_heap, IID_PPV_ARGS(views.put())), "Typed-evidence descriptor heap");
    const auto view0 = views->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE view1{view0.ptr + stride};
    D3D12_RENDER_TARGET_VIEW_DESC unorm{};
    unorm.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    unorm.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    auto srgb = unorm;
    srgb.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const auto check_fallback = [&](bool copied, const char* reason, bool pending = false) {
      generator.record(list.get(), pending ? view0 : ordinary_rtv, pending ? display_width : 64, pending ? 1024 : 64, false, 0, 0);
      const auto before = win::graphics_status();
      const auto copies = runtime::snapshot(key).stamps;
      transition(list.get(), ambiguous.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      const auto after = win::graphics_status();
      require(runtime::snapshot(key).stamps == copies + copied, "Typed-view lifecycle copy decision");
      require(copied ? after.selected_view_resolved == before.selected_view_resolved + 1
                     : after.selected_view_rejected == before.selected_view_rejected + 1,
              "Typed-view lifecycle diagnostics identify admission or refusal");
      require(std::strcmp(after.copy_error, reason) == 0, "Typed-view refusal reason is specific");
      if (pending)
        require(after.selected_pending_matches == before.selected_pending_matches + 1,
                "Current validated view outranks conflicting other descriptor evidence");
      transition(list.get(), ambiguous.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      submit();
      reset();
    };
    check_fallback(false, "typed_rtv_missing");
    device->CreateRenderTargetView(ambiguous.get(), &unorm, view0);
    device->CreateRenderTargetView(ambiguous.get(), &srgb, view1);
    check_fallback(false, "typed_rtv_conflict");
    check_fallback(true, "copied", true);
    device->CopyDescriptorsSimple(1, view1, ordinary_rtv, hd.Type);
    check_fallback(true, "copied");
    device->CreateRenderTargetView(ambiguous.get(), &srgb, view1);
    check_fallback(false, "typed_rtv_conflict");
    device->CopyDescriptors(1, &view1, nullptr, 1, &view0, nullptr, hd.Type);
    check_fallback(true, "copied");
    device->CreateRenderTargetView(ordinary_target.get(), nullptr, view0);
    device->CreateRenderTargetView(ordinary_target.get(), nullptr, view1);
    check_fallback(false, "typed_rtv_missing");
    device->CreateRenderTargetView(ambiguous.get(), &unorm, view0);
    ambiguous.get()->Release();
    *ambiguous.put() = nullptr;
    require(!win::assign_targets(selected, second), "Retired resource cannot reuse observed typed descriptor evidence");
    device->CopyDescriptorsSimple(1, view1, view0, hd.Type);
    for (const auto& candidate : win::pfd_inventory())
      require(candidate.id != selected, "Descriptor copy cannot revive retired resource metadata");
    require(win::assign_targets(first, second), "Restore targets after typed-evidence regression");
    std::printf("Typed-view lifecycle: missing/conflict refuse; pending/simple/ranged replacement recover\n");
  }
  for (const auto format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
    if (!a350)
      break;  // The A380 profile positively admits only RGBA8 UNORM targets.
    const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool srgb = format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    Reference<ID3D12Resource> typed;
    auto desc = pd;
    desc.Format = format;
    create_texture(device.get(), desc, typed.put());
    const auto candidates = win::pfd_inventory();
    std::uint64_t selected = 0;
    for (const auto& c : candidates)
      if (c.id != first && c.id != second)
        selected = std::max(selected, c.id);
    require(selected && win::assign_targets(selected, second), "Typed PFD identity is explicitly selected");
    const D3D12_CPU_DESCRIPTOR_HANDLE typed_rtv{base.ptr + 6 * stride};
    device->CreateRenderTargetView(typed.get(), nullptr, typed_rtv);
    const float background[]{0, 0, 0, 1};
    list->ClearRenderTargetView(typed_rtv, background, 0, nullptr);
    ClearStatePipeline typed_pipeline(device.get(), false, format);
    list->SetPipelineState(typed_pipeline.pipeline.get());
    list->SetGraphicsRootSignature(typed_pipeline.root.get());
    const UINT constants[]{64, 64, 0, 0};
    list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
    const D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
    const D3D12_RECT scissor{0, 0, 1, 1};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->OMSetRenderTargets(1, &typed_rtv, FALSE, nullptr);
    list->DrawInstanced(3, 1, 0, 0);
    const auto before = runtime::snapshot(key).stamps;
    transition(list.get(), typed.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    require(runtime::snapshot(key).stamps == before + 1, "Typed target receives exactly one private copy");
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = typed.get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource = readbacks[0].get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    destination.PlacedFootprint.Footprint.Format = format;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    submit();
    void* mapped{};
    const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)}, none{};
    check(readbacks[0]->Map(0, &range, &mapped), "Map typed private patch pixels");
    for (UINT y = 0; y < 1024; ++y)
      for (UINT x = 0; x < display_width; ++x) {
        const auto offset = SIZE_T{y} * footprint.Footprint.RowPitch + 4 * x;
        const auto* actual = static_cast<const unsigned char*>(mapped) + offset;
        const auto* ref = reference_patch.data() + offset;
        const bool inside = x < (a350 ? 806u : 768u) && y < 763;
        for (UINT c = 0; c < 4; ++c) {
          const UINT channel = bgra && c != 1 && c != 3 ? 2 - c : c;
          double expected = c == 3 ? 255 : inside ? ref[channel] : 0;
          if (srgb && c != 3) {
            const auto v = expected / 255.;
            expected = 255 * (v <= .0031308 ? 12.92 * v : 1.055 * std::pow(v, 1. / 2.4) - .055);
          }
          require(std::abs(actual[c] - std::round(expected)) <= (srgb ? 1 : 0),
                  "Private typed patch preserves RGBA/BGRA/sRGB pixels and leaves outside pixels untouched");
        }
        ++pixels;
      }
    readbacks[0]->Unmap(0, &none);
    reset();
    require(win::assign_targets(first, second), "Restore initial PFD pair");
  }
  // ClearState changes bindings without starting a new command-list recording.
  // The next target switch must not flush the old PFD against now-unbound RTs,
  // or restore its gradient pipeline over the new green pipeline.
  ClearStatePipeline cleared(device.get());
  Reference<ID3D12Resource> clear_target, clear_readback;
  const auto clear_desc = texture_description(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM);
  create_texture(device.get(), clear_desc, clear_target.put());
  const D3D12_CPU_DESCRIPTOR_HANDLE clear_rtv{base.ptr + 6 * stride};
  device->CreateRenderTargetView(clear_target.get(), nullptr, clear_rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT clear_footprint{};
  UINT64 clear_bytes{};
  device->GetCopyableFootprints(&clear_desc, 0, 1, 0, &clear_footprint, nullptr, nullptr, &clear_bytes);
  auto clear_buffer = bd;
  clear_buffer.Width = clear_bytes;
  check(device->CreateCommittedResource(&bh, D3D12_HEAP_FLAG_NONE, &clear_buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(clear_readback.put())),
        "ClearState readback");
  const auto before_clear_stamps = runtime::snapshot(key).stamps;
  const auto before_clears = win::graphics_status().clear_states;
  generator.record(list.get(), rtvs[2], display_width, 1024, false, 0, 0);
  list->ClearState(cleared.pipeline.get());
  list->OMSetRenderTargets(1, &clear_rtv, FALSE, nullptr);
  list->SetGraphicsRootSignature(cleared.root.get());
  const UINT clear_parameters[]{64, 64, 0, 0};
  list->SetGraphicsRoot32BitConstants(0, 4, clear_parameters, 0);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const D3D12_VIEWPORT clear_viewport{0, 0, 64, 64, 0, 1};
  const D3D12_RECT clear_scissor{0, 0, 64, 64};
  list->RSSetViewports(1, &clear_viewport);
  list->RSSetScissorRects(1, &clear_scissor);
  list->DrawInstanced(3, 1, 0, 0);  // Deliberately no SetPipelineState after ClearState.
  transition(list.get(), clear_target.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION clear_source{}, clear_destination{};
  clear_source.pResource = clear_target.get();
  clear_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  clear_destination.pResource = clear_readback.get();
  clear_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  clear_destination.PlacedFootprint = clear_footprint;
  list->CopyTextureRegion(&clear_destination, 0, 0, 0, &clear_source, nullptr);
  submit();
  void* clear_mapped{};
  const D3D12_RANGE clear_range{0, static_cast<SIZE_T>(clear_bytes)}, clear_none{0, 0};
  check(clear_readback->Map(0, &clear_range, &clear_mapped), "Map ClearState pixels");
  for (UINT y = 0; y < 64; ++y)
    for (UINT x = 0; x < 64; ++x) {
      const auto* pixel = static_cast<const unsigned char*>(clear_mapped) + SIZE_T{y} * clear_footprint.Footprint.RowPitch + 4 * x;
      require(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255,
              "ClearState pipeline survives deferred target switch without PSO rebind");
      ++pixels;
    }
  clear_readback->Unmap(0, &clear_none);
  require(runtime::snapshot(key).stamps == before_clear_stamps, "ClearState discards the pending PFD stamp");
  require(win::graphics_status().clear_states == before_clears + 1, "Observe native ClearState once");
  reset();
  // Native BeginRenderPass switches the command-list vtable. State changed
  // through that active table must survive a later deferred PFD stamp, even
  // when the application does not redundantly rebind its pipeline or root.
  transition(list.get(), clear_target.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  generator.record(list.get(), rtvs[2], display_width, 1024, false, 0, 0);
  const float pass_background[]{1, 0, 0, 1};
  list->ClearRenderTargetView(clear_rtv, pass_background, 0, nullptr);
  Reference<ID3D12GraphicsCommandList4> pass_list;
  check(list->QueryInterface(IID_PPV_ARGS(pass_list.put())), "Render-pass command list");
  D3D12_RENDER_PASS_RENDER_TARGET_DESC pass_target{};
  pass_target.cpuDescriptor = clear_rtv;
  pass_target.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
  pass_target.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
  pass_list->BeginRenderPass(1, &pass_target, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
  list->SetPipelineState(cleared.pipeline.get());
  list->SetGraphicsRootSignature(cleared.root.get());
  const UINT pass_parameters[]{64, 64, 0, 77};
  list->SetGraphicsRoot32BitConstants(0, 4, pass_parameters, 0);
  list->SetGraphicsRoot32BitConstant(0, 0, 3);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const D3D12_VIEWPORT pass_viewport{0, 0, 32, 32, 0, 1};
  const D3D12_RECT pass_scissor{0, 0, 16, 16};
  list->RSSetViewports(1, &pass_viewport);
  list->RSSetScissorRects(1, &pass_scissor);
  list->DrawInstanced(3, 1, 0, 0);
  pass_list->EndRenderPass();
  list->OMSetRenderTargets(1, &rtvs[2], FALSE, nullptr);
  list->DrawInstanced(3, 1, 0, 0);
  const auto before_pass_stamp = runtime::snapshot(key).stamps;
  list->OMSetRenderTargets(1, &clear_rtv, FALSE, nullptr);
  require(runtime::snapshot(key).stamps == before_pass_stamp, "OM switch alone cannot admit a PFD copy");
  transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  require(runtime::snapshot(key).stamps == before_pass_stamp + 1, "PFD stamps after a completed ordinary render pass");
  list->DrawInstanced(3, 1, 0, 0);  // All graphics state still inherited from inside the pass.
  transition(list.get(), clear_target.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyTextureRegion(&clear_destination, 0, 0, 0, &clear_source, nullptr);
  submit();
  check(clear_readback->Map(0, &clear_range, &clear_mapped), "Map post-render-pass application pixels");
  for (UINT y = 0; y < 64; ++y)
    for (UINT x = 0; x < 64; ++x) {
      const auto* pixel = static_cast<const unsigned char*>(clear_mapped) + SIZE_T{y} * clear_footprint.Footprint.RowPitch + 4 * x;
      const bool drawn = x < 16 && y < 16;
      require(pixel[0] == (drawn ? 0 : 255) && pixel[1] == (drawn ? 255 : 0) && pixel[2] == 0 && pixel[3] == 255,
              "PSO/root/constants/viewport/scissor set inside a native render pass survive the next deferred PFD stamp");
      ++pixels;
    }
  clear_readback->Unmap(0, &clear_none);
  reset();
  // Exercise the native bridge with actual shader-visible descriptor heaps,
  // texture/sampler tables and a root CBV. No application bindings are replayed
  // by the fixture after a deferred stamp; the next draw must inherit them.
  ClearStatePipeline textured(device.get(), true);
  Reference<ID3D12DescriptorHeap> texture_heap, sampler_heap;
  D3D12_DESCRIPTOR_HEAP_DESC texture_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  check(device->CreateDescriptorHeap(&texture_heap_desc, IID_PPV_ARGS(texture_heap.put())), "Application texture descriptor heap");
  texture_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  check(device->CreateDescriptorHeap(&texture_heap_desc, IID_PPV_ARGS(sampler_heap.put())), "Application sampler descriptor heap");
  Reference<ID3D12Resource> sampled_texture, tint_buffer;
  create_texture(device.get(), texture_description(2, 2, DXGI_FORMAT_R8G8B8A8_UNORM), sampled_texture.put());
  const D3D12_CPU_DESCRIPTOR_HANDLE sampled_rtv{base.ptr + 7 * stride};
  device->CreateRenderTargetView(sampled_texture.get(), nullptr, sampled_rtv);
  const float sampled_colour[]{.75f, .5f, .25f, 1};
  list->ClearRenderTargetView(sampled_rtv, sampled_colour, 0, nullptr);
  transition(list.get(), sampled_texture.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(sampled_texture.get(), &srv, texture_heap->GetCPUDescriptorHandleForHeapStart());
  D3D12_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  device->CreateSampler(&sampler, sampler_heap->GetCPUDescriptorHandleForHeapStart());
  auto tint_desc = bd;
  tint_desc.Width = 256;
  const auto tint_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
  check(device->CreateCommittedResource(&tint_heap, D3D12_HEAP_FLAG_NONE, &tint_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                        IID_PPV_ARGS(tint_buffer.put())),
        "Application root CBV buffer");
  void* tint_data{};
  check(tint_buffer->Map(0, &clear_none, &tint_data), "Initialize application root CBV");
  const float tint[]{.5f, .5f, 1, 1};
  std::memcpy(tint_data, tint, sizeof(tint));
  tint_buffer->Unmap(0, nullptr);
  transition(list.get(), clear_target.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  list->ClearRenderTargetView(clear_rtv, pass_background, 0, nullptr);
  pass_list->BeginRenderPass(1, &pass_target, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
  list->SetPipelineState(textured.pipeline.get());
  list->SetGraphicsRootSignature(textured.root.get());
  ID3D12DescriptorHeap* app_heaps[]{texture_heap.get(), sampler_heap.get()};
  list->SetDescriptorHeaps(2, app_heaps);
  list->SetGraphicsRootDescriptorTable(1, texture_heap->GetGPUDescriptorHandleForHeapStart());
  list->SetGraphicsRootDescriptorTable(2, sampler_heap->GetGPUDescriptorHandleForHeapStart());
  list->SetGraphicsRootConstantBufferView(3, tint_buffer->GetGPUVirtualAddress());
  const UINT texture_parameters[]{64, 64, 1, 1};
  list->SetGraphicsRoot32BitConstants(0, 4, texture_parameters, 0);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->RSSetViewports(1, &clear_viewport);
  list->RSSetScissorRects(1, &clear_scissor);
  list->DrawInstanced(3, 1, 0, 0);
  pass_list->EndRenderPass();
  list->OMSetRenderTargets(1, &rtvs[2], FALSE, nullptr);
  list->DrawInstanced(3, 1, 0, 0);
  const auto before_descriptor_stamp = runtime::snapshot(key).stamps;
  list->OMSetRenderTargets(1, &clear_rtv, FALSE, nullptr);
  transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  require(runtime::snapshot(key).stamps == before_descriptor_stamp + 1, "Descriptor-bound PFD stamp was not exercised");
  list->DrawInstanced(3, 1, 0, 0);
  transition(list.get(), clear_target.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyTextureRegion(&clear_destination, 0, 0, 0, &clear_source, nullptr);
  submit();
  check(clear_readback->Map(0, &clear_range, &clear_mapped), "Map descriptor restoration pixels");
  for (UINT y = 0; y < 64; ++y)
    for (UINT x = 0; x < 64; ++x) {
      const auto* pixel = static_cast<const unsigned char*>(clear_mapped) + SIZE_T{y} * clear_footprint.Footprint.RowPitch + 4 * x;
      if (!(pixel[0] >= 95 && pixel[0] <= 96 && pixel[1] >= 63 && pixel[1] <= 64 && pixel[2] == 64 && pixel[3] == 255))
        std::fprintf(stderr, "Descriptor pixel (%u,%u): %u/%u/%u/%u\n", x, y, pixel[0], pixel[1], pixel[2], pixel[3]);
      require(pixel[0] >= 95 && pixel[0] <= 96 && pixel[1] >= 63 && pixel[1] <= 64 && pixel[2] == 64 && pixel[3] == 255,
              "Native texture/sampler heaps, descriptor tables and root CBV survive deferred stamp without app rebind");
      ++pixels;
    }
  clear_readback->Unmap(0, &clear_none);
  reset();
  {
    Reference<ID3D12QueryHeap> queries;
    const D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 2, 0};
    check(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(queries.put())), "Application occlusion query heap");
    Reference<ID3D12Resource> results;
    auto result_desc = bd;
    result_desc.Width = 2 * sizeof(UINT64);
    check(device->CreateCommittedResource(&bh, D3D12_HEAP_FLAG_NONE, &result_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(results.put())),
          "Application query result readback");
    for (UINT enabled = 0; enabled < 2; ++enabled) {
      win::set_target_mask(enabled ? 3 : 0);
      const auto copies_before = runtime::snapshot(key).stamps;
      list->BeginQuery(queries.get(), D3D12_QUERY_TYPE_OCCLUSION, enabled);
      generator.record(list.get(), rtvs[2], display_width, 1024, false, 0, 0);
      list->OMSetRenderTargets(1, &clear_rtv, FALSE, nullptr);
      transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      require(runtime::snapshot(key).stamps == copies_before + enabled, "Query comparison exercises one enabled copy and zero OFF copies");
      list->EndQuery(queries.get(), D3D12_QUERY_TYPE_OCCLUSION, enabled);
      list->ResolveQueryData(queries.get(), D3D12_QUERY_TYPE_OCCLUSION, enabled, 1, results.get(), enabled * sizeof(UINT64));
      submit();
      reset();
    }
    const D3D12_RANGE result_range{0, 2 * sizeof(UINT64)};
    void* query_data{};
    check(results->Map(0, &result_range, &query_data), "Read application occlusion results");
    std::array<UINT64, 2> samples{};
    std::memcpy(samples.data(), query_data, sizeof(samples));
    results->Unmap(0, &clear_none);
    std::printf("Application query OFF=%llu ON=%llu\n", samples[0], samples[1]);
    require(samples[0] == UINT64{display_width} * 1024 && samples[1] == samples[0],
            "PFD stamp changed the application's occlusion query result");
  }
  // A real predicate must still refuse injection. Disabling it later cannot
  // revive this recording; only the next successful native Reset can do that.
  Reference<ID3D12Resource> predicate;
  auto predicate_desc = bd;
  predicate_desc.Width = sizeof(std::uint64_t);
  const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
  check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &predicate_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                        IID_PPV_ARGS(predicate.put())),
        "Predicate buffer");
  void* predicate_bytes = nullptr;
  const D3D12_RANGE no_reads{0, 0};
  check(predicate->Map(0, &no_reads, &predicate_bytes), "Initialize predicate");
  std::memset(predicate_bytes, 0, sizeof(std::uint64_t));
  predicate->Unmap(0, nullptr);
  const auto before_predicate = runtime::snapshot(key).stamps;
  list->SetPredication(predicate.get(), 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
  generator.record(list.get(), rtvs[2], display_width, 1024, false, 0, 0);
  require(runtime::snapshot(key).stamps == before_predicate, "Real predication refuses PFD injection");
  list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  generator.record(list.get(), rtvs[2], display_width, 1024, false, 0, 0);
  require(runtime::snapshot(key).stamps == before_predicate, "Disable predication cannot revive invalid recording");
  list->ClearState(nullptr);
  generator.record(list.get(), rtvs[2], display_width, 1024, false, 0, 0);
  require(runtime::snapshot(key).stamps == before_predicate, "ClearState cannot revive a predicate-invalidated recording");
  submit();
  require(runtime::snapshot(key).stamps == before_predicate, "Invalidation survives ClearState through Close");
  reset();
  generator.record(list.get(), rtvs[2], display_width, 1024, false, 0, 0);
  require(runtime::snapshot(key).stamps == before_predicate, "Fresh draw is deferred until list boundary");
  submit();
  require(runtime::snapshot(key).stamps == before_predicate, "Close without positive destination-state proof cannot copy");
  reset();
  win::set_target_mask(0);
  const auto stamps = runtime::snapshot(key).stamps;
  generator.record(list.get(), rtvs[2], display_width, 1024, false, 0, 0);
  require(runtime::snapshot(key).stamps == stamps, "OFF state stops new PFD stamping");
  submit();
  reset();
  // Apply all four guide pairs to an already-running compositor. Fresh source
  // pixels distinguish live layout changes from merely replaying an old patch.
  const auto source_handles =
      std::array<std::uint64_t, 2>{reinterpret_cast<std::uint64_t>(textures[0].get()), reinterpret_cast<std::uint64_t>(textures[1].get())};
  const auto source_before = handoff.observe_copy(key, source_handles[0], source_handles[1]);
  const auto publications_before = handoff.diagnostics().publications;
  require(source_before.source.matched && source_before.destination.matched && source_before.source.entry_id == 701 &&
              source_before.destination.entry_id == 702,
          "Live guide adjustment starts with the original camera identities");
  auto adjusted = profile.composition;
  adjusted.nose_dot = {.23f, .34f};
  adjusted.tail_upper = {.18f, .30f};
  adjusted.tail_corner = {.16f, .55f};
  adjusted.tail_inner = {.26f, .56f};
  runtime::set_composition(key, adjusted);
  const auto frames_before_guides = runtime::snapshot(key).frames;
  Sleep(20);  // Next permitted capture opportunity, without recreating either source.
  generator.record(list.get(), rtvs[0], pane_width, nose_height, false, 1, 0);
  generator.record(list.get(), rtvs[1], pane_width, tail_height, false, 1, 1);
  // Earlier predicate tests deliberately invalidate tracked states. These
  // actual source exits establish fresh, explicit RT-state evidence again.
  for (unsigned feed = 0; feed < 2; ++feed) {
    transition(list.get(), textures[feed].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list.get(), textures[feed].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  submit();
  reset();
  const auto guide_deadline = GetTickCount64() + 1000;
  while (runtime::snapshot(key).frames == frames_before_guides && GetTickCount64() < guide_deadline) {
    runtime::service();
    Sleep(1);
  }
  if (runtime::snapshot(key).frames == frames_before_guides) {
    const auto status = runtime::snapshot(key);
    std::fprintf(stderr, "Live guide composition stalled: frames=%llu captures=%llu completed=%llu tail=%s message=%s\n", status.frames,
                 status.capture.captures, status.capture.completed, status.capture.tail_status, status.message);
  }
  require(runtime::snapshot(key).output && runtime::snapshot(key).frames > frames_before_guides,
          "Changed guide layout reaches a fresh running composition");
  const auto source_after = handoff.observe_copy(key, source_handles[0], source_handles[1]);
  for (const auto& pair :
       {std::pair{source_before.source, source_after.source}, std::pair{source_before.destination, source_after.destination}})
    require(pair.second.matched && pair.first.entry_id == pair.second.entry_id && pair.first.manager == pair.second.manager &&
                pair.first.resource == pair.second.resource && pair.first.scene_epoch == pair.second.scene_epoch &&
                handoff.is_current(pair.first),
            "Guide adjustment retains camera IDs, owner, scene and source resource generations");
  require(handoff.diagnostics().publications == publications_before, "Guide adjustment needs no new camera publication");
  win::set_target_mask(3);
  const auto stamps_before_guides = runtime::snapshot(key).stamps;
  for (UINT side = 0; side < 2; ++side) {
    generator.record(list.get(), rtvs[side + 2], display_width, 1024, false, 0, 0);
    if (a350) {
      const float grey[]{.25f, .25f, .25f, 1};
      const D3D12_RECT gutter{806, 0, 838, 763};
      list->ClearRenderTargetView(rtvs[side + 2], grey, 1, &gutter);
    }
  }
  for (UINT side = 0; side < 2; ++side) {
    transition(list.get(), textures[side + 2].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = textures[side + 2].get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource = readbacks[side].get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(list.get(), textures[side + 2].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  submit();
  require(runtime::snapshot(key).stamps == stamps_before_guides + 2, "Both PFDs receive the adjusted private patch");
  // Independent working-pixel centres for the chosen normalized test inputs:
  // nose pair, upper L endpoints, outside corners, and inner L endpoints.
  constexpr std::array<std::array<double, 2>, 8> new_points{{{176.64, 86.70},
                                                             {591.36, 86.70},
                                                             {138.24, 410.20},
                                                             {629.76, 410.20},
                                                             {122.88, 536.20},
                                                             {645.12, 536.20},
                                                             {199.68, 541.24},
                                                             {568.32, 541.24}}};
  std::array<std::array<double, 2>, 8> old_points{};
  const std::array old_normalized{profile.composition.nose_dot, profile.composition.tail_upper, profile.composition.tail_corner,
                                  profile.composition.tail_inner};
  for (UINT mark = 0; mark < old_normalized.size(); ++mark) {
    const double x = old_normalized[mark][0] * 768, y = mark ? 259 + old_normalized[mark][1] * 504 : old_normalized[mark][1] * 255;
    old_points[mark * 2] = {x, y};
    old_points[mark * 2 + 1] = {768 - x, y};
  }
  for (UINT side = 0; side < 2; ++side) {
    void* mapped{};
    const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)}, none{};
    check(readbacks[side]->Map(0, &range, &mapped), "Map live guide adjustment pixels");
    const auto* data = static_cast<const unsigned char*>(mapped);
    const UINT left = a350 && side ? 838u : 0u, width = a350 ? 806u : 768u, inner_left = left + 16, inner_width = width - 32;
    unsigned new_mask = 0, old_mask = 0;
    for (UINT y = 0; y < 1024; ++y)
      for (UINT x = 0; x < display_width; ++x) {
        const auto* pixel = data + SIZE_T{y} * footprint.Footprint.RowPitch + 4 * x;
        const bool outer = x >= left && x < left + width && y < 763;
        const bool inner = x >= inner_left && x < inner_left + inner_width && y >= 12 && y < 763;
        if (outer && !inner)
          require(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255, "Live guide edit preserves the black camera border");
        if (!outer) {
          if (a350 && y < 763 && x >= 806 && x < 838)
            require(pixel[0] >= 63 && pixel[0] <= 65 && pixel[1] >= 63 && pixel[1] <= 65 && pixel[2] >= 63 && pixel[2] <= 65,
                    "Live guide edit preserves the A350 grey gutter");
          else
            require(pixel[2] >= 50 && pixel[2] <= 52, "Live guide edit preserves navigation area and lower trim");
        }
        if (inner) {
          const double wx = std::floor((x - inner_left + .5) * 768 / inner_width) + .5;
          const double wy = std::floor((y - 12 + .5) * 763 / 751) + .5;
          for (unsigned mark = 0; mark < new_points.size(); ++mark) {
            if (std::abs(wx - new_points[mark][0]) < 1 && std::abs(wy - new_points[mark][1]) < 1) {
              require(pixel[0] > 250 && (a350 ? pixel[1] > 135 && pixel[1] < 145 && pixel[2] < 3 : pixel[1] < 3 && pixel[2] > 250),
                      "Every adjusted nose dot and mirrored L endpoint appears at its new position");
              new_mask |= 1u << mark;
            }
            if (std::abs(wx - old_points[mark][0]) < 1 && std::abs(wy - old_points[mark][1]) < 1) {
              const unsigned blue = mark < 2 ? 153 : 102;
              require(pixel[2] >= blue - 1 && pixel[2] <= blue + 1, "Old guide endpoints return to fresh camera imagery");
              old_mask |= 1u << mark;
            }
          }
        }
        ++pixels;
      }
    require(new_mask == 255 && old_mask == 255, "Live adjustment checks every old and new mirrored endpoint on each PFD");
    readbacks[side]->Unmap(0, &none);
  }
  runtime::set_composition(key, profile.composition);
  win::set_target_mask(0);
  reset();
  list->Close();
  runtime::manager().stop_source_tracking();
  handoff.stop_scene();
  runtime::reset_feed(key);
  require(win::graphics_status().hook_failures == 0, "No native hook failures");
  std::uint64_t errors = 0;
  if (messages.get()) {
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
      SIZE_T size{};
      messages->GetMessage(i, nullptr, &size);
      std::vector<unsigned char> memory(size);
      auto* msg = reinterpret_cast<D3D12_MESSAGE*>(memory.data());
      if (SUCCEEDED(messages->GetMessage(i, msg, &size)) && msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
        ++errors;
        std::fprintf(stderr, "D3D12 error %u: %s\n", static_cast<unsigned>(msg->ID), msg->pDescription);
      }
    }
  }
  require(errors == 0, "D3D12 validation errors");
  std::printf(
      "PASS native %s: private patch copies, legacy/enhanced boundaries, replay, profile-admitted typed formats, query OFF==ON, exact "
      "black borders, "
      "inset "
      "GS/guides, live guide adjustment on retained sources, A350 gutter/ND, lower trim, descriptor copies, "
      "OFF, D3D11On12 capture coexistence, ClearState and active-render-pass state pixels, predicate guards; %llu pixels; debug=%d "
      "errors=%llu\n",
      warp ? "WARP" : "hardware", static_cast<unsigned long long>(pixels), debug_enabled, static_cast<unsigned long long>(errors));
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    bool warp = false, a350 = false;
    for (int i = 1; i < argc; ++i) {
      if (std::wcscmp(argv[i], L"--warp") == 0)
        warp = true;
      else if (std::wcscmp(argv[i], L"--a350") == 0)
        a350 = true;
      else
        return 2;
    }
    native_case(warp, a350);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL native graphics: %s\n", e.what());
    return 1;
  }
}
