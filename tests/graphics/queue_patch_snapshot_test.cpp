#include "../support/graphics_fixture.hpp"
// Inspect contention/lifetime state while exercising the actual service entry.
// Exclude the ordinary scene_runtime object when linking this test.
#include "../../src/graphics/scene_runtime.cpp"

#include <chrono>
#include <future>

namespace {
using namespace taxi_camera;
using namespace taxi_camera::testing;
namespace runtime = taxi_camera::scene_runtime;
struct Recording {
  Reference<ID3D12CommandAllocator> allocator;
  Reference<ID3D12GraphicsCommandList> list;
  void create(ID3D12Device* device) {
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())), "Snapshot allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(list.put())),
          "Snapshot command list");
  }
};
void run(bool warp, bool a350) {
  Reference<ID3D12Debug> debug;
  const bool debug_enabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())));
  if (debug_enabled)
    debug->EnableDebugLayer();
  Reference<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "Snapshot DXGI factory");
  Reference<IDXGIAdapter> adapter;
  if (warp)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "Snapshot WARP");
  Reference<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.put())), "Snapshot device");
  Reference<ID3D12InfoQueue> messages;
  if (debug_enabled)
    check(device->QueryInterface(IID_PPV_ARGS(messages.put())), "Snapshot debug messages");
  constexpr std::uint64_t key = 9301;
  const auto& profile = a350 ? profiles::A359 : profiles::IniA380;
  require(runtime::init_device(key, device.get()) && runtime::set_patch_profile(key, profile.id), "Snapshot runtime profile");
  auto& manager = runtime::manager();
  auto& item = *runtime::find(key);
  runtime::QueuePatchConfig config{1, profile.id, 0, 3, {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM}};
  require(runtime::configure_queue_patches(key, config), "Configure typed calibration output");
  auto invalid = config;
  invalid.formats[0] = DXGI_FORMAT_R8G8B8A8_TYPELESS;
  require(!runtime::configure_queue_patches(key, invalid), "Opaque application RTV never becomes typeless output encoding");
  runtime::QueuePatchSnapshot snapshot;
  require(!runtime::try_snapshot_queue_patches(key, 1, snapshot) && !snapshot.ready_mask, "Cold request publishes no uninitialized buffer");
  runtime::service();
  require(runtime::try_snapshot_queue_patches(key, 1, snapshot) && snapshot.ready_mask == 3, "Calibration works without camera frames");
  require(!item.status.initialized && !item.status.frames, "Calibration cannot impersonate camera warmup progress");
  require(drain_copy_queue(item.calibration_output.queue(), device.get()), "First calibration completed");
  auto* stable = snapshot.sides[0].buffer;
  for (unsigned side = 0; side < 2; ++side) {
    const auto outer = profiles::display_rect(profile, side);
    require(snapshot.profile == profile.id && snapshot.sides[side].calibration && snapshot.sides[side].buffer &&
                snapshot.sides[side].destination.left == static_cast<LONG>(outer.left) &&
                snapshot.sides[side].destination.right == static_cast<LONG>(outer.right) &&
                snapshot.sides[side].destination.bottom == 763 && snapshot.sides[side].footprint.Footprint.Height == 763 &&
                snapshot.sides[side].footprint.Footprint.Format == config.formats[side],
            "Exact typed PFD column excludes the lower trim and neighboring A350 display");
  }
  {
    std::unique_lock held(runtime::runtime().mutex);
    auto reader = std::async(std::launch::async, [&] {
      runtime::QueuePatchSnapshot denied;
      return !runtime::try_snapshot_queue_patches(key, 1, denied) && !denied.ready_mask && !denied.generation;
    });
    const bool prompt = reader.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
    held.unlock();
    require(reader.get() && prompt, "Snapshot callback refuses runtime contention without waiting");
  }

  // Make service submit real GPU work behind an unsignalled test fence. The
  // snapshot may be ready before GPU completion only because every consumer
  // joins the manager's already-queued signal on the same device timeline.
  Reference<ID3D12Fence> blocked;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(blocked.put())), "Calibration blocker");
  check(item.calibration_output.queue()->Wait(blocked.get(), 1), "Block calibration queue");
  config.generation = 2;
  require(runtime::configure_queue_patches(key, config), "New patch generation");
  require(!runtime::try_snapshot_queue_patches(key, 1, snapshot), "Configuration change invalidates stale generation immediately");
  runtime::service();
  require(runtime::try_snapshot_queue_patches(key, 2, snapshot) && snapshot.sides[0].buffer == stable &&
              item.calibration_output.completed_submissions() < item.calibration_output.submissions(),
          "Stable calibration buffers publish under the queued manager fence before GPU completion");
  Reference<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC qd{};
  check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.put())), "Snapshot consumer queue");
  std::array<Reference<ID3D12Resource>, 2> readbacks;
  std::array<Recording, 2> consumers;
  unsigned next_generation = 10;
  const auto copy_snapshot = [&](const runtime::QueuePatchSnapshot& value, bool drain) {
    std::array<ID3D12CommandList*, 2> batch{};
    for (unsigned side = 0; side < 2; ++side) {
      const auto& patch = value.sides[side];
      auto& recording = consumers[side];
      const auto bytes = UINT64{patch.footprint.Footprint.RowPitch} * patch.footprint.Footprint.Height;
      if (!readbacks[side].get()) {
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(readbacks[side].put())),
              "Snapshot readback");
        recording.create(device.get());
      } else {
        manager.destroy_command_list(recording.list.get(), next_generation - 2 + side);
        check(recording.allocator->Reset(), "Reset completed consumer allocator");
        check(recording.list->Reset(recording.allocator.get(), nullptr), "Reset completed consumer list");
      }
      require(manager.register_command_list(recording.list.get(), key, next_generation + side) &&
                  manager.register_consumer_recording(recording.list.get()),
              "Register output consumer on actual manager timeline");
      const auto copied_bytes =
          UINT64{patch.footprint.Footprint.RowPitch} * (patch.footprint.Footprint.Height - 1) + UINT64{patch.footprint.Footprint.Width} * 4;
      recording.list->CopyBufferRegion(readbacks[side].get(), 0, patch.buffer, patch.footprint.Offset, copied_bytes);
      check(recording.list->Close(), "Close snapshot consumer");
      batch[side] = recording.list.get();
    }
    next_generation += 2;
    const auto receipt = manager.before_submission(queue.get(), 2, batch.data());
    require(receipt != 0, "Actual queued snapshot consumer receipt");
    queue->ExecuteCommandLists(2, batch.data());
    manager.after_submission(queue.get(), receipt);
    if (drain)
      require(drain_copy_queue(queue.get(), device.get()), "Snapshot consumer completed");
  };
  copy_snapshot(snapshot, false);
  Reference<ID3D12Fence> consumed;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(consumed.put())), "Consumer completion fence");
  check(queue->Signal(consumed.get(), 1), "Consumer fence after output read");
  require(consumed->GetCompletedValue() == 0, "Consumer cannot pass incomplete calibration writer");
  const auto calibration_submissions = item.calibration_output.submissions();
  auto reset = std::async(std::launch::async, [&] { return runtime::reset_session(key); });
  const bool reset_prompt = reset.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
  if (!reset_prompt)
    check(blocked->Signal(1), "Unblock fixture before reporting a reset wait regression");
  const auto session = reset.get();
  require(reset_prompt && session > 1 && !runtime::snapshot(key).session_active, "Flight reset never waits for blocked GPU work");
  require(!runtime::configure_queue_patches(key, config), "Loading controls cannot revive calibration demand");
  runtime::QueuePatchSnapshot stopped;
  require(!runtime::try_snapshot_queue_patches(key, config.generation, stopped) && !stopped.ready_mask,
          "Reset immediately withdraws the old prepared calibration snapshot");
  runtime::service();
  require(item.calibration_output.submissions() == calibration_submissions && !item.queue_config.generation,
          "Paused service drains without submitting more calibration work");
  std::array<ID3D12CommandList*, 2> replay{consumers[0].list.get(), consumers[1].list.get()};
  const auto replay_receipt = manager.before_submission(queue.get(), 2, replay.data());
  require(replay_receipt != 0, "Prior-session consumer replay retains timeline ownership");
  queue->ExecuteCommandLists(2, replay.data());
  manager.after_submission(queue.get(), replay_receipt);
  require(consumed->GetCompletedValue() == 0 && !manager.register_consumer_recording(consumers[0].list.get()),
          "Reset preserves old reads but refuses new work in an old recording");
  require(!runtime::resume_session(key, session - 1) && runtime::resume_session(key, session),
          "Only the current flight generation resumes; old GPU work need not be CPU-waited");
  require(!runtime::try_snapshot_queue_patches(key, config.generation, stopped), "Resume cannot republish previous-flight demand");
  require(runtime::configure_queue_patches(key, {}), "Disable demand without releasing submitted buffers");
  runtime::QueuePatchSnapshot disabled;
  require(!runtime::try_snapshot_queue_patches(key, 2, disabled), "Disabled generation is not published");
  check(blocked->Signal(1), "Release calibration writer");
  require(drain_copy_queue(queue.get(), device.get()), "Disabled retained buffer finishes its existing reader");
  unsigned checked = 0;
  const auto pixel_check = [&](const runtime::QueuePatchSnapshot& value, bool calibration) {
    for (unsigned side = 0; side < 2; ++side) {
      const auto& patch = value.sides[side];
      const auto width = patch.footprint.Footprint.Width, pitch = patch.footprint.Footprint.RowPitch;
      const D3D12_RANGE range{0, SIZE_T{pitch} * 763};
      void* mapped = nullptr;
      check(readbacks[side]->Map(0, &range, &mapped), "Map typed snapshot");
      bool correct = true;
      for (unsigned band = 0; band < 2; ++band) {
        const unsigned y = band ? 500 : 100;
        for (unsigned offset = 0; offset < 8; ++offset) {
          const unsigned x = width / 2 + offset;
          const auto* pixel = static_cast<const unsigned char*>(mapped) + SIZE_T{pitch} * y + x * 4;
          std::array<unsigned char, 4> expected =
              calibration ? (band ? std::array<unsigned char, 4>{31, 82, 8, 255} : std::array<unsigned char, 4>{5, 41, 97, 255})
                          : (band ? std::array<unsigned char, 4>{153, 51, 102, 255} : std::array<unsigned char, 4>{51, 102, 153, 255});
          auto marker = band ? std::array<unsigned char, 4>{255, 204, 0, 255} : std::array<unsigned char, 4>{0, 230, 255, 255};
          if (side) {
            std::swap(expected[0], expected[2]);
            std::swap(marker[0], marker[2]);
          }
          // The cyan marker's 0.9f * 255 is 229.4999939 in exact arithmetic.
          // Hardware clears produce 229 while WARP may round the intermediate
          // product to 229.5 and produce 230. Accept only that one-channel
          // ambiguity; other marker channels, backgrounds and camera pixels
          // retain exact checks. Marker motion makes this case time-dependent.
          const bool cyan_rounding =
              !band && pixel[0] == marker[0] && pixel[1] == marker[1] - 1 && pixel[2] == marker[2] && pixel[3] == marker[3];
          const bool matches =
              std::memcmp(pixel, expected.data(), 4) == 0 || (calibration && (std::memcmp(pixel, marker.data(), 4) == 0 || cyan_rounding));
          if (correct && !matches)
            std::fprintf(stderr,
                         "Snapshot mismatch: mode=%s profile=%u calibration=%d side=%u xy=%u,%u format=%u "
                         "actual=%u,%u,%u,%u expected=%u,%u,%u,%u marker=%u,%u,%u,%u generation=%llu\n",
                         warp ? "WARP" : "hardware", profile.id, calibration, side, x, y,
                         static_cast<unsigned>(patch.footprint.Footprint.Format), pixel[0], pixel[1], pixel[2], pixel[3], expected[0],
                         expected[1], expected[2], expected[3], marker[0], marker[1], marker[2], marker[3], value.generation);
          correct &= matches;
          ++checked;
        }
      }
      const D3D12_RANGE none{0, 0};
      readbacks[side]->Unmap(0, &none);
      require(correct, "Prepared camera/calibration snapshot contains actual typed GPU pixels");
    }
  };
  pixel_check(snapshot, true);

  require(runtime::prepare(key) && runtime::set_display_exposure(key, 0), "Prepare ordinary camera output");
  config = {3, profile.id, 3, 0, {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM}};
  require(runtime::configure_queue_patches(key, config), "Configure camera patch demand");
  auto& handoff = scene_handoff();
  require(handoff.register_device(key) != 0, "Camera source handoff");
  std::array<Reference<ID3D12Resource>, 2> sources;
  std::array<Recording, 2> recordings;
  Reference<ID3D12DescriptorHeap> heap;
  const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(heap.put())), "Camera RTVs");
  std::array<std::uint64_t, 3> handles{};
  for (unsigned feed = 0; feed < 2; ++feed) {
    const auto pane = profile.camera_panes[feed];
    create_texture(device.get(), texture_description(pane[0], pane[1], DXGI_FORMAT_R8G8B8A8_UNORM), sources[feed].put());
    handles[feed] = reinterpret_cast<std::uint64_t>(sources[feed].get());
    require(handoff.register_resource(key, handles[feed], 501 + feed), "Camera source identity");
  }
  handoff.begin_scene();
  require(handoff.publish(handoff.begin_capture(), {73, 1}, {101, 102}, handles), "Camera source publication");
  std::array<ID3D12CommandList*, 2> batch{};
  for (unsigned feed = 0; feed < 2; ++feed) {
    auto& recording = recordings[feed];
    recording.create(device.get());
    require(manager.register_command_list(recording.list.get(), key, 100 + feed), "Capture recording registration");
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{heap->GetCPUDescriptorHandleForHeapStart().ptr +
                                          SIZE_T{feed} * device->GetDescriptorHandleIncrementSize(hd.Type)};
    device->CreateRenderTargetView(sources[feed].get(), nullptr, rtv);
    const std::array<float, 4> colour = feed ? std::array<float, 4>{.6f, .2f, .4f, 1} : std::array<float, 4>{.2f, .4f, .6f, 1};
    recording.list->ClearRenderTargetView(rtv, colour.data(), 0, nullptr);
    require(manager.record_render_target_before_transition(recording.list.get(), sources[feed].get(), true, 100 + feed),
            "Capture actual GPU camera pixels");
    check(recording.list->Close(), "Close camera capture");
    batch[feed] = recording.list.get();
  }
  const auto receipt = manager.before_submission(queue.get(), 2, batch.data());
  require(receipt != 0, "Camera producer receipt");
  queue->ExecuteCommandLists(2, batch.data());
  manager.after_submission(queue.get(), receipt);
  for (unsigned feed = 0; feed < 2; ++feed)
    manager.destroy_command_list(recordings[feed].list.get(), 100 + feed);
  require(drain_copy_queue(queue.get(), device.get()), "Camera snapshots ready");
  runtime::service();
  require(runtime::try_snapshot_queue_patches(key, 3, snapshot) && snapshot.ready_mask == 3 && !snapshot.sides[0].calibration &&
              !snapshot.sides[1].calibration && item.status.frames == 1,
          "Actual service composes and publishes camera patches");
  copy_snapshot(snapshot, true);
  pixel_check(snapshot, false);
  require(runtime::set_patch_profile(key, a350 ? profiles::IniA380.id : profiles::A359.id), "Change patch profile");
  require(!runtime::try_snapshot_queue_patches(key, 3, disabled), "Profile transition cannot expose old geometry");
  manager.destroy_command_list(consumers[0].list.get(), next_generation - 2);
  manager.destroy_command_list(consumers[1].list.get(), next_generation - 1);
  handoff.stop_scene();
  runtime::reset_feed(key);
  UINT64 errors = 0;
  if (messages.get())
    for (UINT64 index = 0; index < messages->GetNumStoredMessages(); ++index) {
      SIZE_T size = 0;
      check(messages->GetMessage(index, nullptr, &size), "Debug size");
      std::vector<unsigned char> bytes(size);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
      check(messages->GetMessage(index, message, &size), "Debug message");
      if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
        ++errors;
        std::fprintf(stderr, "D3D12 error %u: %s\n", message->ID, message->pDescription);
      }
    }
  require(errors == 0, "Queue snapshot GPU validation");
  std::printf(
      "PASS queue patch snapshots %s profile=%u: cold/contended/generation refusal, flight reset/replay/resume, pending-GPU timeline, "
      "retained buffers, %u pixels; "
      "debug=%d errors=%llu.\n",
      warp ? "WARP" : "hardware", profile.id, checked, debug_enabled, errors);
}
// PMDG 777: both navigation displays share one texture, and the lower DU is a
// third slot on another texture. Each slot gets its own exact rectangle.
void lower_display(bool warp) {
  Reference<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "Lower DXGI factory");
  Reference<IDXGIAdapter> adapter;
  if (warp)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "Lower WARP");
  Reference<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.put())), "Lower device");
  constexpr std::uint64_t key = 9302;
  const auto& profile = profiles::Pmdg777300ER;
  require(runtime::init_device(key, device.get()) && runtime::set_patch_profile(key, profile.id), "Lower runtime profile");
  runtime::QueuePatchConfig config{
      1, profile.id, 0, 7, {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM}};
  require(runtime::configure_queue_patches(key, config), "Configure three display slots");
  auto narrow = config;
  narrow.profile = profiles::A359.id;
  require(!runtime::set_patch_profile(key, profiles::A359.id) || !runtime::configure_queue_patches(key, narrow),
          "A two-display profile accepted a lower slot");
  require(runtime::set_patch_profile(key, profile.id) && runtime::configure_queue_patches(key, config), "Restore the 777 profile");
  runtime::service();
  runtime::QueuePatchSnapshot snapshot;
  require(runtime::try_snapshot_queue_patches(key, 1, snapshot) && snapshot.ready_mask == 7, "Lower DU calibration patch missing");
  auto* item = runtime::find(key);
  require(drain_copy_queue(item->calibration_output.queue(), device.get()), "Lower calibration completed");
  for (unsigned side = 0; side < MaxDisplaySides; ++side) {
    const auto outer = profiles::display_rect(profile, side), inner = profiles::display_content_rect(profile, side);
    const auto& patch = snapshot.sides[side];
    require(patch.calibration && patch.buffer && patch.destination.left == static_cast<LONG>(outer.left) &&
                patch.destination.top == static_cast<LONG>(outer.top) && patch.destination.right == static_cast<LONG>(outer.right) &&
                patch.destination.bottom == static_cast<LONG>(outer.bottom) && patch.content.top == static_cast<LONG>(inner.top) &&
                patch.footprint.Footprint.Width == outer.right - outer.left &&
                patch.footprint.Footprint.Height == outer.bottom - outer.top && patch.footprint.Footprint.Format == config.formats[side],
            "A 777 display slot received the wrong rectangle or encoding");
  }
  require(snapshot.sides[2].destination.left == 1058 && snapshot.sides[2].destination.top == 21, "Lower DU is not DU_Lower on EICASCDU");
  runtime::reset_feed(key);
  std::printf("PASS queue patch snapshots %s profile=%u: left/right navigation displays and lower DU rectangles.\n",
              warp ? "WARP" : "hardware", profile.id);
}
}  // namespace
int main(int argc, char** argv) {
  try {
    bool warp = false, a350 = false;
    for (int i = 1; i < argc; ++i)
      if (std::strcmp(argv[i], "--warp") == 0)
        warp = true;
      else if (std::strcmp(argv[i], "--a350") == 0)
        a350 = true;
      else
        return 2;
    run(warp, a350);
    if (!a350)
      lower_display(warp);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL queue patch snapshot: %s\n", error.what());
    return 1;
  }
}
