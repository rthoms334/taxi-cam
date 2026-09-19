#include "../../src/graphics/pfd_submission_pool.hpp"
#include "../../src/hooks/queue_submit_observer.hpp"

#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using Pool = taxi_camera::pfd_submission::Pool;
namespace Queue = taxi_camera::engine_hook::queue_submit;
unsigned checks = 0;
constexpr UINT Width = 32, Height = 16, PatchWidth = 16, PatchHeight = 8;
template <class T>
struct Ref {
  T* p = nullptr;
  ~Ref() {
    if (p)
      p->Release();
  }
  T* operator->() const noexcept { return p; }
  T** put() noexcept { return &p; }
  void reset() noexcept {
    if (p)
      p->Release();
    p = nullptr;
  }
};
void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}
void success(HRESULT result, const char* message) {
  require(SUCCEEDED(result), message);
}
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES result{};
  result.Type = type;
  result.CreationNodeMask = result.VisibleNodeMask = 1;
  return result;
}
D3D12_RESOURCE_DESC texture() {
  D3D12_RESOURCE_DESC result{};
  result.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  result.Width = Width;
  result.Height = Height;
  result.DepthOrArraySize = result.MipLevels = 1;
  result.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  result.SampleDesc.Count = 1;
  result.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  return result;
}
void create_buffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, Ref<ID3D12Resource>& result) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const auto properties = heap(type);
  success(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(result.put())),
          "Create fixture buffer");
}
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, 0, before, after};
  list->ResourceBarrier(1, &barrier);
}
void complete(ID3D12Fence* fence, std::uint64_t value) {
  const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(event != nullptr, "Create completion event");
  const auto result = fence->SetEventOnCompletion(value, event);
  const auto waited = SUCCEEDED(result) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
  CloseHandle(event);
  require(waited == WAIT_OBJECT_0 && fence->GetCompletedValue() >= value, "Owned packet GPU completion timed out");
}

struct Context {
  Pool pool;
  Pool::Copy copy;
  Pool::Recording packet;
  ID3D12Fence* fence = nullptr;
  std::array<ID3D12CommandList*, 3> originals{};
  ID3D12CommandList* separate_producer = nullptr;
  UINT original_count = 3;
  bool before_list = false;
  std::uint64_t receipt = 0;
  unsigned before = 0, after = 0, accepted = 0, refused = 0, bad = 0;
};
Queue::Callbacks callbacks(Context* context) {
  Queue::Callbacks result{};
  result.context = context;
  result.before = [](void* opaque, ID3D12CommandQueue*, UINT count, ID3D12CommandList* const* lists) noexcept {
    auto& current = *static_cast<Context*>(opaque);
    if (count == 1 && lists[0] == current.separate_producer)
      return std::uint64_t{};
    ++current.before;
    if (count != current.original_count) {
      ++current.bad;
      return std::uint64_t{};
    }
    for (UINT i = 0; i < count; ++i)
      if (lists[i] != current.originals[i])
        ++current.bad;
    current.packet = current.pool.record(current.copy);
    if (!current.packet) {
      ++current.bad;
      return std::uint64_t{};
    }
    return ++current.receipt;
  };
  result.augment = [](void* opaque, ID3D12CommandQueue*, std::uint64_t receipt, UINT, ID3D12CommandList* const*, Queue::Insertion* output,
                      UINT capacity) noexcept {
    auto& current = *static_cast<Context*>(opaque);
    if (!capacity || receipt != current.receipt || !current.packet)
      return UINT{};
    output[0] = {current.before_list ? 0u : 1u, current.packet.list, current.before_list};
    return UINT{1};
  };
  result.augmentation_result = [](void* opaque, ID3D12CommandQueue*, std::uint64_t, UINT inserted) noexcept {
    auto& current = *static_cast<Context*>(opaque);
    current.accepted += inserted;
    if (inserted != 1)
      ++current.bad;
  };
  result.after = [](void* opaque, ID3D12CommandQueue* queue, std::uint64_t receipt) noexcept {
    auto& current = *static_cast<Context*>(opaque);
    ++current.after;
    if (FAILED(queue->Signal(current.fence, receipt))) {
      ++current.bad;
      current.pool.quarantine(current.packet.slot);
    } else {
      current.pool.submit(current.packet.slot, receipt);
    }
  };
  result.refused = [](void* opaque, ID3D12CommandQueue*, Queue::Refusal) noexcept {
    auto& current = *static_cast<Context*>(opaque);
    ++current.refused;
    current.pool.quarantine(current.packet.slot);
  };
  return result;
}

struct DeathProbe final : IUnknown {
  explicit DeathProbe(std::atomic<unsigned>& destroyed) : destroyed_(destroyed) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
    *output = nullptr;
    if (iid != __uuidof(IUnknown))
      return E_NOINTERFACE;
    *output = static_cast<IUnknown*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = --references_;
    if (!remaining) {
      ++destroyed_;
      delete this;
    }
    return remaining;
  }

 private:
  std::atomic<unsigned>& destroyed_;
  std::atomic<ULONG> references_{1};
};
constexpr GUID ProbeId{0xa104ce95, 0xe219, 0x4d78, {0x9d, 0xa8, 0x61, 0x6a, 0x03, 0x8b, 0x2e, 0x10}};

void run(bool warp, bool common, bool before) {
  Ref<ID3D12Debug> debug;
  const bool debug_enabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())));
  if (debug_enabled)
    debug->EnableDebugLayer();
  Ref<IDXGIFactory4> factory;
  Ref<IDXGIAdapter> adapter;
  Ref<ID3D12Device> device;
  success(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "Create factory");
  if (warp)
    success(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "Get WARP adapter");
  success(D3D12CreateDevice(adapter.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "Create device");
  Ref<ID3D12InfoQueue> diagnostics;
  device->QueryInterface(IID_PPV_ARGS(diagnostics.put()));
  Ref<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  success(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.put())), "Create queue");
  Ref<ID3D12Fence> fence, blocker;
  success(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())), "Create outer receipt fence");
  success(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(blocker.put())), "Create test GPU blocker");
  std::array<Ref<ID3D12CommandAllocator>, 3> allocators;
  std::array<Ref<ID3D12GraphicsCommandList>, 3> lists;
  for (unsigned i = 0; i < lists.size(); ++i) {
    success(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocators[i].put())), "Create original allocator");
    success(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].p, nullptr, IID_PPV_ARGS(lists[i].put())),
            "Create original list");
  }
  Ref<ID3D12Resource> target, visible, upload, readback, lower_upload, lower_readback;
  const auto desc = texture();
  auto target_desc = desc;
  target_desc.MipLevels = 5;
  const auto properties = heap(D3D12_HEAP_TYPE_DEFAULT);
  success(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                          IID_PPV_ARGS(target.put())),
          "Create selected target");
  success(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                          IID_PPV_ARGS(visible.put())),
          "Create actual consumer output");
  Ref<ID3D12DescriptorHeap> rtvs, srvs;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap_desc.NumDescriptors = 2;
  success(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(rtvs.put())), "Create RTV heap");
  auto target_rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  auto visible_rtv = target_rtv;
  visible_rtv.ptr += device->GetDescriptorHandleIncrementSize(heap_desc.Type);
  device->CreateRenderTargetView(target.p, nullptr, target_rtv);
  device->CreateRenderTargetView(visible.p, nullptr, visible_rtv);
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  heap_desc.NumDescriptors = 1;
  success(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(srvs.put())), "Create consumer SRV heap");
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = desc.Format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(target.p, &srv, srvs->GetCPUDescriptorHandleForHeapStart());

  D3D12_RESOURCE_DESC patch_desc = desc;
  patch_desc.Width = PatchWidth;
  patch_desc.Height = PatchHeight;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT patch_footprint{}, visible_footprint{};
  UINT64 patch_bytes{}, visible_bytes{};
  device->GetCopyableFootprints(&patch_desc, 0, 1, 0, &patch_footprint, nullptr, nullptr, &patch_bytes);
  device->GetCopyableFootprints(&desc, 0, 1, 0, &visible_footprint, nullptr, nullptr, &visible_bytes);
  create_buffer(device.p, patch_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, upload);
  create_buffer(device.p, visible_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, readback);
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 4> lower_footprints{};
  UINT64 lower_bytes{};
  device->GetCopyableFootprints(&target_desc, 1, 4, 0, lower_footprints.data(), nullptr, nullptr, &lower_bytes);
  create_buffer(device.p, lower_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, lower_upload);
  create_buffer(device.p, lower_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, lower_readback);
  std::uint8_t* bytes{};
  const D3D12_RANGE none{};
  success(upload->Map(0, &none, reinterpret_cast<void**>(&bytes)), "Map typed patch");
  for (UINT y = 0; y < PatchHeight; ++y)
    for (UINT x = 0; x < PatchWidth; ++x) {
      auto* pixel = bytes + patch_footprint.Offset + y * patch_footprint.Footprint.RowPitch + x * 4;
      pixel[0] = static_cast<std::uint8_t>(180 + x);
      pixel[1] = static_cast<std::uint8_t>(70 + y);
      pixel[2] = 219;
      pixel[3] = 255;
    }
  upload->Unmap(0, nullptr);
  success(lower_upload->Map(0, &none, reinterpret_cast<void**>(&bytes)), "Map lower-mip sentinels");
  for (unsigned mip = 0; mip < lower_footprints.size(); ++mip) {
    const auto& footprint = lower_footprints[mip];
    for (UINT y = 0; y < footprint.Footprint.Height; ++y)
      for (UINT x = 0; x < footprint.Footprint.Width; ++x)
        for (UINT channel = 0; channel < 4; ++channel)
          bytes[footprint.Offset + y * footprint.Footprint.RowPitch + x * 4 + channel] = static_cast<std::uint8_t>(81 + mip * 13 + channel);
  }
  lower_upload->Unmap(0, nullptr);
  constexpr char shader[] = R"(
Texture2D<float4> Image : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
}
float4 ps(float4 position : SV_Position) : SV_Target { return Image.Load(int3(position.xy, 0)); }
)";
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameter.DescriptorTable = {1, &range};
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.NumParameters = 1;
  root_desc.pParameters = &parameter;
  Ref<ID3DBlob> serialized, vs, ps;
  success(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), nullptr), "Serialize consumer root");
  Ref<ID3D12RootSignature> root;
  Ref<ID3D12PipelineState> pipeline;
  success(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(root.put())),
          "Create consumer root");
  success(D3DCompile(shader, sizeof(shader) - 1, "pfd_submission_test", nullptr, nullptr, "vs", "vs_5_0", 0, 0, vs.put(), nullptr),
          "Compile consumer VS");
  success(D3DCompile(shader, sizeof(shader) - 1, "pfd_submission_test", nullptr, nullptr, "ps", "ps_5_0", 0, 0, ps.put(), nullptr),
          "Compile consumer PS");
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc{};
  pipeline_desc.pRootSignature = root.p;
  pipeline_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pipeline_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pipeline_desc.SampleMask = UINT_MAX;
  pipeline_desc.SampleDesc.Count = 1;
  pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipeline_desc.NumRenderTargets = 1;
  pipeline_desc.RTVFormats[0] = desc.Format;
  success(device->CreateGraphicsPipelineState(&pipeline_desc, IID_PPV_ARGS(pipeline.put())), "Create consumer pipeline");

  // Process-retained callbacks, just like production queue registration.
  auto* context = new Context;
  const auto exit_state = common ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  const auto copy_state = before ? D3D12_RESOURCE_STATE_RENDER_TARGET : exit_state;
  context->copy = {target.p, upload.p, patch_footprint, copy_state, 8, 4, PatchWidth, PatchHeight};
  context->fence = fence.p;
  require(!context->pool.record(context->copy), "Cold pool allocated work in a submission callback");
  require(context->pool.service(device.p, 0) && context->pool.ready_count() == Pool::Capacity,
          "Worker did not preallocate bounded packets");
  const auto valid = context->copy;
  const auto refused = [&](Pool::Copy invalid) {
    require(!context->pool.record(invalid) && context->pool.ready_count() == Pool::Capacity, "Malformed copy consumed a packet");
  };
  auto invalid = valid;
  invalid.target = nullptr;
  refused(invalid);
  invalid = valid;
  invalid.source = invalid.target;
  refused(invalid);
  invalid = valid;
  invalid.state = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  refused(invalid);
  invalid = valid;
  invalid.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  refused(invalid);
  invalid = valid;
  invalid.state = static_cast<D3D12_RESOURCE_STATES>(~UINT{0});
  refused(invalid);
  invalid = valid;
  invalid.footprint.Offset = 1;
  refused(invalid);
  invalid = valid;
  invalid.footprint.Offset = std::numeric_limits<UINT64>::max() - 511;
  refused(invalid);
  invalid = valid;
  invalid.footprint.Footprint.RowPitch = 4;
  refused(invalid);
  invalid = valid;
  invalid.footprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  refused(invalid);
  invalid = valid;
  invalid.footprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
  refused(invalid);
  invalid = valid;
  invalid.x = Width;
  refused(invalid);
  invalid = valid;
  invalid.height = 0;
  refused(invalid);
  for (unsigned i = 0; i < lists.size(); ++i)
    context->originals[i] = lists[i].p;
  if (before) {
    context->separate_producer = lists[0].p;
    context->originals[0] = lists[2].p;
    context->original_count = 1;
    context->before_list = true;
  }
  require(Queue::register_queue(queue.p, callbacks(context)).status == Queue::Status::registered, "Register actual native queue");

  for (unsigned frame = 0; frame < 2; ++frame) {
    if (frame) {
      for (unsigned i = 0; i < lists.size(); ++i) {
        success(allocators[i]->Reset(), "Reset completed original allocator");
        success(lists[i]->Reset(allocators[i].p, nullptr), "Reset completed original list");
      }
      // An implicitly promoted COMMON texture decays at Execute completion;
      // explicit shader-resource state remains exact across submissions.
      transition(lists[0].p, target.p, exit_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      transition(lists[2].p, visible.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    const float background[]{17.0f / 255.0f, 34.0f / 255.0f, (51.0f + frame) / 255.0f, 1};
    lists[0]->ClearRenderTargetView(target_rtv, background, 0, nullptr);
    if (!frame) {
      for (unsigned mip = 0; mip < lower_footprints.size(); ++mip) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {target.p, mip + 1, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST};
        lists[0]->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
        source.pResource = lower_upload.p;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = lower_footprints[mip];
        destination.pResource = target.p;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = mip + 1;
        lists[0]->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        lists[0]->ResourceBarrier(1, &barrier);
      }
    }
    auto* consumer = lists[2].p;
    // The before-list case deliberately puts the exit AND actual sampling in
    // the same original recording. Appending would miss these sampled pixels.
    transition(before ? consumer : lists[1].p, target.p, D3D12_RESOURCE_STATE_RENDER_TARGET, exit_state);
    consumer->SetPipelineState(pipeline.p);
    consumer->SetGraphicsRootSignature(root.p);
    ID3D12DescriptorHeap* heaps[]{srvs.p};
    consumer->SetDescriptorHeaps(1, heaps);
    consumer->SetGraphicsRootDescriptorTable(0, srvs->GetGPUDescriptorHandleForHeapStart());
    consumer->OMSetRenderTargets(1, &visible_rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, Width, Height, 0, 1};
    const D3D12_RECT scissor{0, 0, Width, Height};
    consumer->RSSetViewports(1, &viewport);
    consumer->RSSetScissorRects(1, &scissor);
    consumer->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    consumer->DrawInstanced(3, 1, 0, 0);
    transition(consumer, visible.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = visible.p;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource = readback.p;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = visible_footprint;
    consumer->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    for (unsigned mip = 0; mip < lower_footprints.size(); ++mip) {
      source.pResource = target.p;
      source.SubresourceIndex = mip + 1;
      destination.pResource = lower_readback.p;
      destination.PlacedFootprint = lower_footprints[mip];
      consumer->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    for (auto& list : lists)
      success(list->Close(), "Close original list");
    if (!frame)
      success(queue->Wait(blocker.p, 1), "Block fixture GPU to test fence ownership");
    if (before) {
      ID3D12CommandList* producer[]{lists[0].p};
      queue->ExecuteCommandLists(1, producer);
    }
    queue->ExecuteCommandLists(context->original_count, context->originals.data());
    require(context->before == frame + 1 && context->after == frame + 1 && context->accepted == frame + 1 && context->refused == 0 &&
                context->bad == 0,
            "Actual augmented batch was not accepted exactly once");
    if (!frame) {
      require(fence->GetCompletedValue() == 0, "Blocked GPU fabricated completion");
      require(context->pool.service(device.p, 0) && context->pool.ready_count() == Pool::Capacity - 1,
              "Outstanding inserted list was reset before its fence");
      std::array<Pool::Recording, Pool::Capacity - 1> extra;
      for (auto& packet : extra) {
        packet = context->pool.record(valid);
        require(static_cast<bool>(packet), "Reserve remaining bounded packet");
      }
      require(!context->pool.record(valid) && context->pool.ready_count() == 0, "Pool exceeded fixed packet capacity");
      for (const auto packet : extra)
        context->pool.cancel(packet.slot);
      require(context->pool.ready_count() == 0, "Cancel reset command memory on the submission thread");
      context->pool.service(device.p, 0);
      require(context->pool.ready_count() == Pool::Capacity - 1, "Cancelled unsubmitted packets were not serviced separately");
      success(blocker->Signal(1), "Release fixture GPU blocker");
    }
    complete(fence.p, frame + 1);
    require(context->pool.service(device.p, fence->GetCompletedValue()) && context->pool.ready_count() == Pool::Capacity,
            "Completed packet did not return to the worker-prepared pool");
    D3D12_RANGE readable{0, static_cast<SIZE_T>(visible_bytes)};
    success(readback->Map(0, &readable, reinterpret_cast<void**>(&bytes)), "Map actual sampled consumer pixels");
    for (UINT y = 0; y < Height; ++y)
      for (UINT x = 0; x < Width; ++x) {
        const auto* pixel = bytes + visible_footprint.Offset + y * visible_footprint.Footprint.RowPitch + x * 4;
        const bool patch = x >= 8 && x < 8 + PatchWidth && y >= 4 && y < 4 + PatchHeight;
        require(pixel[0] == (patch ? 180 + x - 8 : 17) && pixel[1] == (patch ? 70 + y - 4 : 34) && pixel[2] == (patch ? 219 : 51 + frame) &&
                    pixel[3] == 255,
                "A later list sampled stale PFD pixels or the copy overwrote surrounding display pixels");
      }
    readback->Unmap(0, &none);
    const D3D12_RANGE lower_range{0, static_cast<SIZE_T>(lower_bytes)};
    success(lower_readback->Map(0, &lower_range, reinterpret_cast<void**>(&bytes)), "Map lower-mip preservation oracle");
    for (unsigned mip = 0; mip < lower_footprints.size(); ++mip) {
      const auto& footprint = lower_footprints[mip];
      for (UINT y = 0; y < footprint.Footprint.Height; ++y)
        for (UINT x = 0; x < footprint.Footprint.Width; ++x)
          for (UINT channel = 0; channel < 4; ++channel)
            require(bytes[footprint.Offset + y * footprint.Footprint.RowPitch + x * 4 + channel] == 81 + mip * 13 + channel,
                    "Base-subresource insertion changed a lower mip or its state");
    }
    lower_readback->Unmap(0, &none);
  }

  // A one-mip typeless display remains valid with an explicitly typed patch.
  Ref<ID3D12Resource> typeless;
  auto typeless_desc = desc;
  typeless_desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
  success(
      device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &typeless_desc, copy_state, nullptr, IID_PPV_ARGS(typeless.put())),
      "Create typeless display");
  auto typed_copy = valid;
  typed_copy.target = typeless.p;
  const auto typed_packet = context->pool.record(typed_copy);
  require(static_cast<bool>(typed_packet), "One-mip typeless display rejected an explicitly typed patch");
  context->pool.cancel(typed_packet.slot);
  context->pool.service(device.p, fence->GetCompletedValue());

  // Dropping the application's last reference must not retire an owned target
  // until an unsubmitted cancellation is serviced outside metadata locks.
  std::atomic<unsigned> retired{0};
  Ref<ID3D12Resource> lifetime_target;
  success(
      device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, copy_state, nullptr, IID_PPV_ARGS(lifetime_target.put())),
      "Create lifetime target");
  auto* probe = new DeathProbe(retired);
  success(lifetime_target->SetPrivateDataInterface(ProbeId, probe), "Attach lifetime oracle");
  probe->Release();
  auto lifetime_copy = valid;
  lifetime_copy.target = lifetime_target.p;
  const auto lifetime_packet = context->pool.record(lifetime_copy);
  require(static_cast<bool>(lifetime_packet), "Record resource lifetime proof");
  lifetime_target.reset();
  require(retired == 0, "Application release destroyed a retained packet target");
  context->pool.cancel(lifetime_packet.slot);
  require(retired == 0, "Submission-thread cancellation released resource lifetime");
  context->pool.service(device.p, fence->GetCompletedValue());
  require(retired == 1, "Worker did not release a safely cancelled packet target");
  const auto uncertain = context->pool.record(valid);
  require(static_cast<bool>(uncertain), "Record quarantine proof");
  context->pool.quarantine(uncertain.slot);
  context->pool.service(device.p, fence->GetCompletedValue());
  require(context->pool.ready_count() == Pool::Capacity - 1, "Worker recycled uncertain work");
  require(Queue::remove().status == Queue::Status::removed, "Remove fixture queue hook");

  // Both COM leases must also survive real queued GPU work after the caller
  // releases its references, then retire only once the covering fence passes.
  std::atomic<unsigned> gpu_retired{0};
  Ref<ID3D12Resource> held_target, held_source;
  success(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, copy_state, nullptr, IID_PPV_ARGS(held_target.put())),
          "Create submitted lifetime target");
  create_buffer(device.p, patch_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, held_source);
  success(held_source->Map(0, &none, reinterpret_cast<void**>(&bytes)), "Map submitted lifetime source");
  std::memset(bytes, 117, static_cast<std::size_t>(patch_bytes));
  held_source->Unmap(0, nullptr);
  for (auto* resource : {held_target.p, held_source.p}) {
    auto* lifetime = new DeathProbe(gpu_retired);
    success(resource->SetPrivateDataInterface(ProbeId, lifetime), "Attach submitted lifetime oracle");
    lifetime->Release();
  }
  auto held_copy = valid;
  held_copy.target = held_target.p;
  held_copy.source = held_source.p;
  const auto held_packet = context->pool.record(held_copy);
  require(static_cast<bool>(held_packet), "Record submitted resource leases");
  success(queue->Wait(blocker.p, 2), "Block submitted lifetime fixture");
  ID3D12CommandList* held_lists[]{held_packet.list};
  queue->ExecuteCommandLists(1, held_lists);
  success(queue->Signal(fence.p, 3), "Signal submitted lifetime receipt");
  context->pool.submit(held_packet.slot, 3);
  held_target.reset();
  held_source.reset();
  require(gpu_retired == 0 && fence->GetCompletedValue() == 2, "A native caller release retired in-flight packet resources");
  context->pool.service(device.p, fence->GetCompletedValue());
  require(gpu_retired == 0, "Worker released target or patch before the covering fence");
  success(blocker->Signal(2), "Release submitted lifetime fixture");
  complete(fence.p, 3);
  context->pool.service(device.p, fence->GetCompletedValue());
  require(gpu_retired == 2, "Completed submitted packet did not release both owned resource leases");
  require(context->pool.ready_count() == Pool::Capacity - 1, "A later completed fence falsely recycled quarantined work");
  success(device->GetDeviceRemovedReason(), "D3D12 device removed");
  if (diagnostics.p) {
    for (UINT64 i = 0; i < diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
      SIZE_T size{};
      diagnostics->GetMessage(i, nullptr, &size);
      std::array<std::byte, 4096> storage{};
      require(size <= storage.size(), "Debug message exceeded fixture bound");
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      success(diagnostics->GetMessage(i, message, &size), "Read D3D12 debug message");
      if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
        std::fprintf(stderr, "%s\n", message->pDescription);
      require(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, "D3D12 reported invalid inserted copy work");
    }
  }
  std::printf("PASS: %u PFD submission checks; 1024 actual sampled pixels, four lower mips unchanged (%s, %s, %s, debug layer %s).\n",
              checks, warp ? "WARP" : "hardware", common ? "COMMON" : "explicit SRV",
              before ? "before mixed list" : "after barrier-only list", debug_enabled ? "enabled" : "unavailable");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    bool warp = false, common = false, before = false;
    for (int i = 1; i < argc; ++i) {
      if (std::string_view(argv[i]) == "--warp")
        warp = true;
      else if (std::string_view(argv[i]) == "--common")
        common = true;
      else if (std::string_view(argv[i]) == "--before")
        before = true;
      else
        throw std::runtime_error("Unknown fixture option");
    }
    run(warp, common, before);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
