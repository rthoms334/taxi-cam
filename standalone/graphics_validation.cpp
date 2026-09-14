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
  explicit ClearStatePipeline(ID3D12Device* device) {
    // Match the gradient's root layout, but make pipeline loss visible as
    // different GPU pixels rather than relying only on intercepted-call counts.
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues = 4;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_ROOT_SIGNATURE_DESC signature{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
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
float4 ps_main() : SV_Target {
  return float4(Mode != 0 ? 1 : 0, Width == 64 && Height == 64 ? 1 : 0, Frame != 0 ? 1 : 0, 1);
}
)";
    check(D3DCompile(shader, sizeof(shader) - 1, "clear_state_pipeline", nullptr, nullptr, "vs_main", "vs_5_0",
                     D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vertex.put(), nullptr),
          "Compile ClearState vertex shader");
    check(D3DCompile(shader, sizeof(shader) - 1, "clear_state_pipeline", nullptr, nullptr, "ps_main", "ps_5_0",
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
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
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
  for (UINT side = 0; side < 2; ++side) {
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
  require(runtime::snapshot(key).stamps == 1, "One overlay at target switch; repeated glyph draws stay deferred");
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
  for (UINT i = 0; i < 2; ++i) {
    check(device->CreateCommittedResource(&bh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(readbacks[i].put())),
          "Test-only readback");
    transition(list.get(), textures[i + 2].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = textures[i + 2].get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readbacks[i].get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.get(), textures[i + 2].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  submit();
  require(runtime::snapshot(key).stamps == 2, "One overlay per completed PFD batch, including RT-to-copy boundary");
  reset();
  std::uint64_t pixels = 0;
  for (UINT side = 0; side < 2; ++side) {
    void* mapped{};
    const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    check(readbacks[side]->Map(0, &range, &mapped), "Map verification readback");
    const auto* data = static_cast<const unsigned char*>(mapped);
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
          if (working_y >= 245 && working_y < 269)
            require(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0, "Black divider scales with the complete working image");
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
          // Independent A350 default lower corners sit below/outside the bogies.
          if (a350 && (working_x == 207 || working_x == 560) && working_y == 682) {
            require(pixel[0] > 250 && pixel[1] > 135 && pixel[1] < 145 && pixel[2] < 3,
                    "A350 lower brackets appear beside the bogies on both sides");
            ++tail_guide_pixels;
          }
          if (a350 && working_x == 234 && working_y == 637)
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
  list->OMSetRenderTargets(1, &clear_rtv, FALSE, nullptr);  // Flush the PFD before switching.
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
  require(runtime::snapshot(key).stamps == before_predicate + 1, "Close flushes a valid pending overlay after fresh Reset");
  reset();
  win::set_target_mask(0);
  const auto stamps = runtime::snapshot(key).stamps;
  generator.record(list.get(), rtvs[2], display_width, 1024, false, 0, 0);
  require(runtime::snapshot(key).stamps == stamps, "OFF state stops new PFD stamping");
  submit();
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
      "PASS native %s: two GPU feeds, two PFDs, pre-existing root/list/queue, partial state restoration, exact black borders, inset "
      "GS/guides, A350 gutter/ND, lower trim, descriptor copies, "
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
