// Reuse device creation and GPU completion helpers; the existing entry is unused.
#undef WIN32_LEAN_AND_MEAN
#undef NOMINMAX
#define wmain existing_compositor_validation_entry
#include "compositor_main.cpp"
#undef wmain
#include "../../src/graphics/scene_runtime.hpp"

namespace {
namespace runtime = taxi_camera::scene_runtime;
void frame_order_case(bool warp) {
  using namespace taxi_camera;
  Reference<ID3D12Debug> debug;
  const bool debug_enabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())));
  if (debug_enabled)
    debug->EnableDebugLayer();
  Reference<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "Frame-order factory");
  Reference<IDXGIAdapter> adapter;
  if (warp)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "Frame-order WARP");
  Reference<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.put())), "Frame-order device");
  Reference<ID3D12InfoQueue> messages;
  if (debug_enabled)
    check(device->QueryInterface(IID_PPV_ARGS(messages.put())), "Frame-order debug messages");
  constexpr std::uint64_t key = 9101;
  require(runtime::init_device(key, device.get()) && runtime::prepare(key), "Frame-order actual runtime");
  auto& manager = runtime::manager();
  auto& handoff = scene_handoff();
  require(handoff.register_device(key) != 0, "Frame-order handoff device");
  Reference<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC qd{};
  check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.put())), "Frame-order queue");
  Reference<ID3D12DescriptorHeap> heap;
  const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(heap.put())), "Frame-order RTVs");
  std::array<Reference<ID3D12Resource>, 2> sources;
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> rtvs{};
  std::array<std::uint64_t, 2> handles{};
  for (UINT feed = 0; feed < 2; ++feed) {
    const auto pane = profiles::A380.camera_panes[feed];
    create_texture(device.get(), texture_description(pane[0], pane[1], DXGI_FORMAT_R8G8B8A8_UNORM), sources[feed].put());
    handles[feed] = reinterpret_cast<std::uint64_t>(sources[feed].get());
    require(handoff.register_resource(key, handles[feed], 501 + feed), "Frame-order resource identity");
    rtvs[feed] = {heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{feed} * device->GetDescriptorHandleIncrementSize(hd.Type)};
    device->CreateRenderTargetView(sources[feed].get(), nullptr, rtvs[feed]);
  }
  handoff.begin_scene();
  require(handoff.publish(handoff.begin_capture(), {73, 1}, {101, 102}, handles), "Frame-order publication");
  struct Recording {
    Reference<ID3D12CommandAllocator> allocator;
    Reference<ID3D12GraphicsCommandList> list;
    std::uint64_t generation = 0;
  };
  std::array<Recording, 24> recordings;
  UINT next = 0;
  const auto record = [&](UINT feed) -> Recording& {
    require(next < recordings.size(), "Bounded frame-order recordings");
    auto& item = recordings[next++];
    item.generation = next;
    check(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(item.allocator.put())), "Frame-order allocator");
    check(device->CreateCommandList(0, qd.Type, item.allocator.get(), nullptr, IID_PPV_ARGS(item.list.put())), "Frame-order list");
    require(manager.register_command_list(item.list.get(), key, item.generation), "Frame-order recording registration");
    const float color[]{.1f, .2f, next / 32.f, 1};
    item.list->ClearRenderTargetView(rtvs[feed], color, 0, nullptr);
    require(manager.record_render_target_before_transition(item.list.get(), sources[feed].get(), true, item.generation),
            "Frame-order source capture");
    transition(item.list.get(), sources[feed].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(item.list.get(), sources[feed].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    check(item.list->Close(), "Frame-order Close");
    return item;
  };
  const auto submit = [&](std::initializer_list<Recording*> batch) {
    std::array<ID3D12CommandList*, 4> lists{};
    UINT count = 0;
    for (auto* item : batch)
      lists[count++] = item->list.get();
    const auto receipt = manager.before_submission(queue.get(), count, lists.data());
    require(receipt != 0, "Frame-order exact submission receipt");
    queue->ExecuteCommandLists(count, lists.data());
    manager.after_submission(queue.get(), receipt);
    require(drain_copy_queue(queue.get(), device.get()), "Frame-order producer completion");
  };
  const auto retire = [&](Recording& item) { manager.destroy_command_list(item.list.get(), item.generation); };
  const auto service = [&] {
    // All application work has completed; retirement signals and compositions
    // still use the real manager fences and private queue, without test bypasses.
    for (unsigned pass = 0; pass < 3; ++pass) {
      // Join the private compositor's timeline before polling. No arbitrary
      // sleep or fixed GPU-performance assumption controls this regression.
      const auto flush = manager.begin_private_submission(key, queue.get());
      require(flush.receipt && manager.end_private_submission(flush.receipt), "Frame-order timeline join");
      require(drain_copy_queue(queue.get(), device.get()), "Frame-order retirement and consumption completion");
      runtime::service();
    }
    const auto status = runtime::snapshot(key);
    require(!status.failed && !status.capture.quarantined, "Frame-order lifecycle remains healthy");
    return status;
  };
  auto& late = record(0);
  submit({&late});  // Still replayable, so this older image cannot be leased yet.
  auto& nose = record(0);
  auto& tail = record(1);
  submit({&nose, &tail});
  retire(nose);
  retire(tail);
  require(service().frames == 1, "Fresh pair composes while older recording remains executable");
  retire(late);
  auto& next_tail = record(1);
  submit({&next_tail});
  retire(next_tail);
  const auto delayed = service();
  require(delayed.capture.completed == 4, "Late retirement and next tail both reach the actual runtime");
  require(delayed.frames == 1, "Older nose image must not compose after a newer nose was displayed");
  require(delayed.stale_frames == 1, "Delayed image rejection is observable in the runtime log counter");
  auto& next_nose = record(0);
  submit({&next_nose});
  retire(next_nose);
  require(service().frames == 2, "Next genuinely fresh nose completes the pending pair");

  // Lower buffer slot and lower allocation token execute later. Both are ready
  // in one poll, so the last array entry must not replace the newer image.
  auto& low_slot = record(0);
  auto& high_slot = record(0);
  auto& reverse_tail = record(1);
  submit({&high_slot});
  submit({&low_slot, &reverse_tail});
  retire(low_slot);
  retire(high_slot);
  retire(reverse_tail);
  const auto reversed = service();
  require(reversed.frames == 3 && reversed.stale_frames == 2, "Submission order wins over recycled buffer/token order");

  auto& batch_low = record(0);
  auto& batch_high = record(0);
  auto& batch_tail = record(1);
  submit({&batch_high, &batch_low, &batch_tail});
  retire(batch_low);
  retire(batch_high);
  retire(batch_tail);
  const auto batched = service();
  require(batched.frames == 4 && batched.stale_frames == 3, "Exact command-list order breaks ties within one GPU submission");

  auto& replay = record(0);
  submit({&replay});
  auto& middle_nose = record(0);
  auto& middle_tail = record(1);
  submit({&middle_nose, &middle_tail});
  retire(middle_nose);
  retire(middle_tail);
  require(service().frames == 5, "A later recording can display before an older recording is replayed");
  submit({&replay});
  retire(replay);
  auto& replay_tail = record(1);
  submit({&replay_tail});
  retire(replay_tail);
  const auto replayed = service();
  require(replayed.frames == 6 && replayed.stale_frames == 3, "Replayed capture uses its latest submission, not its original token");

  auto& obsolete = record(0);
  submit({&obsolete});
  handoff.stop_scene();
  runtime::reset_feed(key);
  handoff.begin_scene();
  require(handoff.publish(handoff.begin_capture(), {73, 1}, {101, 102}, handles), "Frame-order new scene on retained resources");
  retire(obsolete);
  auto& scene_nose = record(0);
  auto& scene_tail = record(1);
  submit({&scene_nose, &scene_tail});
  retire(scene_nose);
  retire(scene_tail);
  const auto restarted = service();
  require(restarted.frames == 7 && restarted.stale_frames == 3, "Scene identity rejects obsolete captures and admits the fresh pair");
  handoff.stop_scene();
  runtime::reset_feed(key);
  UINT64 errors = 0;
  if (messages.get())
    for (UINT64 index = 0; index < messages->GetNumStoredMessages(); ++index) {
      SIZE_T size = 0;
      check(messages->GetMessage(index, nullptr, &size), "Frame-order debug message size");
      std::vector<unsigned char> bytes(size);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
      check(messages->GetMessage(index, message, &size), "Frame-order debug message");
      if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
        ++errors;
        std::fprintf(stderr, "D3D12 error %u: %s\n", message->ID, message->pDescription);
      }
    }
  require(errors == 0, "Frame-order GPU debug validation");
  std::printf(
      "PASS frame order %s: delayed retirement, recycled slots, batch order, replay and scene restart; frames=7 stale=3 debug=%d "
      "errors=%llu.\n",
      warp ? "WARP" : "hardware", debug_enabled, errors);
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc > 2 || (argc == 2 && std::wcscmp(argv[1], L"--warp") != 0))
      return 2;
    frame_order_case(argc == 2);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL frame order: %s\n", error.what());
    return 1;
  }
}
