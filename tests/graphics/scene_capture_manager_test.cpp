#include "../../src/graphics/scene_capture_manager.hpp"
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include "../../src/hooks/render_boundary_observer.hpp"

namespace {
using Manager = taxi_camera::SceneCaptureManager;
namespace Boundary = taxi_camera::engine_hook::render_boundary;
unsigned checks = 0;
constexpr UINT Width = 32, Height = 16;
template <class T>
struct Ref {
  T* p = nullptr;
  ~Ref() {
    if (p)
      p->Release();
  }
  T* operator->() const { return p; }
  T** put() { return &p; }
  Ref() = default;
  Ref(const Ref&) = delete;
  Ref& operator=(const Ref&) = delete;
};
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
void check(HRESULT value, const char* message) {
  require(SUCCEEDED(value), message);
}
template <class F>
void wait(F&& test) {
  const auto start = GetTickCount64();
  while (!test()) {
    require(GetTickCount64() - start < 10000, "GPU wait timed out");
    Sleep(1);
  }
}
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES result{};
  result.Type = type;
  result.CreationNodeMask = result.VisibleNodeMask = 1;
  return result;
}
void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER value{};
  value.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  value.Transition = {resource, 0, from, to};
  list->ResourceBarrier(1, &value);
}
void enhanced_barrier(ID3D12GraphicsCommandList7* list,
                      ID3D12Resource* resource,
                      D3D12_BARRIER_LAYOUT from,
                      D3D12_BARRIER_LAYOUT to,
                      D3D12_BARRIER_ACCESS before,
                      D3D12_BARRIER_ACCESS after) {
  D3D12_TEXTURE_BARRIER value{};
  value.SyncBefore = value.SyncAfter = D3D12_BARRIER_SYNC_ALL;
  value.LayoutBefore = from;
  value.LayoutAfter = to;
  value.AccessBefore = before;
  value.AccessAfter = after;
  value.pResource = resource;
  value.Subresources.NumMipLevels = value.Subresources.NumArraySlices = value.Subresources.NumPlanes = 1;
  D3D12_BARRIER_GROUP group{};
  group.Type = D3D12_BARRIER_TYPE_TEXTURE;
  group.NumBarriers = 1;
  group.pTextureBarriers = &value;
  list->Barrier(1, &group);
}
struct Commands {
  Ref<ID3D12CommandQueue> queue;
  Ref<ID3D12CommandAllocator> allocator;
  Ref<ID3D12GraphicsCommandList> list;
  void initialize(ID3D12Device* device) {
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.put())), "Create queue");
    check(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(allocator.put())), "Create allocator");
    check(device->CreateCommandList(0, desc.Type, allocator.p, nullptr, IID_PPV_ARGS(list.put())), "Create list");
  }
};
struct BoundaryContext {
  Manager* manager = nullptr;
  std::uint64_t generation = 0, accepted = 0, refused = 0;
  std::uint64_t copy_accepted = 0, copy_refused = 0;
  bool capture_copies = false;
};
struct DrawFixture {
  Ref<ID3D12RootSignature> root;
  Ref<ID3D12PipelineState> pipeline;
  void initialize(ID3D12Device* device, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
    constexpr char shader[] = R"(
cbuffer Values : register(b0) { float4 Color; };
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
}
float4 ps_main() : SV_Target { return Color; }
)";
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues = 4;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1;
    desc.pParameters = &parameter;
    Ref<ID3DBlob> serialized, vs, ps;
    check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), nullptr), "Serialize draw fixture root");
    check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(root.put())),
          "Create draw fixture root");
    check(D3DCompile(shader, sizeof(shader) - 1, "boundary_fixture", nullptr, nullptr, "vs_main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                     vs.put(), nullptr),
          "Compile fixture VS");
    check(D3DCompile(shader, sizeof(shader) - 1, "boundary_fixture", nullptr, nullptr, "ps_main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                     ps.put(), nullptr),
          "Compile fixture PS");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
    state.pRootSignature = root.p;
    state.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    state.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    state.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    state.SampleMask = UINT_MAX;
    state.SampleDesc.Count = 1;
    state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    state.RasterizerState.DepthClipEnable = TRUE;
    state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    state.NumRenderTargets = 1;
    state.RTVFormats[0] = format;
    check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(pipeline.put())), "Create draw fixture pipeline");
  }
  void record(ID3D12GraphicsCommandList7* list,
              D3D12_CPU_DESCRIPTOR_HANDLE rtv,
              const float* color,
              bool completed_pass,
              UINT width = Width,
              UINT height = Height) {
    list->SetPipelineState(pipeline.p);
    list->SetGraphicsRootSignature(root.p);
    list->SetGraphicsRoot32BitConstants(0, 4, color, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    if (completed_pass) {
      D3D12_RENDER_PASS_RENDER_TARGET_DESC target{};
      target.cpuDescriptor = rtv;
      target.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
      target.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
      list->BeginRenderPass(1, &target, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
#ifdef TAXI_RENDER_BOUNDARY_STATE_VALIDATION
      require((Boundary::validation_state(list, 23) & 47u) == 35u, "Native Begin observes an enabled active ordinary pass");
#endif
    } else {
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    }
    list->DrawInstanced(3, 1, 0, 0);
    if (completed_pass) {
      list->EndRenderPass();
#ifdef TAXI_RENDER_BOUNDARY_STATE_VALIDATION
      require(Boundary::validation_state(list, 23) == 49u, "Native End supplies completed ordinary-pass proof");
#endif
    }
  }
};
void before_legacy(void* raw,
                   ID3D12GraphicsCommandList* list,
                   std::uint64_t generation,
                   const D3D12_RESOURCE_TRANSITION_BARRIER& value) noexcept {
  auto& context = *static_cast<BoundaryContext*>(raw);
  if (context.capture_copies)
    return;
  if (generation != context.generation || (value.Subresource != 0 && value.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
    return;
  if (context.manager->record_render_target_before_transition(list, value.pResource, true, generation))
    ++context.accepted;
  else
    ++context.refused;
}
void before_enhanced(void* raw, ID3D12GraphicsCommandList7* list, std::uint64_t generation, const D3D12_TEXTURE_BARRIER& value) noexcept {
  auto& context = *static_cast<BoundaryContext*>(raw);
  if (context.capture_copies)
    return;
  const auto& range = value.Subresources;
  const bool whole = range.NumMipLevels == 0 ? range.IndexOrFirstMipLevel == 0 || range.IndexOrFirstMipLevel == UINT_MAX
                                             : range.IndexOrFirstMipLevel == 0 && range.NumMipLevels == 1 && range.FirstArraySlice == 0 &&
                                                   range.NumArraySlices == 1 && range.FirstPlane == 0 && range.NumPlanes == 1;
  if (generation != context.generation || !whole)
    return;
  if (context.manager->record_render_target_before_enhanced_transition(list, value.pResource, true, generation))
    ++context.accepted;
  else
    ++context.refused;
}
void after_copy_resource(void* raw,
                         ID3D12GraphicsCommandList* list,
                         std::uint64_t generation,
                         ID3D12Resource* destination,
                         ID3D12Resource* source,
                         bool allowed) noexcept {
  auto& context = *static_cast<BoundaryContext*>(raw);
  if (!context.capture_copies)
    return;
  if (context.manager->record_copy_after_forward(list, source, destination, allowed, generation))
    ++context.copy_accepted;
  else
    ++context.copy_refused;
}
void after_copy_texture(void* raw,
                        ID3D12GraphicsCommandList* list,
                        std::uint64_t generation,
                        const D3D12_TEXTURE_COPY_LOCATION* destination,
                        UINT x,
                        UINT y,
                        UINT z,
                        const D3D12_TEXTURE_COPY_LOCATION* source,
                        const D3D12_BOX* box,
                        bool allowed) noexcept {
  auto& context = *static_cast<BoundaryContext*>(raw);
  if (!context.capture_copies)
    return;
  if (context.manager->record_texture_copy_after_forward(list, destination, x, y, z, source, box, allowed, generation))
    ++context.copy_accepted;
  else
    ++context.copy_refused;
}
void run(bool warp, bool render_target, bool enhanced, bool native_boundary, bool completed_pass, bool native_copy, bool texture_copy) {
  Ref<ID3D12Debug> debug;
  const bool debug_layer = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())));
  if (debug_layer)
    debug->EnableDebugLayer();
  Ref<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "Create factory");
  Ref<IDXGIAdapter> adapter;
  if (warp)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "WARP adapter");
  Ref<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "Create device");
  if (enhanced) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options))) ||
        !options.EnhancedBarriersSupported) {
      std::printf("{\"passed\":true,\"skipped\":true,\"reason\":\"Enhanced barriers unsupported\",\"adapter\":\"%s\"}\n",
                  warp ? "WARP" : "hardware");
      return;
    }
  }
  Ref<ID3D12InfoQueue> info;
  if (debug_layer)
    check(device->QueryInterface(IID_PPV_ARGS(info.put())), "Info queue");
  taxi_camera::SceneHandoff handoff;
  auto manager = std::make_unique<Manager>(handoff);
  constexpr std::uint64_t DeviceKey = 11, Generation = 23;
  require(manager->register_device(DeviceKey, device.p), "Register device");
  require(handoff.register_device(DeviceKey) != 0, "Register handoff device");
  Commands application, second, private_work, pfd;
  application.initialize(device.p);
  second.initialize(device.p);
  private_work.initialize(device.p);
  pfd.initialize(device.p);
  Ref<ID3D12GraphicsCommandList7> enhanced_list;
  if (enhanced || native_boundary) {
    check(application.list->QueryInterface(IID_PPV_ARGS(enhanced_list.put())), "Get native enhanced list");
    require(static_cast<ID3D12GraphicsCommandList*>(enhanced_list.p) == application.list.p, "Enhanced interface changes list identity");
  }
  require(manager->register_command_list(application.list.p, DeviceKey, Generation), "Register recording");
  require(manager->register_command_list(pfd.list.p, DeviceKey, Generation + 1), "Register consumer");
  BoundaryContext boundary_context{manager.get(), Generation};
  boundary_context.capture_copies = native_copy;
  if (native_boundary) {
    Boundary::Callbacks callbacks{&boundary_context, before_legacy, before_enhanced};
    if (native_copy) {
      callbacks.after_copy_resource = after_copy_resource;
      callbacks.after_copy_texture = after_copy_texture;
    }
    const auto installed = Boundary::register_list(application.list.p, Generation, callbacks);
    require(installed.ready && installed.protection_restored, installed.status);
  }
  DrawFixture draw_fixture;
  if (native_boundary)
    draw_fixture.initialize(device.p);
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = Width;
  desc.Height = Height;
  desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  auto default_heap = heap(D3D12_HEAP_TYPE_DEFAULT);
  std::array<Ref<ID3D12Resource>, 2> sources, scenes;
  Ref<ID3D12Resource> stable;
  for (unsigned index = 0; index < 2; ++index) {
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                          IID_PPV_ARGS(sources[index].put())),
          "Source texture");
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(scenes[index].put())),
          "Scene texture");
    require(handoff.register_resource(DeviceKey, reinterpret_cast<std::uint64_t>(scenes[index].p), 90 + index), "Handoff scene");
    if (native_copy && index == 1)
      require(handoff.register_resource(DeviceKey, reinterpret_cast<std::uint64_t>(sources[index].p), 94), "Handoff copy source");
  }
  check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                        IID_PPV_ARGS(stable.put())),
        "Stable output");
  handoff.begin_scene();
  const auto ticket = handoff.begin_capture();
  require(handoff.publish(
              ticket, {77, 1}, {101, 102},
              {reinterpret_cast<std::uint64_t>(scenes[0].p), reinterpret_cast<std::uint64_t>(native_copy ? sources[1].p : scenes[1].p)}),
          "Publish scene identity");
  Ref<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = 2;
  check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(rtvs.put())), "RTV heap");
  const auto first_rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  const std::array<std::array<unsigned char, 4>, 2> colors{{{31, 87, 203, 255}, {183, 29, 67, 255}}};
  for (unsigned index = 0; index < 2; ++index) {
    auto rtv = first_rtv;
    rtv.ptr += increment * index;
    const float color[]{colors[index][0] / 255.f, colors[index][1] / 255.f, colors[index][2] / 255.f, 1};
    if (render_target) {
      const auto capture = [&](bool proof) {
        if (native_boundary && proof)
          return true;  // The next actual native transition invokes the callback.
        return enhanced ? manager->record_render_target_before_enhanced_transition(enhanced_list.p, scenes[index].p, proof, Generation)
                        : manager->record_render_target_before_transition(application.list.p, scenes[index].p, proof, Generation);
      };
      const auto exit_rt = [&] {
        if (enhanced)
          enhanced_barrier(enhanced_list.p, scenes[index].p, D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_COPY_SOURCE,
                           D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_COPY_SOURCE);
        else
          barrier(application.list.p, scenes[index].p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      };
      const auto reenter_rt = [&] {
        if (enhanced)
          enhanced_barrier(enhanced_list.p, scenes[index].p, D3D12_BARRIER_LAYOUT_COPY_SOURCE, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                           D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_ACCESS_RENDER_TARGET);
        else
          barrier(application.list.p, scenes[index].p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      };
      device->CreateRenderTargetView(scenes[index].p, nullptr, rtv);
      if (enhanced) {
        // Cross the legacy/enhanced boundary only through COMMON. The capture
        // itself must use enhanced barriers on the source throughout.
        barrier(application.list.p, scenes[index].p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        enhanced_barrier(enhanced_list.p, scenes[index].p, D3D12_BARRIER_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                         D3D12_BARRIER_ACCESS_COMMON, D3D12_BARRIER_ACCESS_RENDER_TARGET);
      } else {
        barrier(application.list.p, scenes[index].p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
      }
      const float incomplete[]{1, 0, 1, 1};
      application.list->ClearRenderTargetView(rtv, incomplete, 0, nullptr);
      if (native_boundary) {
        if (index == 0) {
          // Clear alone does not rule out a first cross-list RESUMING pass.
          // The real pre-proof transition must forward without extra GPU work.
          exit_rt();
          require(boundary_context.accepted == 0 && manager->statistics().captures == 0, "Capture preceded native draw/pass proof");
          reenter_rt();
        }
        draw_fixture.record(enhanced_list.p, rtv, incomplete, completed_pass);
      }
      require(!capture(false), "Missing RT proof accepted");
      require(!manager->record_render_target_before_transition(second.list.p, scenes[index].p, true, Generation),
              "Unregistered recording accepted");
      require(!manager->record_render_target_before_transition(application.list.p, sources[index].p, true, Generation),
              "Unmatched RT source accepted");
      require(!manager->record_render_target_before_transition(application.list.p, scenes[index].p, true, Generation + 1),
              "Stale RT callback accepted");
      if (enhanced)
        require(!manager->record_render_target_before_enhanced_transition(enhanced_list.p, scenes[index].p, true, Generation + 1),
                "Stale enhanced callback accepted");
      require(capture(true), "Initial RT capture failed");
      // These are the application's real transitions. The helper must restore
      // RT before each one, and preserve a later write on the same resource.
      exit_rt();
      reenter_rt();
      application.list->ClearRenderTargetView(rtv, color, 0, nullptr);
      if (native_boundary)
        draw_fixture.record(enhanced_list.p, rtv, color, completed_pass);
      require(capture(true), "Final RT capture failed");
      if (enhanced) {
        enhanced_barrier(enhanced_list.p, scenes[index].p, D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_COMMON,
                         D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_COMMON);
        barrier(application.list.p, scenes[index].p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
      } else {
        barrier(application.list.p, scenes[index].p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
      }
      continue;
    }
    device->CreateRenderTargetView(sources[index].p, nullptr, rtv);
    if (native_copy) {
      D3D12_TEXTURE_COPY_LOCATION from{}, to{};
      from.pResource = sources[index].p;
      to.pResource = scenes[index].p;
      from.Type = to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      const D3D12_BOX full_box{0, 0, 0, Width, Height, 1};
      const auto copy = [&] {
        if (texture_copy)
          application.list->CopyTextureRegion(&to, 0, 0, 0, &from, index == 0 ? nullptr : &full_box);
        else
          application.list->CopyResource(scenes[index].p, sources[index].p);
      };
      require(!manager->record_copy_after_forward(application.list.p, sources[index].p, scenes[index].p, true, Generation + 1),
              "Stale native copy accepted");
      require(!manager->record_copy_after_forward(application.list.p, sources[index].p, scenes[index].p, true, 0),
              "Zero native copy generation accepted");
      require(!manager->record_copy_after_forward(application.list.p, sources[index].p, scenes[index].p, false, Generation),
              "Missing whole-copy proof accepted");
      require(!manager->record_texture_copy_after_forward(application.list.p, &to, 1, 0, 0, &from, nullptr, true, Generation),
              "Offset texture copy accepted");
      auto invalid_from = from;
      invalid_from.SubresourceIndex = 1;
      require(!manager->record_texture_copy_after_forward(application.list.p, &to, 0, 0, 0, &invalid_from, nullptr, true, Generation),
              "Nonzero source mip accepted");
      invalid_from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      require(!manager->record_texture_copy_after_forward(application.list.p, &to, 0, 0, 0, &invalid_from, nullptr, true, Generation),
              "Buffer footprint accepted");
      require(manager->statistics().copies[index].matched_copies == 0, "Unproved copy polluted diagnostics");
      const float incomplete[]{1, 0, 1, 1};
      barrier(application.list.p, sources[index].p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      application.list->ClearRenderTargetView(rtv, incomplete, 0, nullptr);
      barrier(application.list.p, sources[index].p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      if (texture_copy) {
        const D3D12_BOX partial{0, 0, 0, Width / 2, Height, 1};
        application.list->CopyTextureRegion(&to, 0, 0, 0, &from, &partial);
        require(manager->statistics().captures == index, "Partial native copy captured a feed");
      }
      copy();
      barrier(application.list.p, sources[index].p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      application.list->ClearRenderTargetView(rtv, color, 0, nullptr);
      barrier(application.list.p, sources[index].p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      copy();
      continue;
    }
    barrier(application.list.p, sources[index].p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    application.list->ClearRenderTargetView(rtv, color, 0, nullptr);
    barrier(application.list.p, sources[index].p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    application.list->CopyResource(scenes[index].p, sources[index].p);
    require(!manager->record_copy_after_forward(application.list.p, sources[index].p, scenes[index].p, false), "Partial copy accepted");
    manager->set_capture_enabled(false);
    require(!manager->record_copy_after_forward(application.list.p, sources[index].p, scenes[index].p, true),
            "Idle admitted a new copy capture");
    manager->set_capture_enabled(true);
    require(manager->record_copy_after_forward(application.list.p, sources[index].p, scenes[index].p, true), "Capture whole destination");
    require(!manager->record_copy_after_forward(application.list.p, sources[index].p, scenes[index].p, true), "Duplicate feed capture");
  }
  if (manager->statistics().captures != 2) {
    const auto observed = Boundary::statistics();
    std::fprintf(stderr, "Capture diagnostic: packets=%llu accepted=%llu refused=%llu candidates=%llu/%llu pass_refusals=%llu\n",
                 static_cast<unsigned long long>(manager->statistics().captures),
                 static_cast<unsigned long long>(boundary_context.accepted), static_cast<unsigned long long>(boundary_context.refused),
                 static_cast<unsigned long long>(observed.legacy_candidates), static_cast<unsigned long long>(observed.enhanced_candidates),
                 static_cast<unsigned long long>(observed.pass_refusals));
  }
  require(manager->statistics().captures == 2, "RT rewrites allocated a second feed packet");
  require(manager->statistics().render_target_writes == (render_target ? 4u : 0u), "RT write count mismatch");
  require(manager->statistics().render_target_rewrites == (render_target ? 2u : 0u), "RT rewrite count mismatch");
  for (unsigned feed = 0; feed < 2; ++feed) {
    const auto diagnostic = manager->statistics().render_targets[feed];
    require(diagnostic.matched_boundaries == (render_target ? 2u : 0u), "Unmatched boundary polluted capture diagnostics");
    if (render_target)
      require(diagnostic.width == Width && diagnostic.height == Height && diagnostic.format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                  diagnostic.mips == 1 && std::strcmp(diagnostic.last_refusal, "none") == 0,
              "Matched RT dimensions or success diagnostic missing");
  }
  if (native_boundary && !native_copy)
    require(boundary_context.accepted == 4 && boundary_context.refused == 0, "Native boundary callback capture mismatch");
  if (native_copy) {
    require(boundary_context.copy_accepted == 4 && boundary_context.copy_refused == (texture_copy ? 2u : 0u),
            "Native copies recursed or missed whole operations");
    require(manager->statistics().copy_writes == 4 && manager->statistics().copy_rewrites == 2, "Native final-copy rewrite missing");
    for (unsigned feed = 0; feed < 2; ++feed) {  // The fixture publishes two of the three feeds.
      const auto diagnostic = manager->statistics().copies[feed];
      require(diagnostic.matched_copies == 2 && diagnostic.width == Width && diagnostic.height == Height && diagnostic.mips == 1 &&
                  diagnostic.format == DXGI_FORMAT_R8G8B8A8_UNORM && std::strcmp(diagnostic.last_refusal, "none") == 0,
              "Native copy diagnostics mismatch");
    }
    boundary_context.capture_copies = false;
  }
  check(application.list->Close(), "Close application recording");
  ID3D12CommandList* batch[]{application.list.p};
  Ref<ID3D12Fence> done, blocked;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(done.put())), "Done fence");
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(blocked.put())), "Blocking fence");
  auto receipt = manager->before_submission(application.queue.p, 1, batch);
  require(receipt != 0, "Producer receipt missing");
  application.queue->ExecuteCommandLists(1, batch);
  manager->after_submission(application.queue.p, receipt);
  if (native_copy) {
    require(!manager->record_copy_after_forward(application.list.p, sources[0].p, scenes[0].p, true, Generation),
            "Submitted native copy recording was modified");
    require(std::strcmp(manager->statistics().copies[0].last_refusal, "recording_busy_or_identity_changed") == 0,
            "Submitted native copy refusal missing");
  }
  if (render_target)
    require(!manager->record_render_target_before_transition(application.list.p, scenes[0].p, true, Generation),
            "Submitted RT recording was modified");
  if (render_target)
    require(std::strcmp(manager->statistics().render_targets[0].last_refusal, "recording_busy_or_identity_changed") == 0,
            "Submitted-recording refusal diagnostic missing");
  check(application.queue->Signal(done.p, 1), "Initial submission fence");
  wait([&] { return done->GetCompletedValue() >= 1; });
  std::array<Manager::Frame, 2> frames{};
  require(manager->poll_completed_frames(frames.data(), frames.size()) == 0, "Replayable recording published before reset");
  manager->successful_reset(application.list.p, Generation + 99);
  require(manager->poll_completed_frames(frames.data(), frames.size()) == 0, "Wrong incarnation reset retired recording");
  // Replay the same recording on a SECOND queue, only after its prior execution
  // completed. The manager inserts its cross-queue dependency automatically.
  check(second.queue->Wait(blocked.p, 1), "Block replay queue");
  receipt = manager->before_submission(second.queue.p, 1, batch);
  require(receipt != 0, "Replay receipt missing");
  second.queue->ExecuteCommandLists(1, batch);
  Ref<ID3D12CommandAllocator> replacement;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(replacement.put())), "Replacement allocator");
  check(application.list->Reset(replacement.p, nullptr), "Reset after real native submission");
  manager->successful_reset(application.list.p, Generation);
  if (native_boundary)
    Boundary::successful_reset(application.list.p, Generation);
  require(manager->poll_completed_frames(frames.data(), frames.size()) == 0, "Reset bypassed in-flight receipt");
  manager->after_submission(second.queue.p, receipt);
  require(manager->poll_completed_frames(frames.data(), frames.size()) == 0, "Blocked producer bypassed GPU fence");
  check(blocked->Signal(1), "Release replay queue");
  std::size_t ready_count = 0;
  wait([&] {
    ready_count += manager->poll_completed_frames(frames.data() + ready_count, frames.size() - ready_count);
    return ready_count == 2;
  });
  require(frames[0].token != frames[1].token && frames[0].match.feed != frames[1].match.feed, "Feed leases overlap");
  std::array<Manager::Frame, 2> duplicate{};
  require(manager->poll_completed_frames(duplicate.data(), duplicate.size()) == 0, "Frame leased twice");

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 size = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
  const auto stride = (size + 511) & ~UINT64{511};
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = stride * 3;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  auto read_heap = heap(D3D12_HEAP_TYPE_READBACK);
  Ref<ID3D12Resource> readback;
  check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(readback.put())),
        "Readback");
  auto copy_to_readback = [&](ID3D12GraphicsCommandList* list, ID3D12Resource* from, unsigned index) {
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = from;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = readback.p;
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint = footprint;
    target.PlacedFootprint.Offset = index * stride;
    list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
  };
  for (const auto& frame : frames) {
    barrier(private_work.list.p, frame.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_to_readback(private_work.list.p, frame.resource, frame.match.feed);
    if (frame.match.feed == 0) {
      barrier(private_work.list.p, stable.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      private_work.list->CopyResource(stable.p, frame.resource);
      barrier(private_work.list.p, stable.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    barrier(private_work.list.p, frame.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  }
  check(private_work.list->Close(), "Close private work");
  check(private_work.queue->Wait(blocked.p, 2), "Block private work");
  const auto private_receipt = manager->begin_private_submission(DeviceKey, private_work.queue.p);
  require(private_receipt.receipt != 0, "Private receipt missing");
  ID3D12CommandList* private_batch[]{private_work.list.p};
  private_work.queue->ExecuteCommandLists(1, private_batch);
  require(manager->end_private_submission(private_receipt.receipt), "Private signal failed");
  for (const auto& frame : frames)
    require(manager->finish_consumption(frame.token, private_receipt.fence, private_receipt.value), "Finish private frame reads");
  require(private_receipt.fence->GetCompletedValue() < private_receipt.value, "Blocking private queue unexpectedly completed");
  copy_to_readback(pfd.list.p, stable.p, 2);
  require(manager->register_consumer_recording(pfd.list.p), "Register stable-output consumer");
  check(pfd.list->Close(), "Close PFD consumer");
  ID3D12CommandList* pfd_batch[]{pfd.list.p};
  receipt = manager->before_submission(pfd.queue.p, 1, pfd_batch);
  require(receipt != 0, "Consumer-only submission omitted timeline");
  pfd.queue->ExecuteCommandLists(1, pfd_batch);
  manager->after_submission(pfd.queue.p, receipt);
  check(pfd.queue->Signal(done.p, 2), "Consumer fence");
  require(done->GetCompletedValue() < 2, "Consumer passed unfinished private output write");
  check(blocked->Signal(2), "Release private work");
  wait([&] { return done->GetCompletedValue() >= 2; });
  void* pixels = nullptr;
  D3D12_RANGE range{0, static_cast<SIZE_T>(stride * 3)};
  check(readback->Map(0, &range, &pixels), "Read completed GPU pixels");
  unsigned checked_pixels = 0;
  for (unsigned index = 0; index < 3; ++index)
    for (unsigned y = 0; y < Height; ++y)
      for (unsigned x = 0; x < Width; ++x) {
        const auto* pixel = static_cast<unsigned char*>(pixels) + index * stride + y * footprint.Footprint.RowPitch + x * 4;
        require(std::memcmp(pixel, colors[index == 2 ? 0 : index].data(), 4) == 0, "Captured/PFD pixels mismatch");
        ++checked_pixels;
      }
  D3D12_RANGE none{0, 0};
  readback->Unmap(0, &none);
  manager->poll_completed_frames(duplicate.data(), duplicate.size());
  const auto bytes = manager->statistics().bytes;
  // More canceled recordings than the complete pool capacity must still reuse
  // the original allocation, after successful Reset proves no possible replay.
  for (unsigned cycle = 0; cycle < Manager::MaximumPackets + 4; ++cycle) {
    application.list->CopyResource(scenes[0].p, sources[0].p);
    require(manager->record_copy_after_forward(application.list.p, sources[0].p, scenes[0].p, true), "Canceled recording exhausted pool");
    check(application.list->Close(), "Close unsubmitted recording");
    check(replacement->Reset(), "Reset unused allocator");
    check(application.list->Reset(replacement.p, nullptr), "Reset never-submitted list");
    manager->successful_reset(application.list.p, Generation);
    if (native_boundary)
      Boundary::successful_reset(application.list.p, Generation);
    require(manager->statistics().bytes == bytes, "Cancellation allocated additional storage");
  }
  if (render_target) {
    Ref<ID3D12Resource> unsupported;
    auto unsupported_desc = desc;
    unsupported_desc.MipLevels = 2;
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &unsupported_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          nullptr, IID_PPV_ARGS(unsupported.put())),
          "Create multi-mip refusal fixture");
    require(handoff.register_resource(DeviceKey, reinterpret_cast<std::uint64_t>(unsupported.p), 105), "Register multi-mip fixture");
    require(handoff.publish(handoff.begin_capture(), {77, 1}, {101, 102},
                            {reinterpret_cast<std::uint64_t>(unsupported.p), reinterpret_cast<std::uint64_t>(scenes[1].p)}),
            "Publish multi-mip fixture");
    const auto previous = manager->statistics().render_targets[0].matched_boundaries;
    require(!manager->record_render_target_before_transition(application.list.p, unsupported.p, true, Generation),
            "Multi-mip RT source accepted");
    const auto diagnostic = manager->statistics().render_targets[0];
    require(diagnostic.matched_boundaries == previous + 1 && diagnostic.mips == 2 && diagnostic.width == Width &&
                std::strcmp(diagnostic.last_refusal, "unsupported_texture_shape_mips_or_samples") == 0 &&
                manager->statistics().bytes == bytes,
            "Unsupported source metadata/refusal was lost or allocated storage");
  }
  check(application.list->Close(), "Close final empty recording");
  manager->destroy_command_list(application.list.p, Generation);
  manager->destroy_command_list(pfd.list.p, Generation + 1);
  if (native_boundary) {
    Boundary::unregister_list(application.list.p, Generation);
    const auto removed = Boundary::remove();
    require(removed.protection_restored && !Boundary::operational(), "Native boundary observation remained active");
  }
  unsigned debug_errors = 0;
  if (info.p)
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
      SIZE_T bytes_needed = 0;
      check(info->GetMessage(i, nullptr, &bytes_needed), "Debug message size");
      std::vector<unsigned char> message(bytes_needed);
      auto* value = reinterpret_cast<D3D12_MESSAGE*>(message.data());
      check(info->GetMessage(i, value, &bytes_needed), "Debug message");
      if (value->Severity == D3D12_MESSAGE_SEVERITY_ERROR || value->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
        ++debug_errors;
    }
  require(debug_errors == 0, "Debug layer reported GPU errors");
  std::printf(
      "{\"passed\":true,\"checks\":%u,\"checked_pixels\":%u,\"capture_packets\":2,\"canceled_reuses\":20,"
      "\"render_target_writes\":%llu,\"render_target_rewrites\":%llu,\"mode\":\"%s\","
      "\"native_boundary\":%s,\"boundary_captures\":%llu,\"completed_pass_proof\":%s,"
      "\"native_copy\":%s,\"native_copy_captures\":%llu,\"texture_copy\":%s,"
      "\"debugLayer\":%s,\"debugErrors\":%u,\"adapter\":\"%s\"}\n",
      checks, checked_pixels, static_cast<unsigned long long>(manager->statistics().render_target_writes),
      static_cast<unsigned long long>(manager->statistics().render_target_rewrites),
      enhanced        ? "enhanced_target"
      : render_target ? "render_target"
                      : "copy",
      native_boundary ? "true" : "false", static_cast<unsigned long long>(boundary_context.accepted), completed_pass ? "true" : "false",
      native_copy ? "true" : "false", static_cast<unsigned long long>(boundary_context.copy_accepted), texture_copy ? "true" : "false",
      debug_layer ? "true" : "false", debug_errors, warp ? "WARP" : "hardware");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    bool warp = false, render_target = false, enhanced = false, native_boundary = false, completed_pass = false;
    bool native_copy = false, texture_copy = false;
    for (int index = 1; index < argc; ++index) {
      if (std::strcmp(argv[index], "--warp") == 0 && !warp)
        warp = true;
      else if (std::strcmp(argv[index], "--render-target") == 0 && !render_target)
        render_target = true;
      else if (std::strcmp(argv[index], "--enhanced-target") == 0 && !render_target) {
        render_target = true;
        enhanced = true;
      } else if (std::strcmp(argv[index], "--boundary-observer") == 0 && !native_boundary)
        native_boundary = true;
      else if (std::strcmp(argv[index], "--completed-pass") == 0 && !completed_pass)
        completed_pass = true;
      else if (std::strcmp(argv[index], "--native-copy") == 0 && !native_copy) {
        native_copy = true;
        native_boundary = true;
      } else if (std::strcmp(argv[index], "--native-texture-copy") == 0 && !native_copy) {
        native_copy = texture_copy = native_boundary = true;
      } else
        require(false, "Only --warp and one render-target mode accepted");
    }
    require(!native_boundary || render_target || native_copy, "Boundary observer requires a native GPU capture mode");
    require(!native_copy || !render_target, "Native copy and RT modes are exclusive");
    require(!completed_pass || native_boundary, "Completed pass requires native boundary observation");
    run(warp, render_target, enhanced, native_boundary, completed_pass, native_copy, texture_copy);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
