// Reuse the existing GPU harness, with a separate entry point and receipts.
#define main existing_manager_validation_entry
#include "scene_capture_manager_test.cpp"
#undef main
#include <future>
#include "../../src/hooks/queue_submit_observer.hpp"
#include "../../src/graphics/native_device_identity.hpp"

namespace {
namespace submission_lock_fixture {
struct Device {
  void** table;
};
struct Queue {
  void** table;
  Device* device;
  std::uint64_t last_signal = 0;
  unsigned waits = 0, signals = 0;
  bool future_wait = false;
};
HRESULT STDMETHODCALLTYPE identity(void* self, REFIID, void** result) {
  *result = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE reference(void*) {
  return 1;
}
HRESULT STDMETHODCALLTYPE get_device(Queue* queue, REFIID, void** result) {
  *result = queue->device;
  return S_OK;
}
// The Windows x64 COM member ABI passes the aggregate-return address after
// the this pointer; a free function returning the struct would reverse them.
D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE description(Queue*, D3D12_COMMAND_QUEUE_DESC* result) {
  *result = {};
  result->Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  return result;
}
HRESULT STDMETHODCALLTYPE signal(Queue* queue, ID3D12Fence*, std::uint64_t value) {
  queue->last_signal = value;
  ++queue->signals;
  return S_OK;
}
HRESULT STDMETHODCALLTYPE wait_on(Queue* queue, ID3D12Fence*, std::uint64_t value) {
  queue->future_wait |= value > queue->last_signal;
  ++queue->waits;
  return S_OK;
}
struct Discovery {
  Manager* manager;
  std::atomic<bool> called{false};
};
void discover(void* opaque, ID3D12GraphicsCommandList*, std::uint64_t) noexcept {
  auto& context = *static_cast<Discovery*>(opaque);
  context.manager->statistics();  // Discovery must not retain the metadata lock either.
  context.called.store(true, std::memory_order_release);
}
void run() {
  void* device_table[]{reinterpret_cast<void*>(&identity), reinterpret_cast<void*>(&reference), reinterpret_cast<void*>(&reference)};
  Device device{device_table};
  std::array<void*, 19> queue_table{};
  queue_table[7] = reinterpret_cast<void*>(&get_device);
  queue_table[14] = reinterpret_cast<void*>(&signal);
  queue_table[15] = reinterpret_cast<void*>(&wait_on);
  queue_table[18] = reinterpret_cast<void*>(&description);
  Queue queue{queue_table.data(), &device};
  auto* native_queue = reinterpret_cast<ID3D12CommandQueue*>(&queue);
  taxi_camera::SceneHandoff handoff;
  auto manager = std::make_unique<Manager>(handoff);
  auto& owner = manager->devices_[0];
  owner.key = 7;
  owner.native = reinterpret_cast<ID3D12Device*>(&device);
  owner.active = true;
  // The fixture uses a recording sink, never a D3D12 device or GPU submission.
  std::array<std::uintptr_t, 2> markers{};
  auto* known = reinterpret_cast<ID3D12GraphicsCommandList*>(&markers[0]);
  auto* unknown = reinterpret_cast<ID3D12GraphicsCommandList*>(&markers[1]);
  auto& recording = manager->lists_[0];
  recording.native = known;
  recording.device_key = 7;
  recording.object_generation = 19;
  manager->list_indices_.emplace(known, 0);
  Discovery discovery{manager.get()};
  require(manager->set_unknown_list_observer(discover, &discovery), "Install CPU-only discovery callback");
  const auto blocked = [&](ID3D12GraphicsCommandList* list, bool should_bypass, bool expect_receipt, bool expect_discovery) {
    discovery.called.store(false, std::memory_order_release);
    std::unique_lock held(manager->submission_mutex_);
    std::promise<void> entered;
    auto started = entered.get_future();
    auto completed = std::async(std::launch::async, [&] {
      ID3D12CommandList* batch[]{list};
      entered.set_value();
      const auto receipt = manager->before_submission(native_queue, 1, batch);
      if (receipt)
        manager->after_submission(native_queue, receipt);
      return receipt;
    });
    started.wait();
    const bool returned_while_locked = completed.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
    const bool discovered_while_locked = discovery.called.load(std::memory_order_acquire);
    held.unlock();  // Always release before checking, including regression failure.
    const auto receipt = completed.get();
    require(returned_while_locked == should_bypass, "Only known unrelated recordings bypass held submission serialization");
    require((receipt != 0) == expect_receipt, "Related recordings retain exact transaction admission");
    require(discovered_while_locked == expect_discovery, "Unknown discovery runs before acquiring submission serialization");
  };
  blocked(known, true, false, false);
  require(!queue.signals && !queue.waits, "Unrelated submission queues no timeline operations");
  recording.consumer = true;
  blocked(known, false, true, false);
  recording.consumer = false;
  recording.source_touched = true;
  blocked(known, false, true, false);
  recording.source_touched = false;
  recording.packets = 1;
  blocked(known, false, true, false);
  recording.packets = 0;
  require(queue.signals == 3 && queue.waits == 2 && !queue.future_wait && owner.last_signal == 3,
          "Consumer/source/capture paths preserve ordered Wait and Signal receipts");
  recording.awaiting_native_reset = true;
  blocked(known, false, false, false);
  recording.awaiting_native_reset = false;
  const taxi_camera::source_state::Key source{0x1234, 1};
  require(owner.source_states.register_source(source, taxi_camera::source_state::Model::legacy_rt), "Seed ordered source-state evidence");
  blocked(unknown, false, false, true);
  require(owner.source_states.state(source).model == taxi_camera::source_state::Model::unknown &&
              manager->statistics().unknown_submitted_lists == 2,
          "Unknown and unobserved submissions retain conservative source invalidation");
  require(queue.signals == 3 && queue.waits == 2, "Unobserved recordings never invent a receipt");
  std::printf("PASS CPU-only submission locks: unrelated bypass, discovery lock order, conservative guards and ordered receipts.\n");
}
}  // namespace submission_lock_fixture
struct TailContext {
  Manager* manager;
};
class SourceLifetimeMarker final : public IUnknown {
 public:
  explicit SourceLifetimeMarker(std::atomic<unsigned>& deaths) noexcept : deaths_(deaths) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
    if (!result)
      return E_POINTER;
    *result = nullptr;
    if (iid != __uuidof(IUnknown))
      return E_NOINTERFACE;
    *result = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = --references_;
    if (!remaining) {
      ++deaths_;
      delete this;
    }
    return remaining;
  }
 private:
  std::atomic<ULONG> references_{1};
  std::atomic<unsigned>& deaths_;
};
struct TailQueueContext {
  Manager* manager = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12CommandAllocator* replacement = nullptr;
  std::array<Ref<ID3D12Resource>, 2>* sources = nullptr;
  std::atomic<unsigned> source_deaths{0};
  std::uint64_t generation = 0;
  bool reset_after_execute = false, receipt_lease_proven = false, reset_failed = false;
};
std::uint64_t tail_before(void* opaque, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept {
  return static_cast<TailQueueContext*>(opaque)->manager->before_submission(queue, count, lists);
}
void tail_after(void* opaque, ID3D12CommandQueue* queue, std::uint64_t receipt) noexcept {
  auto& context = *static_cast<TailQueueContext*>(opaque);
  std::array<ID3D12Resource*, 2> borrowed{};
  if (context.reset_after_execute) {
    context.reset_after_execute = false;
    // Reset the command list with a DIFFERENT allocator after native Execute
    // returned, while the old allocator and immutable GPU recording stay live.
    // This retires the recording's leases before manager after-submit begins.
    if (FAILED(context.list->Reset(context.replacement, nullptr)) || FAILED(context.list->Close())) {
      context.reset_failed = true;
    } else {
      context.manager->successful_reset(context.list, context.generation);
      Boundary::successful_reset(context.list, context.generation);
      for (std::size_t index = 0; index < borrowed.size(); ++index) {
        borrowed[index] = (*context.sources)[index].p;
        (*context.sources)[index].p = nullptr;
        borrowed[index]->Release();
      }
      context.receipt_lease_proven = context.source_deaths.load() == 0;
      if (!context.receipt_lease_proven)
        context.manager->stop_source_tracking();  // Refuse dereference if regression freed sources.
    }
  }
  context.manager->after_submission(queue, receipt);
  if (context.receipt_lease_proven)
    for (std::size_t index = 0; index < borrowed.size(); ++index)
      if (borrowed[index]) {
        // Actual tail packets now own these same sources through GPU completion.
        borrowed[index]->AddRef();
        (*context.sources)[index].p = borrowed[index];
      }
}
void tail_refused(void* opaque, ID3D12CommandQueue* queue, taxi_camera::engine_hook::queue_submit::Refusal reason) noexcept {
  static_cast<TailQueueContext*>(opaque)->manager->submission_refused(queue, reason);
}
void tail_legacy(void* context, ID3D12GraphicsCommandList* list, std::uint64_t generation,
                 const D3D12_RESOURCE_BARRIER& value, std::uint32_t) noexcept {
  static_cast<TailContext*>(context)->manager->observe_source_legacy(list, generation, value);
}
void tail_enhanced(void* context, ID3D12GraphicsCommandList7* list, std::uint64_t generation,
                   const D3D12_TEXTURE_BARRIER& value, std::uint32_t) noexcept {
  static_cast<TailContext*>(context)->manager->observe_source_enhanced(list, generation, value);
}
void tail_draw(void* context, ID3D12GraphicsCommandList* list, std::uint64_t generation, bool allowed) noexcept {
  static_cast<TailContext*>(context)->manager->after_source_draw(list, generation, allowed);
}
void tail_run(bool warp_requested, bool enhanced, bool born_render_target) {
  Ref<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "Create tail factory");
  Ref<IDXGIAdapter> warp;
  if (warp_requested)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.put())), "Get tail WARP");
  Ref<ID3D12Device> device;
  check(D3D12CreateDevice(warp.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "Create tail device");
  if (enhanced) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
    check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options)), "Query enhanced barrier support");
    require(options.EnhancedBarriersSupported, "Enhanced barriers unavailable");
  }
  auto* handoff = new taxi_camera::SceneHandoff;
  auto* manager = new Manager(*handoff);  // Registered contexts live until exit.
  constexpr std::uint64_t DeviceKey = 55, Generation = 707;
  handoff->register_device(DeviceKey);
  require(manager->register_device(DeviceKey, device.p), "Register tail device");
  {
    // Test-only access validates command-object ownership independently of the
    // GPU snapshot key, which ordinary copy capture can legitimately replace.
    Ref<ID3D12Device> other_device;
    check(D3D12CreateDevice(warp.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(other_device.put())), "Create other tail device");
    require(manager->register_device(DeviceKey + 1, other_device.p), "Register other tail device");
    auto& packet = manager->packets_.back();
    require(manager->prepare_tail(packet, *manager->device(DeviceKey)), "Prepare first-device private command objects");
    check(packet.tail_list->Close(), "Close unused first-device tail list");
    packet.device_key = DeviceKey + 1;  // Ordinary snapshot reuse changes ONLY this identity.
    require(manager->prepare_tail(packet, *manager->device(DeviceKey + 1)), "Rebuild other-device tail command objects");
    require(taxi_camera::same_native_device(packet.tail_allocator, other_device.p) &&
                taxi_camera::same_native_device(packet.tail_list, other_device.p),
            "Tail allocator/list retained the snapshot's previous device");
    check(packet.tail_list->Close(), "Close unused other-device tail list");
  }
  require(manager->source_rate_ == 15, "Capture rate default remains15");
  for (const auto setting : std::array<std::array<std::uint32_t, 2>, 8>{
           {{0, 15}, {14, 15}, {20, 20}, {21, 21}, {30, 30}, {60, 60}, {61, 60}, {0xffffffffu, 60}}}) {
    manager->set_source_rate(setting[0]);
    require(manager->source_rate_ == setting[1], "Capture rate follows15..60 clamp rather than former20fps ceiling");
  }
  manager->set_source_rate(20);
  Commands producer, second_queue, consumer, unknown;
  producer.initialize(device.p);
  second_queue.initialize(device.p);
  consumer.initialize(device.p);
  unknown.initialize(device.p);
  require(manager->register_command_list(producer.list.p, DeviceKey, Generation), "Register tail producer");
  Ref<ID3D12GraphicsCommandList7> list7;
  check(producer.list->QueryInterface(IID_PPV_ARGS(list7.put())), "Get producer interface7");
  auto* context = new TailContext{manager};
  Boundary::Callbacks callbacks{};
  callbacks.context = context;
  callbacks.before_legacy = [](void*, ID3D12GraphicsCommandList*, std::uint64_t, const D3D12_RESOURCE_TRANSITION_BARRIER&) noexcept {};
  callbacks.before_enhanced = [](void*, ID3D12GraphicsCommandList7*, std::uint64_t, const D3D12_TEXTURE_BARRIER&) noexcept {};
  callbacks.observe_legacy = tail_legacy;
  callbacks.observe_enhanced = tail_enhanced;
  callbacks.after_draw = tail_draw;
  callbacks.recording_invalidated = [](void* raw, ID3D12GraphicsCommandList* native, std::uint64_t generation, std::uint32_t reasons) noexcept {
    static_cast<TailContext*>(raw)->manager->invalidate_source_recording(native, generation, true, reasons);
  };
  require(Boundary::register_list(producer.list.p, Generation, callbacks).ready, "Register actual native draw/barrier observer");
  namespace Queue = taxi_camera::engine_hook::queue_submit;
  auto* queue_context = new TailQueueContext;
  queue_context->manager = manager;
  queue_context->list = producer.list.p;
  queue_context->generation = Generation;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&queue_context->replacement)),
        "Create concurrent Reset replacement allocator");
  const Queue::Callbacks queue_callbacks{queue_context, tail_before, tail_after, tail_refused};
  require(Queue::register_queue(producer.queue.p, queue_callbacks).hook_installed, "Register producer submission observer");
  require(Queue::register_queue(second_queue.queue.p, queue_callbacks).hook_installed, "Register second producer queue");

  Ref<ID3D12Fence> completed;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(completed.put())), "Create tail test fence");
  std::uint64_t completed_value = 0;
  const auto drain = [&](ID3D12CommandQueue* queue) {
    check(queue->Signal(completed.p, ++completed_value), "Signal tail test completion");
    wait([&] { return completed->GetCompletedValue() >= completed_value; });
  };
  Ref<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = 2;
  check(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(rtvs.put())), "Create tail RTV heap");
  const auto rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  std::array<Ref<ID3D12Resource>, 2> sources, readbacks;
  queue_context->sources = &sources;
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> handles{};
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprints{};
  std::array<UINT64, 2> readback_bytes{};
  std::array<std::uint64_t, 2> ids{29265, 29266};
  const std::array<UINT, 2> heights{251, 496};
  constexpr UINT width = 736;
  Ref<ID3D12Resource> unrelated_source;
  constexpr std::uint64_t UnrelatedGeneration = 929292;
  for (unsigned feed = 0; feed < 2; ++feed) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = heights[feed];
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto default_heap = heap(D3D12_HEAP_TYPE_DEFAULT);
    if (born_render_target && enhanced) {
      Ref<ID3D12Device10> device10;
      check(device->QueryInterface(IID_PPV_ARGS(device10.put())), "Get layout-based resource creation interface");
      D3D12_RESOURCE_DESC1 desc1{};
      desc1.Dimension = desc.Dimension;
      desc1.Width = desc.Width;
      desc1.Height = desc.Height;
      desc1.DepthOrArraySize = desc.DepthOrArraySize;
      desc1.MipLevels = desc.MipLevels;
      desc1.SampleDesc = desc.SampleDesc;
      desc1.Format = desc.Format;
      desc1.Flags = desc.Flags;
      check(device10->CreateCommittedResource3(&default_heap, D3D12_HEAP_FLAG_NONE, &desc1, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                                               nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(sources[feed].put())),
            "Create persistent source directly in enhanced RT layout");
    } else {
      check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            born_render_target ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COMMON,
                                            nullptr, IID_PPV_ARGS(sources[feed].put())), "Create persistent tail source");
    }
    auto* marker = new SourceLifetimeMarker(queue_context->source_deaths);
    const GUID lifetime_id{0xe7bd8641, 0xea37, 0x451c, {0x8f, 0x64, 0x46, 0xe4, 0x8a, 0x39, 0x22, 0x80}};
    check(sources[feed]->SetPrivateDataInterface(lifetime_id, marker), "Attach source destruction witness");
    marker->Release();
    handoff->register_resource(DeviceKey, reinterpret_cast<std::uint64_t>(sources[feed].p), ids[feed]);
    const auto initial_model = !born_render_target ? taxi_camera::source_state::Model::unknown
                               : enhanced ? taxi_camera::source_state::Model::enhanced_rt : taxi_camera::source_state::Model::legacy_rt;
    require(manager->register_source_candidate(DeviceKey, sources[feed].p, ids[feed], desc, initial_model), "Register exact pane candidate");
    handles[feed].ptr = rtvs->GetCPUDescriptorHandleForHeapStart().ptr + feed * rtv_stride;
    device->CreateRenderTargetView(sources[feed].p, nullptr, handles[feed]);
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprints[feed], nullptr, nullptr, &readback_bytes[feed]);
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = readback_bytes[feed];
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto readback_heap = heap(D3D12_HEAP_TYPE_READBACK);
    check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(readbacks[feed].put())), "Create tail test readback");
    // This actual initial barrier runs BEFORE publication and capture enable.
    if (born_render_target) {
      // No transition is emitted anywhere before these resources' first draw.
      // The native creation call above is the only positive initial model.
    } else if (enhanced)
      enhanced_barrier(list7.p, sources[feed].p, D3D12_BARRIER_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                       D3D12_BARRIER_ACCESS_COMMON, D3D12_BARRIER_ACCESS_RENDER_TARGET);
    else
      barrier(producer.list.p, sources[feed].p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  {
    const auto desc = sources[0]->GetDesc();
    const auto default_heap = heap(D3D12_HEAP_TYPE_DEFAULT);
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          nullptr, IID_PPV_ARGS(unrelated_source.put())), "Create unrelated pane-sized pass target");
    require(manager->register_source_candidate(DeviceKey, unrelated_source.p, UnrelatedGeneration, desc,
                                               taxi_camera::source_state::Model::legacy_rt), "Register unrelated pass target");
  }
  check(producer.list->Close(), "Close initial state recording");
  ID3D12CommandList* original = producer.list.p;
  producer.queue->ExecuteCommandLists(1, &original);
  drain(producer.queue.p);
  require(manager->statistics().tail_captures == 0, "Prepublication state observation captured pixels");
  handoff->begin_scene();
  const auto ticket = handoff->begin_capture();
  require(handoff->publish(ticket, {44, 2}, {9001, 9002},
                           {reinterpret_cast<std::uint64_t>(sources[0].p), reinterpret_cast<std::uint64_t>(sources[1].p)}),
          "Publish tail pair");
  manager->begin_source_tracking();
  DrawFixture draw;
  draw.initialize(device.p, DXGI_FORMAT_R11G11B10_FLOAT);
  constexpr float colors[2][2][4] = {
      {{0.25f, 0.5f, 0.75f, 1}, {2, 4, 0.125f, 1}}, {{1, 0, 0.5f, 1}, {0.125f, 1, 2, 1}}};
  constexpr std::array<std::array<std::uint32_t, 2>, 2> words{{
      {0x340u | (0x380u << 11) | (0x1d0u << 22), 0x400u | (0x440u << 11) | (0x180u << 22)},
      {0x3c0u | (0x1c0u << 22), 0x300u | (0x3c0u << 11) | (0x200u << 22)}}};
  std::uint64_t checked_pixels = 0;
  for (unsigned frame = 0; frame < 3; ++frame) {
    Sleep(70);  // Production15..20Hz cap also applies to this real GPU fixture.
    if (frame < 2) {
      check(producer.allocator->Reset(), "Reset completed producer allocator");
      check(producer.list->Reset(producer.allocator.p, nullptr), "Reset completed producer list");
      manager->successful_reset(producer.list.p, Generation);
      Boundary::successful_reset(producer.list.p, Generation);
      ID3D12Resource* unrelated = unrelated_source.p;
      manager->invalidate_source_targets(producer.list.p, Generation, 1, &unrelated, &UnrelatedGeneration);
      // Wrong list/resource incarnations must not invalidate a current pane.
      ID3D12Resource* selected = sources[0].p;
      const std::uint64_t stale_generation = ids[0] + 100000;
      manager->invalidate_source_targets(producer.list.p, Generation + 1, 1, &selected, &ids[0]);
      manager->invalidate_source_targets(producer.list.p, Generation, 1, &selected, &stale_generation);
      for (unsigned feed = 0; feed < 2; ++feed) {
        ID3D12Resource* target = sources[feed].p;
        const float first_color[]{1, 0, 1, 1};
        manager->stage_source_draw(producer.list.p, Generation, 1, &target, &ids[feed]);
        draw.record(list7.p, handles[feed], first_color, false, width, heights[feed]);
        manager->stage_source_draw(producer.list.p, Generation, 1, &target, &ids[feed]);
        draw.record(list7.p, handles[feed], colors[frame][feed], false, width, heights[feed]);
      }
      if (frame == 1 && !enhanced) {
        // A legal large application batch between two live frames must not
        // erase the ordered RT evidence for untouched camera sources.
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        std::vector<D3D12_RESOURCE_BARRIER> large_batch(4097, uav);
        producer.list->ResourceBarrier(static_cast<UINT>(large_batch.size()), large_batch.data());
        require(!manager->list(producer.list.p)->source_effects.invalid, "Large unrelated batch froze live capture");
      }
      // No source transition at all in these recordings: resources stay RT.
      check(producer.list->Close(), "Close persistent RT draw recording");
    }
    auto* queue = frame == 1 ? second_queue.queue.p : producer.queue.p;
    queue_context->reset_after_execute = frame == 0;
    queue->ExecuteCommandLists(1, &original);  // Frame2 replays exact closed frame1.
    require(manager->device(DeviceKey)->source_states.state({reinterpret_cast<std::uint64_t>(unrelated_source.p), UnrelatedGeneration}).model ==
                taxi_camera::source_state::Model::other,
            "Scoped target invalidation was not applied to its exact source");
    require(!queue_context->reset_failed && queue_context->receipt_lease_proven,
            "Receipt failed to retain actual sources across Reset and application reference release");
    std::array<Manager::Frame, 2> frames{};
    std::size_t frame_count = 0;
    wait([&] { frame_count += manager->poll_completed_frames(frames.data() + frame_count, frames.size() - frame_count); return frame_count == 2; });
    if (frame) {
      check(consumer.allocator->Reset(), "Reset completed consumer allocator");
      check(consumer.list->Reset(consumer.allocator.p, nullptr), "Reset consumer list");
    }
    for (const auto& result : frames) {
      const auto feed = result.match.feed;
      require(feed < 2, "Bad tail feed identity");
      barrier(consumer.list.p, result.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION from{}, to{};
      from.pResource = result.resource;
      from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      to.pResource = readbacks[feed].p;
      to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      to.PlacedFootprint = footprints[feed];
      consumer.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      barrier(consumer.list.p, result.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    check(consumer.list->Close(), "Close tail consumer");
    const auto consume = manager->begin_private_submission(DeviceKey, consumer.queue.p);
    require(consume.receipt != 0, "Begin synchronized tail consumer");
    ID3D12CommandList* read = consumer.list.p;
    consumer.queue->ExecuteCommandLists(1, &read);
    require(manager->end_private_submission(consume.receipt), "Submit synchronized tail consumer");
    for (const auto& result : frames)
      require(manager->finish_consumption(result.token, consume.fence, consume.value), "Finish tail consumer lease");
    drain(consumer.queue.p);
    for (unsigned feed = 0; feed < 2; ++feed) {
      void* mapped = nullptr;
      const D3D12_RANGE range{0, static_cast<SIZE_T>(readback_bytes[feed])};
      check(readbacks[feed]->Map(0, &range, &mapped), "Map completed tail pixels");
      for (UINT y = 0; y < heights[feed]; ++y)
        for (UINT x = 0; x < width; ++x) {
          std::uint32_t pixel = 0;
          std::memcpy(&pixel, static_cast<const std::uint8_t*>(mapped) + footprints[feed].Offset +
                               UINT64(y) * footprints[feed].Footprint.RowPitch + x * 4, 4);
          require(pixel == words[frame == 0 ? 0 : 1][feed], "Tail snapshot is not the last actual draw's packed RGB");
          ++checked_pixels;
        }
      const D3D12_RANGE empty{0, 0};
      readbacks[feed]->Unmap(0, &empty);
    }
  }
  require(manager->statistics().tail_captures == 6, "Unexpected persistent/replay tail captures");
  // GPU completion and the pixel walk above may take longer than one interval
  // on a hosted runner. Validate capture timestamps rather than assuming this
  // replay is still immediate. A burst checks any additional captures per feed.
  for (unsigned replay = 0; replay < 8; ++replay) {
    if (replay == 4)
      Sleep(70);  // Also exercise a replay legitimately due after one interval.
    const auto previous = manager->last_tail_us_;
    const auto before = manager->statistics().tail_captures;
    producer.queue->ExecuteCommandLists(1, &original);
    unsigned changed = 0;
    for (unsigned feed = 0; feed < 2; ++feed) {
      const auto captured = manager->last_tail_us_[feed];
      if (captured != previous[feed]) {
        ++changed;
        require(captured > previous[feed] && captured - previous[feed] >= 66667, "Tail capture exceeded the configured 15 Hz interval");
      }
    }
    require(manager->statistics().tail_captures == before + changed, "Tail capture counts disagree with per-feed timestamps");
  }
  drain(producer.queue.p);
  const auto rate_checked_captures = manager->statistics().tail_captures;
  require(rate_checked_captures > 6, "Delayed replay did not exercise an additional capture interval");
  // Legitimate delayed captures own GPU packets too. Retire these unused
  // frames before asserting that shutdown releases all source references.
  std::array<Manager::Frame, Manager::MaximumPackets> unused{};
  const auto unused_count = manager->poll_completed_frames(unused.data(), unused.size());
  for (std::size_t i = 0; i < unused_count; ++i)
    require(manager->discard_frame(unused[i].token), "Discard unused rate-check frame");
  require(manager->poll_completed_frames(unused.data(), unused.size()) == 0, "Rate-check frame remained unretired");
  // Unknown actual recordings must destroy global model proof, even if empty.
  check(unknown.list->Close(), "Close unknown list");
  ID3D12CommandList* unregistered = unknown.list.p;
  producer.queue->ExecuteCommandLists(1, &unregistered);
  Sleep(70);
  producer.queue->ExecuteCommandLists(1, &original);
  drain(producer.queue.p);
  require(manager->statistics().tail_captures == rate_checked_captures, "Unregistered submission left stale state proof usable");
  require(Boundary::remove().protection_restored, "Remove boundary observation");
  producer.queue->ExecuteCommandLists(1, &original);
  drain(producer.queue.p);
  require(manager->statistics().tail_captures == rate_checked_captures &&
              std::strcmp(manager->statistics().tail_status, "observer_disabled") == 0,
          "Disabled observer left prior immutable recording eligible");
  manager->stop_source_tracking();
  require(queue_context->source_deaths.load() == 0, "A source died while the application still owned it");
  for (auto& source : sources) {
    source.p->Release();
    source.p = nullptr;
  }
  require(queue_context->source_deaths.load() == 2, "Stop left candidate-registry or recording source references pinned");
  require(SUCCEEDED(device->GetDeviceRemovedReason()), "Tail GPU work removed device");
  std::printf(
      "{\"passed\":true,\"warp\":%s,\"enhanced\":%s,\"born_render_target\":%s,\"checked_pixels\":%llu,\"tail_captures\":%llu,\"replayed\":"
      "true,"
      "\"two_producer_queues\":true,\"persistent_rt\":true,\"reset_receipt_lease\":true,\"stop_releases_sources\":true,"
      "\"tail_device_reuse\":true,\"scoped_target_invalidation\":true,\"checks\":%u}\n",
      warp_requested ? "true" : "false", enhanced ? "true" : "false", born_render_target ? "true" : "false",
      static_cast<unsigned long long>(checked_pixels), static_cast<unsigned long long>(rate_checked_captures), checks);
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::strcmp(argv[1], "--submission-locks") == 0) {
      submission_lock_fixture::run();
      return 0;
    }
    bool warp = false, enhanced = false, born_render_target = false;
    for (int n = 1; n < argc; ++n) {
      if (std::strcmp(argv[n], "--warp") == 0 && !warp)
        warp = true;
      else if (std::strcmp(argv[n], "--enhanced") == 0 && !enhanced)
        enhanced = true;
      else if (std::strcmp(argv[n], "--born-render-target") == 0 && !born_render_target)
        born_render_target = true;
      else
        require(false, "Usage: scene-queue-tail-test [--warp] [--enhanced] [--born-render-target]");
    }
    tail_run(warp, enhanced, born_render_target);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
