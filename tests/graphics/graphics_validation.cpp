#include <d3d11on12.h>
#include "../../src/bridge/d3d12_bridge.hpp"
#include "../../src/bridge/native_hooks.hpp"
#include "../../src/graphics/scene_frame_output.hpp"
#include "../support/graphics_fixture.hpp"
namespace {
using namespace taxi_camera::testing;
namespace win = taxi_camera::standalone;
namespace runtime = taxi_camera::scene_runtime;
void patch_demand_case() {
  taxi_camera::SceneFrameOutput output;
  const auto request = [&](const taxi_camera::profiles::AircraftProfile& profile, DXGI_FORMAT format) {
    const auto outer = taxi_camera::profiles::display_rect(profile, 0);
    const auto inner = taxi_camera::profiles::display_content_rect(profile, 0);
    const UINT width = outer.right - outer.left, height = outer.bottom - outer.top;
    const D3D12_RECT content{static_cast<LONG>(inner.left - outer.left), static_cast<LONG>(inner.top - outer.top),
                             static_cast<LONG>(inner.right - outer.left), static_cast<LONG>(inner.bottom - outer.top)};
    const auto before = output.patch_requests();
    require(!output.request_patch(format, width + 1, height, content), "Wrong patch width cannot reserve work");
    auto wrong = content;
    ++wrong.left;
    require(!output.request_patch(format, width, height, wrong), "Wrong content rectangle cannot reserve work");
    require(!output.request_patch(DXGI_FORMAT_R8G8B8A8_TYPELESS, width, height, content), "Untyped patch cannot reserve work");
    require(output.patch_requests() == before && !output.patch_draws(), "Refused requests cannot mutate demand or record work");
    require(output.request_patch(format, width, height, content), "Exact current-profile patch demand admitted");
    const auto admitted = output.patch_requests();
    require(output.request_patch(format, width, height, content) && output.patch_requests() == admitted,
            "Repeated copy demand shares one stable slot");
    require(!output.patch(format, width, height, content).buffer && !output.patch_draws(),
            "Metadata admission never allocates, records or publishes a GPU patch");
  };
  require(!output.patch_requests() && !output.patch_draws(), "No patch work before copy demand");
  request(taxi_camera::profiles::A380, DXGI_FORMAT_R8G8B8A8_UNORM);
  require(output.patch_requests() == 1, "A380 admits one requested slot");
  require(output.set_patch_profile(2), "Select A350 metadata without a GPU");
  for (const auto format :
       {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
    request(taxi_camera::profiles::A359, format);
  require(output.patch_requests() == 5, "A350 requests retain the earlier A380 replay slot");
  require(output.set_patch_profile(3), "Select A350-1000");
  request(taxi_camera::profiles::A35K, DXGI_FORMAT_R8G8B8A8_UNORM);
  require(output.patch_requests() == 5, "Matching profile geometry reuses its exact typed slot");
  require(!output.set_patch_profile(99) && output.set_patch_profile(1), "Invalid profile refuses without losing prior slots");
  request(taxi_camera::profiles::A380, DXGI_FORMAT_R8G8B8A8_UNORM);
  require(output.patch_requests() == 5 && !output.patch_draws(), "Profile roundtrip retains bounded demand without GPU work");
  std::printf("PASS CPU-only typed patch demand: cold, invalid, duplicate, typed formats and retained profile geometry.\n");
}
struct ClearStatePipeline {
  Reference<ID3D12RootSignature> root;
  Reference<ID3D12PipelineState> pipeline;
  explicit ClearStatePipeline(ID3D12Device* device,
                              bool descriptor_case = false,
                              DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM,
                              DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN,
                              bool gray_case = false) {
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
float4 ps_main(float4 position : SV_Position) : SV_Target {
  float4 value = float4(Mode != 0 ? 1 : 0, Width == 64 && Height == 64 ? 1 : 0, Frame != 0 ? 1 : 0, 1);
#ifdef DESCRIPTOR_CASE
#ifdef GRAY_CASE
  float2 uv = frac(position.xy / float2(7,9));
  value = Texture.SampleLevel(Sample, uv, Frame) * Tint;
#else
  value *= Texture.SampleLevel(Sample, float2(.5,.5), 0) * Tint;
#endif
#endif
  return value;
}
)";
    const D3D_SHADER_MACRO macros[]{{"DESCRIPTOR_CASE", "1"}, {gray_case ? "GRAY_CASE" : nullptr, "1"}, {nullptr, nullptr}};
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
    if (gray_case) {
      blend.BlendEnable = TRUE;
      blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
      blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    }
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = depth_format != DXGI_FORMAT_UNKNOWN;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = depth_format == DXGI_FORMAT_UNKNOWN ? D3D12_COMPARISON_FUNC_ALWAYS : D3D12_COMPARISON_FUNC_LESS;
    desc.DSVFormat = depth_format;
    desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                                                          D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.SampleDesc.Count = 1;
    desc.RTVFormats[0] = format;
    check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pipeline.put())), "Create ClearState pipeline");
  }
};
// Trace the real stamp setup against a recording sink. Unlike the pixel oracle,
// this rejects redundant changes to any state outside the chosen diagnostic.
namespace setup_trace {
std::vector<unsigned> calls;
ID3D12RootSignature* last_root{};
bool sample_query_allowed = true, samples_normalized = false;
unsigned sample_queries{}, sample_releases{};
HRESULT STDMETHODCALLTYPE query(void* self, REFIID iid, void** output) {
  *output = nullptr;
  ++sample_queries;
  if (!sample_query_allowed || iid != __uuidof(ID3D12GraphicsCommandList1))
    return E_NOINTERFACE;
  *output = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE release(void*) {
  ++sample_releases;
  return 1;
}
D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE type(void*) {
  return D3D12_COMMAND_LIST_TYPE_DIRECT;
}
void STDMETHODCALLTYPE pipeline(void*, ID3D12PipelineState*) {
  calls.push_back(25);
}
void STDMETHODCALLTYPE root(void*, ID3D12RootSignature* value) {
  last_root = value;
  calls.push_back(30);
}
void STDMETHODCALLTYPE table(void*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE) {
  calls.push_back(32);
}
void STDMETHODCALLTYPE constant(void*, UINT, UINT, UINT) {
  calls.push_back(34);
}
void STDMETHODCALLTYPE cbv(void*, UINT, UINT64) {
  calls.push_back(38);
}
void STDMETHODCALLTYPE uav(void*, UINT, UINT64) {
  calls.push_back(42);
}
void STDMETHODCALLTYPE srv(void*, UINT, UINT64) {
  calls.push_back(40);
}
void STDMETHODCALLTYPE constants(void*, UINT, UINT, const void*, UINT) {
  calls.push_back(36);
}
void STDMETHODCALLTYPE topology(void*, D3D_PRIMITIVE_TOPOLOGY) {
  calls.push_back(20);
}
void STDMETHODCALLTYPE viewport(void*, UINT, const D3D12_VIEWPORT*) {
  calls.push_back(21);
}
void STDMETHODCALLTYPE scissor(void*, UINT, const D3D12_RECT*) {
  calls.push_back(22);
}
void STDMETHODCALLTYPE draw(void*, UINT, UINT, UINT, UINT) {
  calls.push_back(12);
}
void STDMETHODCALLTYPE samples(void*, UINT count, UINT pixels, D3D12_SAMPLE_POSITION* positions) {
  calls.push_back(63);
  samples_normalized = !count && !pixels && !positions;
}
void STDMETHODCALLTYPE bias(void*, FLOAT, FLOAT, FLOAT) {
  calls.push_back(82);
}
void STDMETHODCALLTYPE cut(void*, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE) {
  calls.push_back(83);
}
void verify(ID3D12Device* device, D3D12_GPU_VIRTUAL_ADDRESS address) {
  using namespace taxi_camera;
  const win::OwnedWork owned;
  PfdStampD3D12 stamp;
  check(stamp.initialize(device, DXGI_FORMAT_R8G8B8A8_UNORM), "Initialize diagnostic setup trace");
  std::array<void*, 84> slots{};
  slots[0] = reinterpret_cast<void*>(&query);
  slots[2] = reinterpret_cast<void*>(&release);
  slots[8] = reinterpret_cast<void*>(&type);
  slots[12] = reinterpret_cast<void*>(&draw);
  slots[25] = reinterpret_cast<void*>(&pipeline);
  slots[30] = reinterpret_cast<void*>(&root);
  slots[32] = reinterpret_cast<void*>(&table);
  slots[34] = reinterpret_cast<void*>(&constant);
  slots[38] = reinterpret_cast<void*>(&cbv);
  slots[40] = reinterpret_cast<void*>(&srv);
  slots[42] = reinterpret_cast<void*>(&uav);
  slots[36] = reinterpret_cast<void*>(&constants);
  slots[20] = reinterpret_cast<void*>(&topology);
  slots[21] = reinterpret_cast<void*>(&viewport);
  slots[22] = reinterpret_cast<void*>(&scissor);
  slots[63] = reinterpret_cast<void*>(&samples);
  slots[82] = reinterpret_cast<void*>(&bias);
  slots[83] = reinterpret_cast<void*>(&cut);
  auto* pointer = slots.data();
  auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(&pointer);
  const std::array<std::vector<unsigned>, 4> expected{{{25, 30, 40, 36, 20, 21, 22}, {25}, {30, 40, 36}, {20, 21, 22}}};
  for (unsigned group = 0; group < 4; ++group) {
    calls.clear();
    require(stamp.record_private_patch(list, device, address, 768, 1024, nullptr, nullptr, false, static_cast<PfdStateGroup>(group)),
            "Record exact diagnostic setup group");
    require(calls == expected[group], "Diagnostic setup touched another state group or issued a draw");
    if (group) {
      calls.clear();
      require(!stamp.record_private_patch(list, device, address, 768, 1024, nullptr, nullptr, true, static_cast<PfdStateGroup>(group)) &&
                  calls.empty(),
              "Partial state cannot be used for drawing and must refuse before mutation");
    }
  }
  calls.clear();
  require(!stamp.record_private_patch(list, device, address, 768, 1024, nullptr, nullptr, false, static_cast<PfdStateGroup>(99)) &&
              calls.empty(),
          "Invalid group refuses before mutation");
  std::puts("PASS diagnostic setup: exact all/pipeline/root/raster setters, no draw; partial draws and invalid group refused.");
  PfdGraphicsState state;
  state.reset(1, true);
  state.bind_pipeline(reinterpret_cast<ID3D12PipelineState*>(UINT_PTR{1}));
  state.bind_observed_root(reinterpret_cast<ID3D12RootSignature*>(UINT_PTR{2}), 2);
  const UINT application_value = 17;
  state.constants(0, 3, 1, &application_value);
  state.table(1, 0x1000);
  state.descriptor(2, PfdRootKind::cbv, 0x2000);
  state.topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const D3D12_VIEWPORT application_viewport{64, 64, 64, 64, 0, 1};
  const D3D12_RECT application_scissor{64, 64, 128, 128};
  state.viewports(0, 1, &application_viewport);
  state.scissors(0, 1, &application_scissor);
  auto* native9 = reinterpret_cast<d3d12_extended::CommandList9*>(list);
  state.depth_bias(native9, -4, 0, 0);
  state.strip_cut(native9, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
  require(state.can_restore(list), "Final stamp fixture has complete distinct application state");
  calls.clear();
  require(stamp.record_final_buffer(list, state, device, address, 768, 1024), "Record production final-buffer draw");
  require(calls == std::vector<unsigned>{25, 30, 40, 36, 20, 21, 22, 12} && last_root != state.root(),
          "Final-buffer path records camera setup and draw with no application root, pipeline or raster restoration");
  const D3D12_SAMPLE_POSITION positions[]{{-3, 2}, {4, -1}};
  state.sample_positions(reinterpret_cast<ID3D12GraphicsCommandList1*>(list), 2, 1, positions);
  calls.clear();
  require(stamp.record_final_buffer(list, state, device, address, 768, 1024), "Record final draw with an application sample pattern");
  require(calls == std::vector<unsigned>{63, 25, 30, 40, 36, 20, 21, 22, 12} && samples_normalized && sample_queries == 1 &&
              sample_releases == 1 && last_root != state.root(),
          "Final draw normalizes samples once and never restores any application state afterward");
  calls.clear();
  const D3D12_RECT invalid_rectangle{0, 0, 769, 1024};
  require(!stamp.record_final_buffer(list, state, device, address, 768, 1024, &invalid_rectangle) && calls.empty() && sample_queries == 1,
          "Invalid final rectangle refuses before sample or graphics mutation");
  require(!stamp.record_final_buffer(list, state, device, address + 1, 768, 1024) && calls.empty() && sample_queries == 1,
          "Invalid final buffer address refuses before sample or graphics mutation");
  sample_query_allowed = false;
  require(
      !stamp.record_final_buffer(list, state, device, address, 768, 1024) && calls.empty() && sample_queries == 2 && sample_releases == 1,
      "Unavailable sample-position interface refuses the final draw without mutation");
  sample_query_allowed = true;
  std::puts("PASS final-buffer setup: one camera draw, no application replay, exact sample reset and pre-mutation refusals.");
}
}  // namespace setup_trace

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

void active_profile_switch_case(bool warp) {
  using namespace taxi_camera;
  Reference<ID3D12Debug> debug;
  const bool debug_enabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())));
  if (debug_enabled)
    debug->EnableDebugLayer();
  Reference<IDXGIFactory4> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "Profile switch factory");
  Reference<IDXGIAdapter> adapter;
  if (warp)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "Profile switch WARP");
  Reference<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.put())), "Profile switch device");
  Reference<ID3D12InfoQueue> messages;
  if (debug_enabled)
    device->QueryInterface(IID_PPV_ARGS(messages.put()));
  GradientGenerator generator(device.get());
  require(win::initialize_graphics(device.get()), win::graphics_status().error);
  const auto key = win::graphics_status().device;
  win::set_aircraft_profile(profiles::A380.id);
  require(runtime::prepare(key), "Prepare actual running compositor for profile switch");
  runtime::manager().set_source_rate(60);
  D3D12_COMMAND_QUEUE_DESC qd{};
  Reference<ID3D12CommandQueue> queue;
  Reference<ID3D12CommandAllocator> allocator;
  Reference<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.put())), "Profile switch queue");
  check(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(allocator.put())), "Profile switch allocator");
  check(device->CreateCommandList(0, qd.Type, allocator.get(), nullptr, IID_PPV_ARGS(list.put())), "Profile switch list");
  std::array<Reference<ID3D12Resource>, 2> sources;
  std::array<Reference<ID3D12Resource>, 4> displays, readbacks;
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 4> footprints{};
  std::array<UINT64, 4> byte_counts{};
  Reference<ID3D12DescriptorHeap> heap;
  const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 6, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(heap.put())), "Profile switch RTVs");
  const auto base = heap->GetCPUDescriptorHandleForHeapStart();
  const auto stride = device->GetDescriptorHandleIncrementSize(hd.Type);
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 6> rtvs{};
  for (UINT i = 0; i < rtvs.size(); ++i)
    rtvs[i] = {base.ptr + SIZE_T{i} * stride};
  for (UINT feed = 0; feed < 2; ++feed) {
    const auto pane = profiles::A380.camera_panes[feed];
    create_texture(device.get(), texture_description(pane[0], pane[1], DXGI_FORMAT_R8G8B8A8_UNORM), sources[feed].put());
    device->CreateRenderTargetView(sources[feed].get(), nullptr, rtvs[feed]);
  }
  for (UINT i = 0; i < 4; ++i) {
    const auto& profile = i < 2 ? profiles::A380 : profiles::A359;
    auto d = texture_description(profile.width, 1024, DXGI_FORMAT_R8G8B8A8_UNORM);
    d.MipLevels = profile.mips ? profile.mips : 1;
    create_texture(device.get(), d, displays[i].put());
    D3D12_RENDER_TARGET_VIEW_DESC view{};
    view.Format = d.Format;
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(displays[i].get(), &view, rtvs[i + 2]);
    device->GetCopyableFootprints(&d, 0, 1, 0, &footprints[i], nullptr, nullptr, &byte_counts[i]);
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = byte_counts[i];
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto props = heap_properties(D3D12_HEAP_TYPE_READBACK);
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(readbacks[i].put())),
          "Profile switch readback");
  }
  const std::array<std::uint64_t, 2> handles{reinterpret_cast<std::uint64_t>(sources[0].get()),
                                             reinterpret_cast<std::uint64_t>(sources[1].get())};
  auto& handoff = scene_handoff();
  const SceneManagerIdentity manager{501, 1};
  const std::array<std::uint64_t, 2> entries{701, 702};
  const auto submit = [&] {
    check(list->Close(), "Profile switch Close");
    ID3D12CommandList* batch[]{list.get()};
    queue->ExecuteCommandLists(1, batch);
    require(drain_copy_queue(queue.get(), device.get()), "Profile switch GPU completion");
    check(allocator->Reset(), "Profile switch allocator Reset");
    check(list->Reset(allocator.get(), nullptr), "Profile switch observed Reset");
  };
  const auto service_until = [&](auto predicate, const char* label) {
    const auto deadline = GetTickCount64() + 5000;
    while (!predicate() && GetTickCount64() < deadline) {
      runtime::service();
      Sleep(1);
    }
    const auto status = runtime::snapshot(key);
    if (!predicate())
      std::fprintf(stderr, "%s: output=%d frames=%llu completed=%llu tail=%s error=%s\n", label, status.output, status.frames,
                   status.capture.completed, status.capture.tail_status, status.message);
    require(predicate() && !status.failed, label);
  };
  const auto draw_source = [&](UINT feed, UINT frame) {
    const auto pane = profiles::A380.camera_panes[feed];
    generator.record(list.get(), rtvs[feed], pane[0], pane[1], false, frame, feed);
    submit();
  };
  const auto render_displays = [&](UINT offset, unsigned mask) {
    win::set_target_mask(mask);
    const auto before_close = runtime::snapshot(key).stamps;
    const UINT width = offset ? 1644 : 768;
    for (UINT side = 0; side < 2; ++side)
      generator.record(list.get(), rtvs[offset + side + 2], width, 1024, false, 0, 0);
    list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    require(runtime::snapshot(key).stamps == before_close, "Profile-switch PFD drawings remain deferred until Close");
    submit();
    // Close completes both deferred stamps. Disable further RT-exit copies and
    // use a separate recording so readback cannot cause a second display write.
    win::set_target_mask(0);
    for (UINT side = 0; side < 2; ++side) {
      const UINT index = offset + side;
      transition(list.get(), displays[index].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
      src.pResource = displays[index].get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.pResource = readbacks[index].get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = footprints[index];
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      transition(list.get(), displays[index].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    submit();
    std::array<std::vector<unsigned char>, 2> pixels;
    for (UINT side = 0; side < 2; ++side) {
      const UINT index = offset + side;
      void* mapped{};
      const D3D12_RANGE range{0, static_cast<SIZE_T>(byte_counts[index])}, none{};
      check(readbacks[index]->Map(0, &range, &mapped), "Profile switch image map");
      const auto* bytes = static_cast<const unsigned char*>(mapped);
      pixels[side].assign(bytes, bytes + byte_counts[index]);
      readbacks[index]->Unmap(0, &none);
    }
    return pixels;
  };
  std::array<std::uint64_t, 2> old_routes{};
  SceneCopyObservation initial_match{}, previous_match{};
  UINT64 checked_pixels = 0;
  for (UINT phase = 0; phase < 3; ++phase) {
    const bool a350 = phase == 1;
    const auto& profile = a350 ? profiles::A359 : profiles::A380;
    const UINT offset = a350 ? 2 : 0;
    const auto old_status = runtime::snapshot(key);
    if (phase) {
      require(old_status.output && old_status.frames > 0, "Profile changes while a real camera output is active");
      win::set_target_mask(0);
      runtime::manager().stop_source_tracking();
      handoff.stop_scene();
      runtime::reset_feed(key);
    }
    win::set_aircraft_profile(profile.id);
    runtime::set_composition(key, profile.composition);
    require(win::target_ids() == std::array<std::uint64_t, 2>{}, "Active profile change clears all previous PFD routes");
    if (phase)
      require(!win::assign_targets(old_routes[0], old_routes[1]), "Other-profile PFD routes cannot be reused");
    const auto inventory = win::pfd_inventory();
    require(inventory.size() == 2, "Only the current profile's live PFD sizes are eligible");
    const std::array current_routes{std::min(inventory[0].id, inventory[1].id), std::max(inventory[0].id, inventory[1].id)};
    require(win::assign_targets(current_routes[0], current_routes[1]), "Bind current-profile PFD identities explicitly");
    handoff.begin_scene();
    require(handoff.publish(handoff.begin_capture(), manager, entries, handles), "Retained sources get a fresh scene publication");
    const auto match = handoff.observe_copy(key, handles[0], handles[1]);
    require(match.source.matched && match.destination.matched, "Both retained camera sources match new publication");
    if (!phase)
      initial_match = match;
    else {
      require(!handoff.is_current(previous_match.source) && !handoff.is_current(previous_match.destination),
              "Previous scene matches are stale");
      require(match.source.scene_epoch != previous_match.source.scene_epoch, "Retained pair receives a new scene epoch");
    }
    for (const auto& pair : {std::pair{initial_match.source, match.source}, std::pair{initial_match.destination, match.destination}})
      require(pair.first.resource == pair.second.resource && pair.first.entry_id == pair.second.entry_id &&
                  pair.first.manager == pair.second.manager,
              "Aircraft switch retains native resource generation, camera entry IDs and manager");
    for (UINT feed = 0; feed < 2; ++feed) {
      const auto d = sources[feed]->GetDesc();
      const auto pane = profiles::A380.camera_panes[feed];
      require(d.Width == static_cast<UINT>(pane[0]) && d.Height == static_cast<UINT>(pane[1]),
              "Original A380 camera allocation dimensions are retained");
    }
    runtime::manager().begin_source_tracking();
    runtime::reset_feed(key);
    runtime::service();
    require(!runtime::snapshot(key).output && runtime::snapshot(key).frames == old_status.frames,
            "Old composed frame is unavailable before fresh sources");
    const auto old_stamps = runtime::snapshot(key).stamps;
    render_displays(offset, 3);
    require(runtime::snapshot(key).stamps == old_stamps, "No stale profile image can stamp before either fresh feed");
    const auto completed = runtime::snapshot(key).capture.completed;
    draw_source(0, phase & 1);
    service_until([&] { return runtime::snapshot(key).capture.completed > completed; }, "Fresh nose capture completes");
    require(!runtime::snapshot(key).output && runtime::snapshot(key).frames == old_status.frames,
            "One fresh feed cannot pair with the old profile tail");
    render_displays(offset, 3);
    require(runtime::snapshot(key).stamps == old_stamps, "No stale mixed-profile image can stamp after only one fresh feed");
    Sleep(20);
    draw_source(1, phase & 1);
    service_until([&] { return runtime::snapshot(key).output && runtime::snapshot(key).frames > old_status.frames; },
                  "Both fresh retained-source feeds compose under new profile");
    const auto off = render_displays(offset, 0);
    const auto before = runtime::snapshot(key).stamps;
    const auto on = render_displays(offset, 3);
    require(runtime::snapshot(key).stamps == before + 2, "Fresh camera images reach both current-profile PFDs");
    UINT nose_pixels = 0, tail_pixels = 0;
    for (UINT side = 0; side < 2; ++side) {
      const UINT left = a350 && side ? 838u : 0u, width = a350 ? 806u : 768u;
      const auto row_pitch = footprints[offset + side].Footprint.RowPitch;
      for (UINT y = 0; y < 1024; ++y)
        for (UINT x = 0; x < profile.width; ++x) {
          const auto address = SIZE_T{y} * row_pitch + 4 * x;
          const auto* pixel = on[side].data() + address;
          const bool outer = x >= left && x < left + width && y < 763;
          const bool inner = x >= left + 16 && x < left + width - 16 && y >= 12 && y < 763;
          if (!outer)
            require(std::memcmp(pixel, off[side].data() + address, 4) == 0,
                    "Profile switch preserves every ND, gutter and lower-trim byte");
          else if (!inner)
            require(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255, "Current-profile border stays opaque black");
          else {
            const auto wx = static_cast<UINT>((x - left - 16 + .5) * 768 / (width - 32));
            const auto wy = static_cast<UINT>((y - 12 + .5) * 763 / 751);
            if (wx >= 350 && wx < 400 && ((wy >= 100 && wy < 150) || (wy >= 400 && wy < 450))) {
              const bool nose = wy < 200;
              const UINT blue = phase & 1 ? (nose ? 153 : 102) : (nose ? 51 : 204);
              require(pixel[2] >= blue - 2 && pixel[2] <= blue + 2, "Current-profile panes contain the fresh phase's GPU pixel pattern");
              ++(nose ? nose_pixels : tail_pixels);
            }
            if (wy >= 251 && wy < 263)
              require(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0, "Profile switch keeps the exact black divider");
          }
          ++checked_pixels;
        }
    }
    require(nose_pixels > 1000 && tail_pixels > 1000, "Both fresh camera panes verified over broad regions");
    std::printf("Profile phase%u id%u: retained736x251/496, fresh nose+tail, routes rebound, frames=%llu stamps=%llu checked=%llu\n", phase,
                profile.id, runtime::snapshot(key).frames, runtime::snapshot(key).stamps, checked_pixels);
    old_routes = current_routes;
    previous_match = match;
  }
  win::set_target_mask(0);
  runtime::manager().stop_source_tracking();
  handoff.stop_scene();
  runtime::reset_feed(key);
  check(list->Close(), "Profile switch final Close");
  require(win::graphics_status().hook_failures == 0, "Profile switch has no native hook failures");
  UINT64 errors = 0;
  if (messages.get())
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
      SIZE_T size{};
      messages->GetMessage(i, nullptr, &size);
      std::vector<unsigned char> bytes(size);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
      if (SUCCEEDED(messages->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
        ++errors;
        std::fprintf(stderr, "D3D12 error%u: %s\n", message->ID, message->pDescription);
      }
    }
  require(errors == 0, "Profile switch GPU debug validation");
  std::printf(
      "PASS active profile switch %s: A380->A350->A380, retained sources, fresh pair per phase, exact PFD/ND/trim pixels=%llu debug=%d "
      "errors=%llu\n",
      warp ? "WARP" : "hardware", checked_pixels, debug_enabled, errors);
}

// Exercise final-Draw fallback on a fresh ordinary recording after two native
// alpha-blended gray texture draws with every binding left untouched. Both typed
// views of a typeless SRV/RTV and all mip levels carry non-endpoint values.
void textured_gray_fallback(ID3D12Device* device,
                            ID3D12CommandQueue* queue,
                            ID3D12CommandAllocator* allocator,
                            ID3D12GraphicsCommandList* list,
                            std::uint64_t key,
                            std::uint64_t other_id,
                            bool warp) {
  using namespace taxi_camera;
  const auto& profile = profiles::A359;
  win::set_target_mask(0);
  win::set_aircraft_profile(profile.id);
  Reference<ID3D12Resource> target, sampled, tint, readback;
  auto target_desc = texture_description(profile.width, profile.height, DXGI_FORMAT_R8G8B8A8_TYPELESS);
  target_desc.MipLevels = 4;
  create_texture(device, target_desc, target.put());
  auto source_desc = texture_description(16, 16, DXGI_FORMAT_R8G8B8A8_TYPELESS);
  source_desc.MipLevels = 4;
  create_texture(device, source_desc, sampled.put());
  std::uint64_t target_id = 0;
  for (const auto& c : win::pfd_inventory())
    if (c.levels == 4)
      target_id = c.id;
  require(target_id && win::assign_targets(target_id, other_id), "Select typeless multi-mip gray fixture PFD");
  Reference<ID3D12DescriptorHeap> rtvs, srvs, samplers;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 12, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(rtvs.put())), "Gray RTV heap");
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(srvs.put())), "Gray SRV heap");
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(samplers.put())), "Gray sampler heap");
  const auto rtv_base = rtvs->GetCPUDescriptorHandleForHeapStart();
  const auto rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  const auto rtv = [&](UINT index) { return D3D12_CPU_DESCRIPTOR_HANDLE{rtv_base.ptr + SIZE_T{index} * rtv_stride}; };
  const std::array<DXGI_FORMAT, 2> formats{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
  for (UINT f = 0; f < 2; ++f) {
    D3D12_RENDER_TARGET_VIEW_DESC d{};
    d.Format = formats[f];
    d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(target.get(), &d, rtv(f));
    D3D12_SHADER_RESOURCE_VIEW_DESC s{};
    s.Format = formats[f];
    s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    s.Texture2D.MipLevels = 4;
    auto handle = srvs->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T{f} * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device->CreateShaderResourceView(sampled.get(), &s, handle);
  }
  const std::array<float, 4> grays{.18f, .35f, .5f, .7f};
  for (UINT mip = 0; mip < 4; ++mip) {
    D3D12_RENDER_TARGET_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipSlice = mip;
    device->CreateRenderTargetView(sampled.get(), &d, rtv(4 + mip));
    const float gray[]{grays[mip], grays[mip], grays[mip], .65f};
    list->ClearRenderTargetView(rtv(4 + mip), gray, 0, nullptr);
    const UINT width = 16u >> mip;
    const D3D12_RECT half{0, 0, static_cast<LONG>(std::max(1u, width / 2)), static_cast<LONG>(width)};
    const float lighter[]{grays[mip] + .1f, grays[mip] + .1f, grays[mip] + .1f, .4f};
    list->ClearRenderTargetView(rtv(4 + mip), lighter, 1, &half);
    device->CreateRenderTargetView(target.get(), &d, rtv(8 + mip));
    const float sentinel[]{.13f, .17f, .21f, .9f};
    list->ClearRenderTargetView(rtv(8 + mip), sentinel, 0, nullptr);
  }
  D3D12_RESOURCE_BARRIER sample_barrier{};
  sample_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  sample_barrier.Transition = {sampled.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
  list->ResourceBarrier(1, &sample_barrier);
  D3D12_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  device->CreateSampler(&sampler, samplers->GetCPUDescriptorHandleForHeapStart());
  auto buffer_desc = texture_description(1, 1, DXGI_FORMAT_UNKNOWN);
  buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer_desc.Width = 256;
  buffer_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const auto upload = heap_properties(D3D12_HEAP_TYPE_UPLOAD), read_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
  check(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                        IID_PPV_ARGS(tint.put())),
        "Gray tint CBV");
  void* mapped{};
  const D3D12_RANGE none{0, 0};
  check(tint->Map(0, &none, &mapped), "Gray tint map");
  const float tint_value[]{.8f, .8f, .8f, .75f};
  std::memcpy(mapped, tint_value, sizeof(tint_value));
  tint->Unmap(0, nullptr);
  setup_trace::verify(device, tint->GetGPUVirtualAddress());
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 4> footprints{};
  UINT64 bytes{};
  device->GetCopyableFootprints(&target_desc, 0, 4, 0, footprints.data(), nullptr, nullptr, &bytes);
  buffer_desc.Width = bytes;
  check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(readback.put())),
        "Gray pixels readback");
  const auto submit = [&](bool replay = false) {
    check(list->Close(), "Close gray recording");
    ID3D12CommandList* lists[]{list};
    queue->ExecuteCommandLists(1, lists);
    require(drain_copy_queue(queue, device), "Gray GPU completion");
    if (replay) {
      const auto recorded_stamps = runtime::snapshot(key).stamps;
      const auto submissions = runtime::snapshot(key).capture.submissions;
      queue->ExecuteCommandLists(1, lists);
      require(drain_copy_queue(queue, device), "Closed final-Draw recording reexecutes with its output buffer alive");
      require(runtime::snapshot(key).stamps == recorded_stamps && runtime::snapshot(key).capture.submissions > submissions,
              "Reexecution retains consumer synchronization without recording another camera draw");
    }
    check(allocator->Reset(), "Reset gray allocator");
    check(list->Reset(allocator, nullptr), "Fresh ordinary gray recording");
  };
  submit();  // Source preparation cannot supply tested PFD RT-entry evidence.
  std::uint64_t checked = 0, fallback_count = 0;
  std::array<double, 16> means{};
  unsigned case_index = 0;
  for (UINT output_format = 0; output_format < 2; ++output_format) {
    ClearStatePipeline pipeline(device, true, formats[output_format], DXGI_FORMAT_UNKNOWN, true);
    for (UINT input_format = 0; input_format < 2; ++input_format) {
      for (UINT mip = 0; mip < 4; ++mip) {
        std::vector<unsigned char> baseline;
        for (UINT on = 0; on < 2; ++on) {
          const bool draw = on != 0;
          win::set_target_mask(on ? 1 : 0);
          const auto before = win::graphics_status();
          const float background[]{.15f, .15f, .15f, 1};
          list->ClearRenderTargetView(rtv(output_format), background, 0, nullptr);
          list->SetPipelineState(pipeline.pipeline.get());
          list->SetGraphicsRootSignature(pipeline.root.get());
          ID3D12DescriptorHeap* heaps[]{srvs.get(), samplers.get()};
          list->SetDescriptorHeaps(2, heaps);
          auto gpu = srvs->GetGPUDescriptorHandleForHeapStart();
          gpu.ptr += UINT64{input_format} * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
          list->SetGraphicsRootDescriptorTable(1, gpu);
          list->SetGraphicsRootDescriptorTable(2, samplers->GetGPUDescriptorHandleForHeapStart());
          list->SetGraphicsRootConstantBufferView(3, tint->GetGPUVirtualAddress());
          const UINT parameters[]{64, 64, 1, mip};
          list->SetGraphicsRoot32BitConstants(0, 4, parameters, 0);
          list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          // Native trim quad is outside the camera patch, with alpha accumulating
          // twice. Camera delivery must wait until both draws are recorded.
          const D3D12_VIEWPORT viewport{812, 800, 64, 64, 0, 1};
          const D3D12_RECT scissor{812, 800, 876, 864};
          list->RSSetViewports(1, &viewport);
          list->RSSetScissorRects(1, &scissor);
          const auto target_rtv = rtv(output_format);
          list->OMSetRenderTargets(1, &target_rtv, FALSE, nullptr);
          list->DrawInstanced(3, 1, 0, 0);
          list->OMSetRenderTargets(1, &target_rtv, FALSE, nullptr);  // Stage the target without changing shader state.
          const auto delivered = win::graphics_status();
          require(delivered.fallback_stamps == before.fallback_stamps && delivered.recording_end_draws == before.recording_end_draws &&
                      delivered.preferred_copy_stamps == before.preferred_copy_stamps,
                  "OM stages the target without a shader draw or an unproven private copy");
          list->DrawInstanced(3, 1, 0, 0);  // No application state rebind at all.
          require(win::graphics_status().fallback_stamps == before.fallback_stamps,
                  "Production camera draw cannot precede the final unrebound native gray draw");
          win::set_target_mask(draw ? 1 : 0);
          submit(draw);
          const auto closed = win::graphics_status();
          require(closed.fallback_stamps == before.fallback_stamps + draw &&
                      closed.recording_end_draws == before.recording_end_draws + draw &&
                      closed.preferred_copy_stamps == before.preferred_copy_stamps &&
                      closed.close_forward_refused == before.close_forward_refused,
                  "Close records exactly one camera draw; no private copy without transition evidence");
          win::set_target_mask(0);
          fallback_count += draw;
          for (UINT level = 0; level < 4; ++level) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {target.get(), level, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
            list->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = target.get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = level;
            destination.pResource = readback.get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = footprints[level];
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(1, &barrier);
          }
          submit();  // Next OFF/ON also has no RT-entry evidence in its recording.
          const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
          check(readback->Map(0, &range, &mapped), "Read gray native pixels");
          const auto* actual = static_cast<const unsigned char*>(mapped);
          if (!on)
            baseline.assign(actual, actual + bytes);
          double sum = 0;
          std::uint64_t changed_patch = 0;
          for (UINT level = 0; level < 4; ++level) {
            const auto& fp = footprints[level];
            for (UINT y = 0; y < fp.Footprint.Height; ++y)
              for (UINT x = 0; x < fp.Footprint.Width; ++x) {
                const SIZE_T offset = fp.Offset + SIZE_T{y} * fp.Footprint.RowPitch + 4 * x;
                const bool inside = !level && x < 806 && y < 763;
                if (on && !inside) {
                  require(std::memcmp(actual + offset, baseline.data() + offset, 4) == 0,
                          "Native textured gray/alpha pixels, gutter, trim or lower mip changed after shader fallback");
                  ++checked;
                }
                if (on && inside && std::memcmp(actual + offset, baseline.data() + offset, 4))
                  ++changed_patch;
                if (!level && x >= 812 && x < 876 && y >= 800 && y < 864)
                  sum += actual[offset];
              }
          }
          if (draw)
            require(changed_patch > 1000, "The final camera draw must survive closed-list reexecution and visibly write the patch");
          else
            means[case_index] = sum / 4096.;
          readback->Unmap(0, &none);
          std::printf("Gray case RTV=%s SRV=%s mip=%u enabled=%u mean=%.6f fallback+%u\n", output_format ? "sRGB" : "UNORM",
                      input_format ? "sRGB" : "UNORM", mip, on, sum / 4096., static_cast<unsigned>(draw));
        }
        ++case_index;
      }
    }
  }
  for (unsigned output = 0; output < 2; ++output) {
    require(means[output * 8] > means[output * 8 + 4] + 5, "sRGB SRV control must measurably change sampled gray");
    for (unsigned input = 0; input < 2; ++input)
      require(means[output * 8 + input * 4 + 3] > means[output * 8 + input * 4] + 15, "Mip control must measurably change sampled gray");
  }
  require(means[8] > means[0] + 15, "sRGB RTV control must measurably change encoded gray");
  check(device->GetDeviceRemovedReason(), "Gray fixture device health");
  Reference<ID3D12InfoQueue> messages;
  const bool debug_messages = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(messages.put())));
  unsigned debug_errors = 0;
  if (debug_messages)
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
      SIZE_T size{};
      messages->GetMessage(i, nullptr, &size);
      std::vector<unsigned char> storage(size);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      if (SUCCEEDED(messages->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
        ++debug_errors;
        std::fprintf(stderr, "Gray GPU error %u: %s\n", static_cast<unsigned>(message->ID), message->pDescription);
      }
    }
  require(!debug_errors, "Gray fixture GPU debug errors");
  std::printf("Gray GPU debug messages available=%u errors=%u\n", debug_messages, debug_errors);
  check(list->Close(), "Finish gray fixture recording");
  runtime::manager().stop_source_tracking();
  scene_handoff().stop_scene();
  runtime::reset_feed(key);
  std::printf("PASS textured gray %s: 16 UNORM/sRGB/mip/alpha cases; fallback=%llu preferredcopy=0; preserved pixels=%llu\n",
              warp ? "WARP" : "hardware", fallback_count, checked);
}
void native_case(bool warp, bool a350, bool query_fallback, bool prefer_copy, bool textured_gray = false) {
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
  const auto successful_copies = [] {
    const auto s = win::graphics_status();
    return s.copy_attempts - s.copy_rejected;
  };
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
  win::set_aircraft_profile(profile.id);
  require(win::target_ids() == std::array<std::uint64_t, 2>{}, "Same-aircraft session clears old display bindings");
  require(win::pfd_inventory().size() == 2, "Same-aircraft session preserves live resource incarnations");
  require(win::assign_targets(first, second), "Same-aircraft session reacquires existing displays");
  win::set_aircraft_profile(a350 ? taxi_camera::profiles::A380.id : taxi_camera::profiles::A359.id);
  require(win::target_ids() == std::array<std::uint64_t, 2>{} && win::pfd_inventory().empty(),
          "Other aircraft profile releases bindings and rejects previous display dimensions");
  require(!win::assign_targets(first, second), "Previous aircraft display IDs cannot bind the other profile");
  win::set_aircraft_profile(profile.id);
  require(win::target_ids() == std::array<std::uint64_t, 2>{} && win::pfd_inventory().size() == 2,
          "Profile round trip returns to unbound eligible displays");
  require(win::assign_targets(first, second), "Profile round trip reacquires existing displays");

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
  require(!capture.patch_requests && !capture.patch_draws, "Close output preparation records no unsolicited typed patches");
  if (textured_gray) {
    textured_gray_fallback(device.get(), queue.get(), allocator.get(), list.get(), key, second, warp);
    return;
  }
  if (!query_fallback) {
    // A cold, proven copy opportunity requests metadata only. Its pending
    // target must still receive normal terminal delivery at native Close.
    // Establish COMMON before the tested recording. Two transitions before
    // its first Draw are deliberately ambiguous to PfdCopyProof.
    win::set_target_mask(0);
    transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    submit();
    reset();
    win::set_target_mask(1);
    transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    generator.record(list.get(), rtvs[2], display_width, 1024, false, 0, 0);
    const auto cold = win::graphics_status();
    const auto before_cold = runtime::snapshot(key);
    list->OMSetRenderTargets(1, &rtvs[3], FALSE, nullptr);
    const auto requested = runtime::snapshot(key);
    require(requested.patch_requests == 1 && !requested.patch_draws && requested.stamps == before_cold.stamps,
            "Cold native copy request adds one exact patch slot without drawing or copying");
    submit();
    const auto delivered = win::graphics_status();
    require(delivered.preferred_copy_stamps == cold.preferred_copy_stamps &&
                delivered.recording_end_draws == cold.recording_end_draws + 1 && runtime::snapshot(key).stamps == before_cold.stamps + 1,
            "Absent private patch preserves terminal Close fallback");
    win::set_target_mask(0);
    reset();
  }
  const auto delivery_baseline = runtime::snapshot(key).stamps;
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
  require(runtime::snapshot(key).patch_requests == (query_fallback ? 0u : 1u) &&
              runtime::snapshot(key).patch_draws == (query_fallback ? 0u : runtime::snapshot(key).frames - frames_before_interop),
          "Each new composition generates only the requested format; Close-only output generates none");
  if (prefer_copy) {
    Reference<ID3D12Resource> other;
    create_texture(device.get(), texture_description(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM), other.put());
    const D3D12_CPU_DESCRIPTOR_HANDLE other_rtv{base.ptr + 6 * stride};
    device->CreateRenderTargetView(other.get(), nullptr, other_rtv);
    Reference<ID3D12QueryHeap> occlusion, statistics;
    const D3D12_QUERY_HEAP_DESC oh{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 2, 0}, sh{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 2, 0};
    check(device->CreateQueryHeap(&oh, IID_PPV_ARGS(occlusion.put())), "Preferred-copy occlusion queries");
    check(device->CreateQueryHeap(&sh, IID_PPV_ARGS(statistics.put())), "Preferred-copy pipeline queries");
    const auto read_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
    auto buffer = texture_description(1, 1, DXGI_FORMAT_UNKNOWN);
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 512;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    Reference<ID3D12Resource> query_results, pixels, other_pixels;
    check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(query_results.put())),
          "Preferred-copy query readback");
    const auto desc = textures[2]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    buffer.Width = bytes;
    check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(pixels.put())),
          "Preferred-copy PFD readback");
    buffer.Width = 256 * 64;
    check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(other_pixels.put())),
          "Preferred-copy state readback");
    // Put both targets in COMMON in a separate completed recording. Only the
    // explicit entry in the tested recording may prove a preferred copy.
    for (UINT side = 0; side < 2; ++side)
      transition(list.get(), textures[side + 2].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    submit();
    reset();
    std::uint64_t checked{};
    for (bool proof : {true, false}) {
      for (bool at_close : {false, true}) {
        const UINT side = at_close ? 1 : 0;
        auto* target = textures[side + 2].get();
        std::vector<unsigned char> off_image, off_other;
        for (UINT on = 0; on < 2; ++on) {
          if (!proof) {
            // Exact same real RT state, but established on an earlier recording:
            // this must retain the working Draw fallback, never borrow proof.
            transition(list.get(), target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            submit();
            reset();
          }
          win::set_target_mask(on ? 1u << side : 0);
          const auto before = win::graphics_status();
          const auto stamps_before = runtime::snapshot(key).stamps;
          list->BeginQuery(occlusion.get(), D3D12_QUERY_TYPE_OCCLUSION, on);
          list->BeginQuery(statistics.get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, on);
          if (proof)
            transition(list.get(), target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
          generator.record(list.get(), rtvs[side + 2], display_width, 1024, false, 0, 0);
          if (!at_close) {
            list->OMSetRenderTargets(1, &other_rtv, FALSE, nullptr);
            require(runtime::snapshot(key).stamps == stamps_before + (proof ? on : 0),
                    "OM uses proven copy inside paired queries; missing proof defers Draw");
            list->DrawInstanced(3, 1, 0, 0);  // Native state must survive the preferred copy without any app rebind.
          }
          list->EndQuery(occlusion.get(), D3D12_QUERY_TYPE_OCCLUSION, on);
          require(runtime::snapshot(key).stamps == stamps_before + (!at_close && proof ? on : 0),
                  "Open pipeline query still excludes the Draw fallback");
          list->EndQuery(statistics.get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, on);
          require(runtime::snapshot(key).stamps == stamps_before + (!at_close && proof ? on : 0),
                  "Final EndQuery never issues a production Draw fallback before the application's last draw");
          if (!at_close)
            list->DrawInstanced(3, 1, 0, 0);  // No-proof production leaves native bindings untouched until Close.
          list->ResolveQueryData(occlusion.get(), D3D12_QUERY_TYPE_OCCLUSION, on, 1, query_results.get(), on * sizeof(UINT64));
          list->ResolveQueryData(statistics.get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, on, 1, query_results.get(),
                                 64 + on * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
          submit();  // Close is the second tested existing delivery boundary.
          const auto after = win::graphics_status();
          require(runtime::snapshot(key).stamps == stamps_before + on, "Exactly one preferred or fallback image per selected boundary");
          require(after.preferred_copy_stamps == before.preferred_copy_stamps + (proof ? on : 0),
                  "Per-recording explicit RT entry chooses the private-copy route");
          require(after.fallback_stamps == before.fallback_stamps + (proof ? 0 : on),
                  "Missing per-recording proof delivers only the final Close Draw fallback");
          require(after.recording_end_draws == before.recording_end_draws + (proof ? 0 : on),
                  "Every no-proof production camera draw occurs at Close");
          std::printf("Boundary delivery %s proof=%u enabled=%u: copies+%llu fallback+%llu\n", at_close ? "Close" : "OM", proof, on,
                      after.preferred_copy_stamps - before.preferred_copy_stamps, after.fallback_stamps - before.fallback_stamps);
          win::set_target_mask(0);
          reset();
          transition(list.get(), target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
          D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
          src.pResource = target;
          src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          dst.pResource = pixels.get();
          dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          dst.PlacedFootprint = footprint;
          list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
          transition(list.get(), target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
          if (!at_close) {
            transition(list.get(), other.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            src.pResource = other.get();
            dst.pResource = other_pixels.get();
            dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256}};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            transition(list.get(), other.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
          }
          submit();
          reset();
          void* data{};
          D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)}, none{};
          check(pixels->Map(0, &range, &data), "Preferred-copy PFD pixels");
          const auto* image = static_cast<unsigned char*>(data);
          if (!on)
            off_image.assign(image, image + bytes);
          else {
            const UINT left = a350 && side ? 838 : 0, width = a350 ? 806 : 768;
            for (UINT y = 0; y < 1024; ++y)
              for (UINT x = 0; x < display_width; ++x) {
                const auto* pixel = image + SIZE_T{y} * footprint.Footprint.RowPitch + 4 * x;
                if (x >= left && x < left + width && y < 763) {
                  if (x < left + 16 || x >= left + width - 16 || y < 12)
                    require(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0, "Preferred-copy exact black border");
                } else
                  require(std::memcmp(pixel, off_image.data() + SIZE_T{y} * footprint.Footprint.RowPitch + 4 * x, 4) == 0,
                          "Preferred-copy preserves exact OFF bytes in ND, opposite atlas half, gutter and trim");
                ++checked;
              }
            const auto* nose = image + SIZE_T{150} * footprint.Footprint.RowPitch + 4 * (left + width / 2);
            const auto* tail = image + SIZE_T{650} * footprint.Footprint.RowPitch + 4 * (left + width / 2);
            require(nose[2] >= 49 && nose[2] <= 53 && tail[2] >= 202 && tail[2] <= 206,
                    "Preferred-copy and no-proof fallback both contain real composed nose and tail images");
          }
          pixels->Unmap(0, &none);
          if (!at_close) {
            range = {0, 256 * 64};
            check(other_pixels->Map(0, &range, &data), "Preferred-copy native next-draw pixels");
            const auto* image = static_cast<unsigned char*>(data);
            if (!on)
              off_other.assign(image, image + 256 * 64);
            else
              require(std::memcmp(image, off_other.data(), off_other.size()) == 0,
                      "Native draws after delivery preserve exact OFF graphics-state pixels without rebinding");
            other_pixels->Unmap(0, &none);
          }
        }
        void* data{};
        const D3D12_RANGE range{0, 512}, none{};
        check(query_results->Map(0, &range, &data), "Preferred-copy query equality");
        std::array<UINT64, 2> samples{};
        std::memcpy(samples.data(), data, sizeof(samples));
        require(samples[0] == UINT64{display_width} * 1024 + (at_close ? 0 : 4096) && samples[1] == samples[0],
                "Preferred-copy and fallback preserve native occlusion results OFF versus ON");
        D3D12_QUERY_DATA_PIPELINE_STATISTICS off{}, on{};
        std::memcpy(&off, static_cast<unsigned char*>(data) + 64, sizeof(off));
        std::memcpy(&on, static_cast<unsigned char*>(data) + 64 + sizeof(off), sizeof(on));
        require(std::memcmp(&off, &on, sizeof(off)) == 0 && off.IAVertices == (at_close ? 3u : 6u),
                "Preferred-copy and fallback preserve every native pipeline-statistics field");
        query_results->Unmap(0, &none);
      }
    }
    list->Close();
    require(win::graphics_status().hook_failures == 0, "Preferred-copy native hook failures");
    if (messages.get())
      for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
        SIZE_T size{};
        messages->GetMessage(i, nullptr, &size);
        std::vector<unsigned char> memory(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(memory.data());
        if (SUCCEEDED(messages->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
          std::fprintf(stderr, "D3D12 preferred-copy error %u: %s\n", static_cast<unsigned>(message->ID), message->pDescription);
          require(false, "Preferred-copy D3D12 validation error");
        }
      }
    std::printf(
        "PASS preferred copy %s %s: explicit legacy entry, OM/Close, missing-proof fallback, exact OFF pixels, paired-query equality; %llu "
        "pixels\n",
        a350 ? "A350" : "A380", warp ? "WARP" : "hardware", checked);
    return;
  }
  if (query_fallback) {
    Reference<ID3D12Resource> other_a, other_b, depth_a, depth_b;
    Reference<ID3D12DescriptorHeap> raw_rtvs, raw_dsvs;
    D3D12_CPU_DESCRIPTOR_HANDLE raw_rtv{}, raw_dsv{};
    {
      const win::OwnedWork unobserved;
      create_texture(device.get(), texture_description(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM), other_a.put());
      create_texture(device.get(), texture_description(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM), other_b.put());
      auto depth = texture_description(64, 64, DXGI_FORMAT_D32_FLOAT);
      depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
      const auto heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
      check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
                                            IID_PPV_ARGS(depth_a.put())),
            "Depth A");
      check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
                                            IID_PPV_ARGS(depth_b.put())),
            "Depth B");
      D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
      check(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(raw_rtvs.put())), "Untracked RTV heap");
      h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
      check(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(raw_dsvs.put())), "Untracked DSV heap");
      raw_rtv = raw_rtvs->GetCPUDescriptorHandleForHeapStart();
      raw_dsv = raw_dsvs->GetCPUDescriptorHandleForHeapStart();
    }
    ClearStatePipeline green(device.get(), false, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT);
    Reference<ID3D12QueryHeap> occlusion, statistics;
    const D3D12_QUERY_HEAP_DESC oh{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 4, 0}, sh{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 2, 0};
    check(device->CreateQueryHeap(&oh, IID_PPV_ARGS(occlusion.put())), "Overlapping occlusion queries");
    check(device->CreateQueryHeap(&sh, IID_PPV_ARGS(statistics.put())), "Overlapping pipeline query");
    const auto read_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 512;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Reference<ID3D12Resource> query_results, pixels, other_pixels;
    check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(query_results.put())),
          "Query results");
    const auto desc = textures[2]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    buffer.Width = bytes;
    check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(pixels.put())),
          "Query PFD pixels");
    buffer.Width = 2 * 256 * 64;
    check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(other_pixels.put())),
          "Restored target pixels");
    std::uint64_t checked = 0;
    std::vector<unsigned char> off_image;
    for (UINT on = 0; on < 2; ++on) {
      win::set_target_mask(on ? 1 : 0);
      {
        const win::OwnedWork unobserved;
        device->CreateRenderTargetView(other_a.get(), nullptr, raw_rtv);
        device->CreateRenderTargetView(other_b.get(), nullptr, {raw_rtv.ptr + stride});
        device->CreateDepthStencilView(depth_a.get(), nullptr, raw_dsv);
        device->CreateDepthStencilView(depth_b.get(), nullptr,
                                       {raw_dsv.ptr + device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV)});
      }
      const float red[]{1, 0, 0, 1};
      list->ClearRenderTargetView(raw_rtv, red, 0, nullptr);
      list->ClearRenderTargetView({raw_rtv.ptr + stride}, red, 0, nullptr);
      list->ClearDepthStencilView(raw_dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
      list->ClearDepthStencilView({raw_dsv.ptr + device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV)},
                                  D3D12_CLEAR_FLAG_DEPTH, 0, 0, 0, nullptr);
      const auto before = runtime::snapshot(key).stamps;
      const auto before_end_draws = win::graphics_status().recording_end_draws;
      list->BeginQuery(occlusion.get(), D3D12_QUERY_TYPE_OCCLUSION, on * 2);
      list->BeginQuery(occlusion.get(), D3D12_QUERY_TYPE_OCCLUSION, on * 2 + 1);
      list->BeginQuery(statistics.get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, on);
      generator.record(list.get(), rtvs[2], display_width, 1024, false, 0, 0);
      list->OMSetRenderTargets(1, &raw_rtv, FALSE, &raw_dsv);
      list->SetPipelineState(green.pipeline.get());
      list->SetGraphicsRootSignature(green.root.get());
      const UINT values[]{64, 64, 0, 0};
      list->SetGraphicsRoot32BitConstants(0, 4, values, 0);
      const D3D12_VIEWPORT vp{0, 0, 64, 64, 0, 1};
      const D3D12_RECT scissor{0, 0, 64, 64};
      list->RSSetViewports(1, &vp);
      list->RSSetScissorRects(1, &scissor);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      // Legal immediate descriptor reuse. Native OM captured A; staging the
      // camera must not bind newly written B through these old handles.
      {
        const win::OwnedWork unobserved;
        device->CreateRenderTargetView(other_b.get(), nullptr, raw_rtv);
        device->CreateDepthStencilView(depth_b.get(), nullptr, raw_dsv);
      }
      if (on) {
        raw_rtvs.get()->Release();
        *raw_rtvs.put() = nullptr;
        raw_dsvs.get()->Release();
        *raw_dsvs.put() = nullptr;
      }  // Legal CPU descriptor heap retirement after OM.
      list->EndQuery(occlusion.get(), D3D12_QUERY_TYPE_OCCLUSION, on * 2 + 1);
      require(runtime::snapshot(key).stamps == before, "Inner EndQuery cannot drain while other queries remain");
      list->EndQuery(occlusion.get(), D3D12_QUERY_TYPE_OCCLUSION, on * 2);
      require(runtime::snapshot(key).stamps == before, "Occlusion End cannot drain active pipeline-statistics query");
      list->EndQuery(statistics.get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, on);
      std::printf("Query-aware PFD delivery enabled=%u: %llu -> %llu\n", on, before, runtime::snapshot(key).stamps);
      require(runtime::snapshot(key).stamps == before, "Final EndQuery retains the PFD image until native work finishes at Close");
      list->ResolveQueryData(occlusion.get(), D3D12_QUERY_TYPE_OCCLUSION, on * 2, 2, query_results.get(), on * 16);
      list->ResolveQueryData(statistics.get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, on, 1, query_results.get(),
                             64 + on * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
      list->DrawInstanced(3, 1, 0, 0);  // No OM/PSO/root/viewport rebind; A and its DSV must survive.
      submit();
      require(runtime::snapshot(key).stamps == before + on, "Close delivers exactly one deferred PFD image after native query work");
      require(win::graphics_status().recording_end_draws == before_end_draws + on,
              "Deferred-query camera delivery uses only the final no-restore draw");
      reset();
      // Separate transition-only recording reproduces the live rejected copy route.
      const auto before_exit = runtime::snapshot(key).stamps;
      transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      require(runtime::snapshot(key).stamps == before_exit, "Empty exit list remains ineligible for private copy");
      D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
      src.pResource = textures[2].get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.pResource = pixels.get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = footprint;
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      for (UINT i = 0; i < 2; ++i) {
        auto* target = i ? other_b.get() : other_a.get();
        transition(list.get(), target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        src.pResource = target;
        dst.pResource = other_pixels.get();
        dst.PlacedFootprint = {UINT64{i} * 256 * 64, {DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256}};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(list.get(), target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      }
      submit();
      reset();
      void* data{};
      D3D12_RANGE range{0, 2 * 256 * 64}, none{};
      check(other_pixels->Map(0, &range, &data), "Restored OM pixels");
      for (UINT i = 0; i < 2; ++i)
        for (UINT n = 0; n < 64 * 64; ++n) {
          const auto* pixel = static_cast<unsigned char*>(data) + i * 256 * 64 + n * 4;
          require(pixel[0] == (i ? 255 : 0) && pixel[1] == (i ? 0 : 255) && pixel[2] == 0 && pixel[3] == 255,
                  "Current untracked RTV+DSV contents preserved through native work despite handle reuse");
          ++checked;
        }
      other_pixels->Unmap(0, &none);
      range = {0, static_cast<SIZE_T>(bytes)};
      check(pixels->Map(0, &range, &data), "Deferred PFD pixels");
      if (!on)
        off_image.assign(static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + bytes);
      if (on) {
        const auto* image = static_cast<unsigned char*>(data);
        const UINT width = a350 ? 806 : 768;
        for (UINT y = 0; y < 1024; ++y)
          for (UINT x = 0; x < display_width; ++x) {
            const auto* pixel = image + SIZE_T{y} * footprint.Footprint.RowPitch + 4 * x;
            if (x < width && y < 763) {
              if (x < 16 || x >= width - 16 || y < 12)
                require(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0, "Deferred camera patch black border");
            } else {
              require(std::memcmp(pixel, off_image.data() + SIZE_T{y} * footprint.Footprint.RowPitch + 4 * x, 4) == 0,
                      "Deferred camera preserves exact OFF pixels outside rectangle (ND/gutter/trim)");
            }
            ++checked;
          }
        const auto* nose = image + SIZE_T{150} * footprint.Footprint.RowPitch + 4 * (width / 2);
        require(nose[2] >= 49 && nose[2] <= 53, "Deferred PFD contains actual composed nose pixels");
        const auto* tail = image + SIZE_T{650} * footprint.Footprint.RowPitch + 4 * (width / 2);
        require(tail[2] >= 202 && tail[2] <= 206, "Deferred PFD contains actual composed tail pixels");
      }
      pixels->Unmap(0, &none);
    }
    void* data{};
    const D3D12_RANGE range{0, 512}, none{};
    check(query_results->Map(0, &range, &data), "Compare native query results");
    std::array<UINT64, 4> samples{};
    std::memcpy(samples.data(), data, 32);
    require(samples[0] == UINT64{display_width} * 1024 && samples[1] == samples[0] && samples[2] == samples[0] && samples[3] == samples[0],
            "Overlapping occlusion queries unchanged OFF versus ON");
    D3D12_QUERY_DATA_PIPELINE_STATISTICS off{}, on{};
    std::memcpy(&off, static_cast<unsigned char*>(data) + 64, sizeof(off));
    std::memcpy(&on, static_cast<unsigned char*>(data) + 64 + sizeof(off), sizeof(on));
    require(std::memcmp(&off, &on, sizeof(off)) == 0 && off.IAVertices == 3, "Pipeline statistics unchanged OFF versus ON");
    query_results->Unmap(0, &none);
    std::printf(
        "PASS query fallback %s: overlap blocked, final End staged, Close delivered, independent exit refused, raw RTV+DSV reuse "
        "preserved; "
        "query OFF=ON=%llu; %llu pixels\n",
        warp ? "WARP" : "hardware", samples[0], checked);
    win::set_target_mask(0);
    list->Close();
    return;
  }
  win::set_target_mask(3);
  Reference<ID3D12QueryHeap> private_copy_guard;
  const D3D12_QUERY_HEAP_DESC private_guard_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 1, 0};
  check(device->CreateQueryHeap(&private_guard_desc, IID_PPV_ARGS(private_copy_guard.put())), "Private-copy-only fixture query");
  list->BeginQuery(private_copy_guard.get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
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
  require(runtime::snapshot(key).stamps == delivery_baseline, "Active query defers fallback; repeated glyph draws do not stamp");
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
      const auto before = successful_copies();
      list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
      std::printf("7105-barrier selected PFD copies: %llu -> %llu\n", before, successful_copies());
      require(successful_copies() == before + 1, "Selected PFD near end of7105barriers receives exactly one copy");
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
  list->EndQuery(private_copy_guard.get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
  submit();
  require(successful_copies() == 2 && runtime::snapshot(key).stamps == delivery_baseline + 2,
          "One private patch per PFD: legacy and enhanced RT-exit boundaries");
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
                                     (working_x == 40 && working_y == 4) || (working_x == 136 && working_y == 30) ||
                                     (working_x == 40 && working_y == 52);
          if (camera_margin)
            require(pixel[2] >= 50 && pixel[2] <= 52, "Camera image around inset GS panel");
          if (working_x == 20 && working_y == 16) {
            require(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0, "GS panel internal padding");
            ++gs_padding_pixels;
          }
          if (working_x >= 24 && working_x < 36 && working_y >= 20 && working_y < 40) {
            require(pixel[0] == pixel[1] && pixel[1] == pixel[2] && pixel[3] == 255,
                    "Antialiased GS label stays white on opaque black after inset scaling");
            gs_label_pixels += pixel[0] > 200;
          }
          // These pixels verify configured coordinates, not alignment to aircraft geometry.
          if (a350 && (working_x == 199 || working_x == 568) && working_y == 697) {
            require(pixel[0] > 250 && pixel[1] > 135 && pixel[1] < 145 && pixel[2] < 3,
                    "Configured A350 lower bracket corners render on both sides");
            ++tail_guide_pixels;
          }
          if (a350 && ((working_x == 234 && working_y == 637) || ((working_x == 207 || working_x == 560) && working_y == 728)))
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
  const auto cross_copies = successful_copies();
  transition(list.get(), textures[2].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  const auto cross_after = win::graphics_status();
  std::printf("Cross-recording selected PFD copies: %llu -> %llu; pending matches: %llu -> %llu\n", cross_copies, successful_copies(),
              cross_before.selected_pending_matches, cross_after.selected_pending_matches);
  require(successful_copies() == cross_copies + 1 && cross_after.selected_pending_matches == cross_before.selected_pending_matches &&
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
    const auto draw_typed = [&] {
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
    };
    draw_typed();
    const auto cold = runtime::snapshot(key);
    transition(list.get(), typed.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    require(runtime::snapshot(key).stamps == cold.stamps && runtime::snapshot(key).patch_requests == cold.patch_requests + 1 &&
                runtime::snapshot(key).patch_draws == cold.patch_draws,
            "First request for another typed format records no speculative copy");
    transition(list.get(), typed.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    submit();
    reset();
    win::set_target_mask(0);
    const auto before_refresh = runtime::snapshot(key);
    generator.record(list.get(), rtvs[0], pane_width, nose_height, false, 0, 0);
    generator.record(list.get(), rtvs[1], pane_width, tail_height, false, 0, 1);
    for (UINT feed = 0; feed < 2; ++feed) {
      transition(list.get(), textures[feed].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      transition(list.get(), textures[feed].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    submit();
    reset();
    const auto ready_by = GetTickCount64() + 1000;
    while (runtime::snapshot(key).frames == before_refresh.frames && GetTickCount64() < ready_by) {
      runtime::service();
      Sleep(1);
    }
    const auto refreshed = runtime::snapshot(key);
    require(refreshed.frames > before_refresh.frames &&
                refreshed.patch_draws - before_refresh.patch_draws == (refreshed.frames - before_refresh.frames) * refreshed.patch_requests,
            "New typed demand is generated while every prior replay slot keeps refreshing");
    win::set_target_mask(3);
    draw_typed();
    const auto before = runtime::snapshot(key).stamps;
    transition(list.get(), typed.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    require(runtime::snapshot(key).stamps == before + 1, "Demanded typed target receives exactly one private copy");
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
  generator.record(list.get(), rtvs[2], display_width, 1024, false, 0, 0);
  // Count the explicit API call before submission can run other observed lists.
  const auto before_clears = win::graphics_status().clear_states;
  list->ClearState(cleared.pipeline.get());
  const auto after_clear_call = win::graphics_status().clear_states;
  std::printf("ClearState explicit call: %llu -> %llu\n", before_clears, after_clear_call);
  require(after_clear_call == before_clears + 1, "Observe the explicit native ClearState once");
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
  std::printf("ClearState after submission: %llu -> %llu; pipeline pixels and pending-stamp discard passed\n", after_clear_call,
              win::graphics_status().clear_states);
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
  require(runtime::snapshot(key).stamps == before_predicate + 1,
          "Observed Reset recovers query-safe Close fallback without a guessed resource barrier");
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
  const auto stamps_before_guides = successful_copies();
  list->BeginQuery(private_copy_guard.get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
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
  list->EndQuery(private_copy_guard.get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
  submit();
  require(successful_copies() == stamps_before_guides + 2, "Both PFDs receive the adjusted private patch");
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
  // Calibration is clear-only on the actual draw recording, so it remains
  // available when its later RT exit is recorded on a separate barrier list.
  win::set_calibration(3, 4096);
  const auto calibration_before = win::graphics_status().calibration_clears;
  const auto copies_before_calibration = runtime::snapshot(key).stamps;
  const float calibration_background[]{.125f, .25f, .5f, 1};
  for (UINT side = 0; side < 2; ++side) {
    list->ClearRenderTargetView(rtvs[side + 2], calibration_background, 0, nullptr);
    list->SetPipelineState(cleared.pipeline.get());
    list->SetGraphicsRootSignature(cleared.root.get());
    const UINT constants[]{64, 64, 0, 0};
    list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
    const float origin = a350 && side ? 838.f : 0.f;
    const D3D12_VIEWPORT viewport{origin, 0, 1, 1, 0, 1};
    const D3D12_RECT scissor{static_cast<LONG>(origin), 0, static_cast<LONG>(origin) + 1, 1};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->OMSetRenderTargets(1, &rtvs[side + 2], FALSE, nullptr);
    list->DrawInstanced(3, 1, 0, 0);
    if (!side) {
      list->OMSetRenderTargets(1, &clear_rtv, FALSE, nullptr);
      require(win::graphics_status().calibration_clears == calibration_before + 1, "Calibration clears at safe OM switch without RT exit");
    }
  }
  submit();
  require(win::graphics_status().calibration_clears == calibration_before + 2, "Calibration clears at safe Close without RT exit");
  require(runtime::snapshot(key).stamps == copies_before_calibration, "Calibration does not restore application overlay Draw");
  win::set_calibration(0, 4096);
  reset();
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
  reset();
  for (UINT side = 0; side < 2; ++side) {
    void* mapped{};
    const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)}, none{};
    check(readbacks[side]->Map(0, &range, &mapped), "Calibration pixels");
    const UINT left = a350 && side ? 838u : 0u, width = a350 ? 806u : 768u;
    for (UINT y = 0; y < 1024; ++y)
      for (UINT x = 0; x < display_width; ++x) {
        const auto* pixel = static_cast<unsigned char*>(mapped) + SIZE_T{y} * footprint.Footprint.RowPitch + 4 * x;
        if (x >= left && x < left + width && y < 763) {
          const bool blue = pixel[0] >= 4 && pixel[0] <= 6 && pixel[1] >= 40 && pixel[1] <= 42 && pixel[2] >= 96 && pixel[2] <= 98;
          const bool green = pixel[0] >= 30 && pixel[0] <= 32 && pixel[1] >= 81 && pixel[1] <= 83 && pixel[2] >= 7 && pixel[2] <= 9;
          const bool cyan = pixel[0] == 0 && pixel[1] >= 229 && pixel[1] <= 230 && pixel[2] == 255;
          const bool yellow = pixel[0] == 255 && pixel[1] == 204 && pixel[2] == 0;
          const bool white = pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 255;
          require(blue || green || cyan || yellow || white, "Calibration pattern reaches selected upper rectangle");
        } else
          require(
              pixel[0] >= 31 && pixel[0] <= 32 && pixel[1] >= 63 && pixel[1] <= 64 && pixel[2] >= 127 && pixel[2] <= 128 && pixel[3] == 255,
              "Calibration leaves lower trim, gutter, and other atlas side unchanged");
        ++pixels;
      }
    readbacks[side]->Unmap(0, &none);
  }
  std::printf("Calibration OM/Close clears: %llu -> %llu; image copies unchanged\n", calibration_before,
              win::graphics_status().calibration_clears);
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
    bool warp = false, a350 = false, query_fallback = false, prefer_copy = false, profile_switch = false, textured_gray = false;
    for (int i = 1; i < argc; ++i) {
      if (std::wcscmp(argv[i], L"--warp") == 0)
        warp = true;
      else if (std::wcscmp(argv[i], L"--a350") == 0)
        a350 = true;
      else if (std::wcscmp(argv[i], L"--query-fallback") == 0)
        query_fallback = true;
      else if (std::wcscmp(argv[i], L"--textured-gray") == 0)
        textured_gray = a350 = true;
      else if (std::wcscmp(argv[i], L"--profile-switch") == 0)
        profile_switch = true;
      else if (std::wcscmp(argv[i], L"--patch-demand") == 0)
        continue;
      else if (std::wcscmp(argv[i], L"--prefer-copy") == 0)
        prefer_copy = true;
      else
        return 2;
    }
    patch_demand_case();
    if (argc == 2 && std::wcscmp(argv[1], L"--patch-demand") == 0)
      return 0;
    if (profile_switch)
      active_profile_switch_case(warp);
    else
      native_case(warp, a350, query_fallback, prefer_copy, textured_gray);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL native graphics: %s\n", e.what());
    return 1;
  }
}
