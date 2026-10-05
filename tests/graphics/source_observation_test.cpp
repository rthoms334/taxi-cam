#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include "../../src/graphics/scene_capture_manager.hpp"

namespace {
unsigned checks;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}
template <typename T>
struct Ref {
  T* value = nullptr;
  ~Ref() {
    if (value)
      value->Release();
  }
};
void run(bool warp) {
  using Manager = taxi_camera::SceneCaptureManager;
  using namespace taxi_camera::source_state;
  Ref<ID3D12Device> device;
  Ref<IDXGIFactory4> factory;
  Ref<IDXGIAdapter> adapter;
  require(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory.value))), "Factory");
  if (warp)
    require(SUCCEEDED(factory.value->EnumWarpAdapter(IID_PPV_ARGS(&adapter.value))), "WARP adapter");
  require(SUCCEEDED(D3D12CreateDevice(adapter.value, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device.value))), "Create device");
  Ref<ID3D12CommandAllocator> allocator, idle_allocator;
  Ref<ID3D12GraphicsCommandList> list, idle_list;
  require(SUCCEEDED(device.value->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.value))), "Allocator");
  require(SUCCEEDED(device.value->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&idle_allocator.value))),
          "Idle allocator");
  require(
      SUCCEEDED(device.value->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.value, nullptr, IID_PPV_ARGS(&list.value))),
      "List");
  require(SUCCEEDED(device.value->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, idle_allocator.value, nullptr,
                                                    IID_PPV_ARGS(&idle_list.value))),
          "Idle list");
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 736;
  desc.Height = 251;
  desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  Ref<ID3D12Resource> resource;
  require(SUCCEEDED(device.value->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                          IID_PPV_ARGS(&resource.value))),
          "Resource");
  taxi_camera::SceneHandoff handoff;
  auto manager = std::make_unique<Manager>(handoff);
  require(manager->register_device(1, device.value), "Register device");
  require(manager->register_command_list(list.value, 1, 10), "Register list");
  require(manager->register_source_candidate(1, resource.value, 20, desc, Model::legacy_rt), "Register source");
  manager->begin_source_tracking();
  const std::uint64_t generation = 20, wrong_generation = 21;
  const Key key{reinterpret_cast<std::uint64_t>(resource.value), generation};
  auto* recording = manager->list(list.value);
  auto* owner = manager->device(1);
  manager->observe_source_draw_after(list.value, 10, 1, &resource.value, &generation, true);
  require(manager->statistics().source_draws == 1 && recording->source_lease_count == 1, "Combined draw retains exact source lease");
  manager->observe_source_draw_after(list.value, 11, 1, &resource.value, &generation, true);
  manager->observe_source_draw_after(list.value, 10, 1, &resource.value, &wrong_generation, true);
  manager->observe_source_draw_after(list.value, 10, 9, &resource.value, &generation, true);
  require(manager->statistics().source_draws == 1, "Combined draw admitted stale or excessive metadata");
  manager->stage_source_draw(list.value, 10, 1, &resource.value, &generation);
  manager->observe_source_draw_after(list.value, 10, 0, nullptr, nullptr, true);
  manager->after_source_draw(list.value, 10, true);
  require(manager->statistics().source_draws == 1, "Empty combined call retained a stale split stage");
  manager->stage_source_draw(list.value, 10, 1, &resource.value, &generation);
  manager->after_source_draw(list.value, 10, true);
  require(manager->statistics().source_draws == 2 && recording->source_lease_count == 1, "Split adapter behavior changed");
  manager->observe_source_draw_after(list.value, 10, 1, &resource.value, &generation, false);
  require(manager->statistics().invalid_draws == 1 && recording->source_effects.invalid, "Forbidden draw did not invalidate proof");
  require(manager->register_consumer_recording(list.value), "Register stable output replay consumer");
  const auto leases = recording->source_lease_count;
  manager->set_capture_enabled(false);
  require(recording->consumer && recording->source_lease_count == leases, "Idle dropped replay/lease obligations");
  require(owner->source_states.state(key).model == Model::legacy_rt, "Idle discarded creation-state proof");
  manager->observe_source_draw_after(list.value, 10, 1, &resource.value, &generation, true);
  require(manager->statistics().source_draws == 3 && recording->source_effects.invalid, "Idle changed draw observation or invalid proof");
  require(manager->register_command_list(idle_list.value, 1, 11), "Register idle-created list");
  require(!manager->list(idle_list.value)->source_effects.invalid, "Idle-created list lost observed recording proof");
  require(SUCCEEDED(idle_list.value->Close()) && SUCCEEDED(idle_list.value->Reset(idle_allocator.value, nullptr)), "Reset idle list");
  const auto idle_slot = manager->successful_reset(idle_list.value, 11);
  require(!manager->list(idle_list.value)->source_effects.invalid, "Reset while idle lost observed recording proof");
  // Nothing changed the list since: its next Reset with the returned slot is
  // retired without the manager lock and counted like a locked clean Reset.
  require(SUCCEEDED(idle_list.value->Close()) && SUCCEEDED(idle_list.value->Reset(idle_allocator.value, nullptr)), "Reset clean idle list");
  const auto before_clean = manager->statistics();
  require(manager->successful_reset(idle_list.value, 11, idle_slot) == idle_slot &&
              manager->statistics().fast_resets == before_clean.fast_resets + 1 &&
              manager->statistics().resets == before_clean.resets + 1 &&
              manager->statistics().clean_resets == before_clean.clean_resets + 1,
          "A clean list's Reset took the manager lock or was not counted");
  require(SUCCEEDED(list.value->Close()) && SUCCEEDED(list.value->Reset(allocator.value, nullptr)), "Reset spanning list");
  manager->successful_reset(list.value, 10);
  require(!recording->source_effects.invalid && !recording->consumer && recording->source_lease_count == 0,
          "Reset retained retired leases or forbidden prior recording");
  manager->observe_source_draw_after(list.value, 10, 1, &resource.value, &generation, true);
  require(manager->statistics().source_draws == 4 && recording->source_lease_count == 1, "Idle draw observation stopped");
  owner->source_states.begin_batch();
  require(owner->source_states.apply(recording->source_effects), "Observed idle recording invalid");
  manager->set_capture_enabled(true);
  require(owner->source_states.state(key).model == Model::legacy_rt && owner->source_states.state(key).drawn,
          "Resume required a new barrier for a continuously observed born-RT source");

  // Source barriers remain observed while capture is disabled; the gate must
  // not freeze the last known RT state across a real application transition.
  manager->set_capture_enabled(false);
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource.value, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON};
  manager->observe_source_legacy(idle_list.value, 11, barrier);
  owner->source_states.begin_batch();
  require(owner->source_states.apply(manager->list(idle_list.value)->source_effects), "Idle barrier effects invalid");
  require(owner->source_states.state(key).model == Model::other, "Capture gate skipped a real source-state transition");
  manager->set_capture_enabled(true);
  require(owner->source_states.state(key).model == Model::other, "Resume fabricated RT proof");

  // A failed tail attempt can leave closed, never-submitted query commands on
  // a free packet. A later ordinary application capture must not publish those
  // queries under its unrelated submission fence.
  Ref<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  require(SUCCEEDED(device.value->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.value))), "Timing regression queue");
  auto& packet = manager->packets_[0];
  require(manager->prepare_tail(packet, *owner), "Prepare never-submitted private recording");
  require(packet.tail_timing.begin(device.value, queue.value, packet.tail_list), "Begin abandoned private timing");
  packet.tail_timing.start(0);
  taxi_camera::SceneCopyMatch match{};
  match.matched = true;
  match.resource = {1, 20, 1};
  Manager::List private_recording;
  private_recording.native = packet.tail_list;
  private_recording.device_key = 1;
  manager->set_capture_enabled(false);
  require(!manager->capture_source(private_recording, *owner, match, resource.value, true, nullptr, false, nullptr, &packet),
          "Fixture must refuse the private capture before submission");
  packet.tail_timing.end(0);
  packet.tail_timing.resolve();
  require(SUCCEEDED(packet.tail_list->Close()), "Close never-submitted private recording");
  manager->set_capture_enabled(true);
  require(manager->capture_source(*recording, *owner, match, resource.value, true, nullptr, false, nullptr, &packet),
          "Reuse same packet for an ordinary application capture");
  require(SUCCEEDED(list.value->Close()), "Close application capture");
  manager->set_capture_enabled(false);  // Existing recordings still execute and retire while capture is OFF.
  ID3D12CommandList* application = list.value;
  const auto receipt = manager->before_submission(queue.value, 1, &application);
  require(receipt != 0, "Admit application capture receipt");
  queue.value->ExecuteCommandLists(1, &application);
  manager->after_submission(queue.value, receipt);
  const auto deadline = GetTickCount64() + 5000;
  while (owner->timeline->GetCompletedValue() < owner->last_signal && GetTickCount64() < deadline)
    Sleep(1);
  require(owner->timeline->GetCompletedValue() == owner->last_signal, "Complete ordinary capture fence");
  require(packet.tail_timing.state_ == taxi_camera::OwnedGpuTiming<1>::State::prepared && !packet.tail_timing.fence_,
          "Unsubmitted private queries borrowed an unrelated application receipt");
  require(manager->statistics().capture_copy_gpu.samples == 0, "Unsubmitted private queries produced a GPU timing sample");
  require(SUCCEEDED(packet.tail_allocator->Reset()) && SUCCEEDED(packet.tail_list->Reset(packet.tail_allocator, nullptr)),
          "Reset never-executed private queries before discarding");
  packet.tail_timing.discard_unsubmitted();
  require(packet.tail_timing.state_ == taxi_camera::OwnedGpuTiming<1>::State::idle,
          "Discarded private queries did not recover for the next owned recording");
  require(SUCCEEDED(packet.tail_list->Close()), "Close discarded private recording");
  manager->destroy_command_list(list.value, 10);
  manager->destroy_command_list(idle_list.value, 11);
  manager->stop_source_tracking();
  std::printf("{\"passed\":true,\"checks\":%u,\"combined_draw\":true,\"idle_reset_recovery\":true,\"warp\":%s}\n", checks,
              warp ? "true" : "false");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    require(argc == 1 || (argc == 2 && !std::strcmp(argv[1], "--warp")), "Usage: source-observation-test [--warp]");
    run(argc == 2);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
