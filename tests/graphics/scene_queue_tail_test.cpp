// Reuse the existing GPU harness, with a separate entry point and receipts.
#define main existing_manager_validation_entry
#include "scene_capture_manager_test.cpp"
#undef main
#include <chrono>
#include <future>
#include "../../src/camera/aircraft_mounts.hpp"
#include "../../src/graphics/native_device_identity.hpp"
#include "../../src/hooks/queue_submit_observer.hpp"

namespace {
namespace submission_lock_fixture {
struct Device {
  void** table;
};
struct Queue {
  void** table;
  Device* device;
  std::uint64_t last_signal = 0, last_wait = 0;
  unsigned waits = 0, signals = 0;
  bool future_wait = false;
  ULONG references = 1;
  // Set: each Release checks from another thread that the manager mutex is free.
  const Manager* lock_probe = nullptr;
  bool released_under_lock = false;
};
HRESULT STDMETHODCALLTYPE identity(void* self, REFIID iid, void** result) {
  *result = nullptr;
  if (iid != __uuidof(IUnknown))
    return E_NOINTERFACE;
  *result = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE reference(void*) {
  return 1;
}
ULONG STDMETHODCALLTYPE add_queue_reference(Queue* queue) {
  return ++queue->references;
}
ULONG STDMETHODCALLTYPE release_queue_reference(Queue* queue) {
  if (queue->lock_probe) {
    auto& mutex = queue->lock_probe->mutex_;
    queue->released_under_lock |= !std::async(std::launch::async, [&mutex] {
                                     std::unique_lock probe(mutex, std::try_to_lock);
                                     return probe.owns_lock();
                                   }).get();
  }
  return --queue->references;
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
  queue->last_wait = value;
  ++queue->waits;
  return S_OK;
}
// The fake DIRECT queue vtable: identity through GetDevice, counted references.
std::array<void*, 19> queue_vtable() {
  std::array<void*, 19> table{};
  table[1] = reinterpret_cast<void*>(&add_queue_reference);
  table[2] = reinterpret_cast<void*>(&release_queue_reference);
  table[7] = reinterpret_cast<void*>(&get_device);
  table[14] = reinterpret_cast<void*>(&signal);
  table[15] = reinterpret_cast<void*>(&wait_on);
  table[18] = reinterpret_cast<void*>(&description);
  return table;
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
struct Timeline {
  void** table;
  std::atomic<std::uint64_t> completed{0};
  std::atomic<unsigned> cpu_signals{0};
};
std::uint64_t STDMETHODCALLTYPE completed_value(Timeline* self) {
  return self->completed.load(std::memory_order_acquire);
}
HRESULT STDMETHODCALLTYPE cpu_signal(Timeline* self, std::uint64_t value) {
  auto seen = self->completed.load(std::memory_order_relaxed);
  while (seen < value && !self->completed.compare_exchange_weak(seen, value, std::memory_order_release, std::memory_order_relaxed)) {
  }
  self->cpu_signals.fetch_add(1, std::memory_order_relaxed);
  return S_OK;
}
// ReShade 6.8's dxgi.dll flush CPU-waits on a fence queued behind the bridge
// timeline Wait. Closing the gate must satisfy that Wait from the CPU without
// taking the manager mutex. ReShade stays in the DXGI chain.
void reshade_present_gate() {
  void* device_table[]{reinterpret_cast<void*>(&identity), reinterpret_cast<void*>(&reference), reinterpret_cast<void*>(&reference)};
  Device device{device_table};
  auto queue_table = queue_vtable();
  // Only a receipt on another queue than the last Signal's queues a Wait.
  Queue queue{queue_table.data(), &device}, other_queue{queue_table.data(), &device};
  std::array<void*, 11> fence_table{};
  fence_table[8] = reinterpret_cast<void*>(&completed_value);
  fence_table[10] = reinterpret_cast<void*>(&cpu_signal);
  Timeline timeline{fence_table.data()};
  taxi_camera::SceneHandoff handoff;
  auto manager = std::make_unique<Manager>(handoff);
  auto& owner = manager->devices_[0];
  owner.key = 7;
  owner.native = reinterpret_cast<ID3D12Device*>(&device);
  owner.timeline = reinterpret_cast<ID3D12Fence*>(&timeline);
  owner.active = true;
  manager->published_devices_[0].store(owner.native);
  std::uintptr_t marker = 0;
  auto* known = reinterpret_cast<ID3D12GraphicsCommandList*>(&marker);
  auto& recording = manager->lists_[0];
  recording.native = known;
  recording.device_key = 7;
  recording.object_generation = 19;
  recording.session_generation = owner.session_generation;
  recording.packets = 1;
  manager->list_indices_.emplace(known, 0);
  manager->publish_list(recording);
  manager->set_submission_gate(false);
  require(timeline.cpu_signals.load() == 0, "Closing the gate with no queued Wait signaled a fence");
  manager->set_submission_gate(true);
  require(timeline.cpu_signals.load() == 0, "Opening the gate signaled a fence");
  const auto submit = [&](Queue& target) {
    auto* native_queue = reinterpret_cast<ID3D12CommandQueue*>(&target);
    ID3D12CommandList* batch[]{known};
    const auto receipt = manager->before_submission(native_queue, 1, batch);
    require(receipt != 0, "ReShade-present fixture did not open an ordered transaction");
    manager->after_submission(native_queue, receipt);
  };
  submit(queue);
  submit(other_queue);
  require(queue.signals == 1 && other_queue.signals == 1 && !queue.waits && other_queue.waits == 1 && other_queue.last_wait == 1 &&
              manager->published_timelines_[0].waited.load() == 1 && timeline.completed.load() == 0 && timeline.cpu_signals.load() == 0,
          "The queued Wait value was not published ahead of the ReShade flush");
  std::promise<void> locked;
  std::promise<void> release_holder;
  auto locked_future = locked.get_future();
  auto release_future = release_holder.get_future();
  auto holder = std::async(std::launch::async, [&] {
    std::lock_guard lock(manager->mutex_);
    locked.set_value();
    release_future.wait();
  });
  locked_future.wait();
  auto waiter = std::async(std::launch::async, [&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (timeline.completed.load(std::memory_order_acquire) < 1) {
      if (std::chrono::steady_clock::now() > deadline)
        return false;
    }
    return true;
  });
  auto closed = std::async(std::launch::async, [&] { manager->set_submission_gate(false); });
  const bool returned = closed.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
  release_holder.set_value();
  holder.get();
  if (returned)
    closed.get();
  else
    closed.wait();
  require(returned, "set_submission_gate(false) blocked while the manager mutex was held");
  require(waiter.get(), "ReShade-shaped waiter was not released by the CPU fence signal");
  require(timeline.cpu_signals.load() == 1 && timeline.completed.load() == 1 && manager->statistics().released_waits == 1,
          "Gate close did not CPU-signal the waited timeline value once");
  manager->set_submission_gate(false);
  manager->set_submission_gate(true);
  require(timeline.cpu_signals.load() == 1, "A repeated close or an open signaled the fence again");
  submit(queue);
  require(manager->published_timelines_[0].waited.load() == 2 && queue.last_wait == 2 && timeline.completed.load() == 1,
          "A later transaction did not publish its new Wait");
  manager->set_submission_gate(false);
  require(timeline.cpu_signals.load() == 2 && timeline.completed.load() >= 2 && manager->statistics().released_waits == 2,
          "A newer outstanding Wait was not released when the gate closed again");
  manager->set_submission_gate(true);
  manager->set_submission_gate(false);
  require(timeline.cpu_signals.load() == 2, "Closing the gate after the waited value completed signaled again");
}
// Plans one display copy for device 7 while active; the copy names no target
// or source, so Pool::record refuses it.
struct DisplayPlanFixture {
  std::atomic<std::uint64_t> generation{1};
  bool active = false;
};
void plan_unrecordable_copy(void* opaque,
                            ID3D12CommandQueue*,
                            UINT,
                            ID3D12CommandList* const*,
                            Manager::DisplaySubmissionPlan& plan) noexcept {
  auto& fixture = *static_cast<DisplayPlanFixture*>(opaque);
  if (!fixture.active)
    return;
  plan.device_key = 7;
  plan.generation = 1;
  plan.current_generation = &fixture.generation;
  plan.count = 1;
}
// Receipts without source work (consumer-only, private, display and in-list
// capture) must still Signal, be accounted and, when the device fails between
// before and after, quarantine their packets.
void receipts_without_source_work() {
  void* device_table[]{reinterpret_cast<void*>(&identity), reinterpret_cast<void*>(&reference), reinterpret_cast<void*>(&reference)};
  Device device{device_table};
  auto queue_table = queue_vtable();
  Queue queue{queue_table.data(), &device}, private_queue{queue_table.data(), &device};
  std::array<void*, 11> fence_table{};
  fence_table[8] = reinterpret_cast<void*>(&completed_value);
  fence_table[10] = reinterpret_cast<void*>(&cpu_signal);
  Timeline timeline{fence_table.data()};
  auto* native_queue = reinterpret_cast<ID3D12CommandQueue*>(&queue);
  auto* private_native = reinterpret_cast<ID3D12CommandQueue*>(&private_queue);
  taxi_camera::SceneHandoff handoff;
  auto manager = std::make_unique<Manager>(handoff);
  auto& owner = manager->devices_[0];
  owner.key = 7;
  owner.native = reinterpret_cast<ID3D12Device*>(&device);
  owner.timeline = reinterpret_cast<ID3D12Fence*>(&timeline);
  owner.active = true;
  manager->published_devices_[0].store(owner.native);
  std::uintptr_t marker = 0;
  auto* known = reinterpret_cast<ID3D12GraphicsCommandList*>(&marker);
  auto& recording = manager->lists_[0];
  recording.native = known;
  recording.device_key = 7;
  recording.object_generation = 21;
  recording.session_generation = owner.session_generation;
  recording.consumer = true;
  manager->list_indices_.emplace(known, 0);
  manager->publish_list(recording);
  ID3D12CommandList* batch[]{known};
  auto receipt = manager->before_submission(native_queue, 1, batch);
  require(receipt != 0 && !manager->transaction_.source_work, "A consumer-only batch did not open a receipt without source work");
  manager->after_submission(native_queue, receipt);
  require(queue.signals == 1 && queue.last_signal == 1 && owner.last_signal == 1 && manager->statistics().submissions == 1 &&
              !owner.failed && !manager->transaction_.id,
          "A consumer-only receipt was not signaled and accounted");
  recording.consumer = false;
  recording.source_touched = true;
  manager->publish_list(recording);
  receipt = manager->before_submission(native_queue, 1, batch);
  require(receipt != 0 && manager->transaction_.source_work, "A source batch did not open a receipt with source work");
  manager->after_submission(native_queue, receipt);
  require(queue.signals == 2 && owner.last_signal == 2 && manager->statistics().submissions == 2, "A source receipt was not accounted");
  recording.source_touched = false;
  const auto consume = manager->begin_private_submission(owner.key, private_native);
  require(consume.receipt != 0 && consume.value == 3, "Begin a private receipt");
  const bool ended = manager->end_private_submission(consume.receipt);
  require(ended && private_queue.signals == 1 && private_queue.last_signal == 3 && owner.last_signal == 3 &&
              manager->statistics().submissions == 3,
          "A private receipt was not signaled and accounted");
  // A display receipt whose planned copy the pool cannot record (never
  // serviced, no target or source): counted as a record failure, no copy.
  DisplayPlanFixture plan;
  require(manager->set_display_submission_planner(plan_unrecordable_copy, &plan), "Install the display plan fixture");
  plan.active = true;
  receipt = manager->before_submission(native_queue, 1, batch);
  plan.active = false;
  require(receipt != 0 && !manager->transaction_.source_work && !manager->transaction_.display_count &&
              manager->transaction_.display_unrecorded == 1,
          "A display plan did not open a receipt that counts its unrecorded copy");
  manager->after_submission(native_queue, receipt);
  require(queue.signals == 3 && owner.last_signal == 4 && manager->statistics().submissions == 4 &&
              manager->statistics().display_record_failures == 1 && !manager->statistics().display_copies,
          "An unrecorded display copy was not counted once");
  // An in-list capture receipt: its packet is not one fail_device quarantines
  // by itself, so only the finishing receipt can quarantine it.
  recording.packets = 1;
  manager->publish_list(recording);
  receipt = manager->before_submission(native_queue, 1, batch);
  require(receipt != 0 && !manager->transaction_.source_work && manager->packets_[0].in_flight == 1,
          "A capture batch did not open a receipt without source work");
  {
    const std::lock_guard lock(manager->mutex_);
    manager->fail_device(owner);  // Another thread fails the device mid-receipt.
  }
  require(!manager->packets_[0].quarantined, "The fixture packet was quarantined before its receipt finished");
  const auto quarantined = manager->statistics().quarantined;
  manager->after_submission(native_queue, receipt);
  require(queue.signals == 4 && queue.last_signal == 5 && owner.last_signal == 5 && manager->statistics().submissions == 5 &&
              manager->statistics().display_record_failures == 1,
          "A receipt on a device failed mid-flight did not Signal its forwarded work or was not accounted");
  require(manager->packets_[0].quarantined && !manager->packets_[0].in_flight && !manager->packets_[0].submitted &&
              manager->statistics().quarantined == quarantined + 1 && owner.failed && !manager->transaction_.id,
          "A receipt on a device failed mid-flight did not quarantine its packet");
}
struct FakeList {
  void** table;
  Device* device;
};
HRESULT STDMETHODCALLTYPE list_device(FakeList* list, REFIID, void** result) {
  *result = list->device;
  return S_OK;
}
D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE direct_type(FakeList*) {
  return D3D12_COMMAND_LIST_TYPE_DIRECT;
}
// A Reset of a list its last retirement left clean takes no manager lock. The
// scenario runs twice: `fast` passes back the slot each Reset returned, as the
// bridge does, and the other run never passes one, so all of its Resets lock.
// Both runs must reach the same observable state at every step; only
// fast_resets shows which path a Reset took.
void clean_reset_fast_path() {
  using Model = taxi_camera::source_state::Model;
  constexpr auto barrier_batch = taxi_camera::engine_hook::render_boundary::InvalidationBarrierBatch;
  void* device_table[]{reinterpret_cast<void*>(&identity), reinterpret_cast<void*>(&reference), reinterpret_cast<void*>(&reference)};
  Device device{device_table};
  std::array<void*, 9> list_table{};
  list_table[0] = reinterpret_cast<void*>(&identity);
  list_table[1] = list_table[2] = reinterpret_cast<void*>(&reference);
  list_table[7] = reinterpret_cast<void*>(&list_device);
  list_table[8] = reinterpret_cast<void*>(&direct_type);
  std::array<FakeList, 3> objects{{{list_table.data(), &device}, {list_table.data(), &device}, {list_table.data(), &device}}};
  std::array<ID3D12GraphicsCommandList*, 3> natives{};
  for (std::size_t index = 0; index < natives.size(); ++index)
    natives[index] = reinterpret_cast<ID3D12GraphicsCommandList*>(&objects[index]);
  auto* const source = reinterpret_cast<ID3D12Resource*>(std::uintptr_t{0x2340});
  const std::uint64_t source_generation = 5;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 736;
  desc.Height = 251;
  desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  const auto scenario = [&](bool fast) {
    std::vector<std::vector<std::uint64_t>> snapshots;
    taxi_camera::SceneHandoff handoff;
    auto manager = std::make_unique<Manager>(handoff);
    auto& owner = manager->devices_[0];
    owner.key = 7;
    owner.native = reinterpret_cast<ID3D12Device*>(&device);
    owner.active = true;
    manager->published_devices_[0].store(owner.native);
    std::array<std::uint32_t, 3> slots{Manager::NoListSlot, Manager::NoListSlot, Manager::NoListSlot};
    std::array<std::uint64_t, 3> generations{31, 32, 33};
    const auto reset = [&](std::size_t index) {
      const auto slot = manager->successful_reset(natives[index], generations[index], fast ? slots[index] : Manager::NoListSlot);
      if (fast)
        slots[index] = slot;
      return slot;
    };
    std::uint64_t fast_seen = 0;
    auto paths_seen = manager->reset_paths();
    // Checks how the Resets since the last call were retired: lock_free of
    // them without the lock, `locked` with it (the run without slots locks
    // every Reset), and `expired` of the locked ones deferred by an expired
    // wait.
    const auto took = [&](std::uint64_t lock_free, const char* label, std::uint64_t locked = 0, std::uint64_t expired = 0) {
      const auto now = manager->statistics().fast_resets;
      require(now == fast_seen + (fast ? lock_free : 0), label);
      fast_seen = now;
      const auto paths = manager->reset_paths();
      require(paths.fast - paths_seen.fast == (fast ? lock_free : 0), "Reset paths miscounted the lock-free Resets");
      require(paths.locked - paths_seen.locked == locked + (fast ? 0 : lock_free), "Reset paths miscounted the locked Resets");
      require(paths.expired - paths_seen.expired == expired, "Reset paths miscounted the expired Resets");
      paths_seen = paths;
    };
    const auto snapshot = [&] {
      std::vector<std::uint64_t> state;
      for (auto* native : natives) {
        const auto found = manager->list_indices_.find(native);
        state.push_back(found != manager->list_indices_.end());
        if (found == manager->list_indices_.end())
          continue;
        const auto& item = manager->lists_[found->second];
        state.insert(state.end(),
                     {item.object_generation, item.packets, item.feeds, item.consumer, item.source_touched, item.source_effects.count,
                      item.source_effects.invalid, item.source_effects.overflowed, item.session_generation, item.awaiting_native_reset,
                      item.source_lease_count, manager->published_lists_[found->second].effects.load() & 0xffffffffull});
      }
      const auto stats = manager->statistics();
      state.insert(state.end(),
                   {stats.resets, stats.clean_resets, stats.deferred_retirements, stats.contended_lifecycle, stats.contended_evidence,
                    stats.deferred_evidence, stats.deferred_overflows, stats.wipes, stats.source_retirements, stats.source_draws,
                    stats.unordered_consumers, owner.session_generation, owner.failed});
      snapshots.push_back(std::move(state));
    };
    // Runs call on another thread while this thread holds the manager lock.
    const auto while_held = [&](auto&& call) {
      std::unique_lock held(manager->mutex_);
      auto result = std::async(std::launch::async, [&] {
        call();
        return Manager::last_call_contended();
      });
      const bool returned = result.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
      held.unlock();
      const bool contended = result.get();
      require(returned, "A Reset fixture call was parked on the held manager lock");
      return contended;
    };

    require(manager->register_command_list(natives[0], owner.key, generations[0]), "Register the clean-Reset list");
    snapshot();
    require(reset(0) < Manager::MaximumLists, "A locked Reset did not return its list's slot");
    took(0, "The first Reset after admission did not take the lock", 1);
    snapshot();
    reset(0);
    took(1, "A clean list's Reset with its slot took the manager lock");
    snapshot();
    if (fast) {
      require(!while_held([&] { reset(0); }), "A clean Reset waited for the held manager lock");
      require(manager->statistics().deferred_retirements == 0, "A clean Reset was deferred instead of retired");
    } else {
      reset(0);
    }
    took(1, "A clean Reset under a held manager lock was not lock-free");
    snapshot();

    // Every change to the list forgets its mark first.
    manager->invalidate_source_recording(natives[0], generations[0], false, barrier_batch);  // Local: no publish.
    snapshot();
    reset(0);
    took(0, "A Reset after a local invalidation skipped the retirement", 1);
    snapshot();
    reset(0);
    took(1, "A list retired again was not clean");
    manager->invalidate_source_recording(natives[0], generations[0], true, barrier_batch);  // Touches sources: publishes.
    reset(0);
    took(0, "A Reset after a global invalidation skipped the retirement", 1);
    snapshot();
    require(manager->register_consumer_recording(natives[0]), "Register a consumer recording");
    snapshot();
    reset(0);
    took(0, "A consumer recording was retired without the lock", 1);
    snapshot();
    reset(0);
    took(1, "A retired consumer list was not clean");

    // A ring entry, even a peeked one whose apply has not committed, makes the
    // next Reset take the lock (here: defer behind it).
    require(while_held([&] { manager->invalidate_source_recording(natives[0], generations[0], false, barrier_batch); }),
            "Evidence under a held lock was not deferred");
    require(while_held([&] { reset(0); }), "A Reset behind a deferred entry did not take the lock");
    took(0, "A Reset behind a deferred entry was lock-free", 1, 1);
    snapshot();
    manager->apply_deferred();
    snapshot();
    reset(0);
    took(1, "A list retired by the drain was not clean");
    {
      std::unique_lock held(manager->mutex_);
      Manager::DeferredWork work;
      work.kind = Manager::DeferredWork::Kind::recording_report;
      work.native = natives[0];
      work.generation = generations[0];
      work.reasons = barrier_batch;
      manager->defer(work);
      Manager::DeferredWork entry;
      require(manager->deferred_work_.peek(entry) && entry.native == natives[0], "Peek the deferred entry");
      auto& item = manager->lists_[manager->list_indices_.at(natives[0])];
      manager->forget_clean(item);  // As the drain does, before applying.
      manager->apply_recording_report(item, false, barrier_batch);
      require(!manager->deferred_work_.idle(), "A peeked entry left the ring idle before its commit");
      auto behind = std::async(std::launch::async, [&] {
        reset(0);
        return Manager::last_call_contended();
      });
      const bool returned = behind.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
      manager->deferred_work_.commit();
      manager->apply_deferred_work();  // The deferred Reset, behind the committed entry.
      held.unlock();
      require(returned && behind.get(), "A Reset during an uncommitted apply did not take the lock");
    }
    took(0, "A Reset during an uncommitted apply was lock-free", 1, 1);
    snapshot();
    reset(0);
    took(1, "A list retired after the committed entry was not clean");

    // A lost entry: the overflowed ring makes the next Reset lock, and a drain
    // by any holder forgets every list before it clears the flag.
    const auto overflow = [&] {
      const std::unique_lock held(manager->mutex_);
      Manager::DeferredWork work;
      work.kind = Manager::DeferredWork::Kind::recording_report;
      work.native = natives[2];  // Not registered yet: drained as a no-op.
      work.generation = generations[2];
      for (unsigned index = 0; index <= 256; ++index)
        manager->defer(work);
      require(manager->deferred_work_.overflowed(), "The fixture did not overflow the ring");
    };
    overflow();
    reset(0);
    took(0, "A Reset with an overflowed ring was lock-free", 1);
    snapshot();
    reset(0);
    took(1, "A list retired after the overflow drain was not clean");
    overflow();
    manager->apply_deferred();  // Another holder drains the overflow first.
    require(manager->deferred_work_.idle(), "The overflow drain left the ring busy");
    snapshot();
    reset(0);
    took(0, "A Reset after another holder's overflow drain skipped the retirement", 1);
    snapshot();
    reset(0);
    took(1, "A list retired after the overflow invalidation was not clean");

    // Pending device work keeps its retirement time: such a Reset locks.
    manager->submission_refused_completed(1);
    reset(0);
    took(0, "A Reset with deferred source work pending was lock-free", 1);
    snapshot();
    manager->deferred_recordings_.store(true);
    reset(0);
    took(0, "A Reset with a deferred recording notice pending was lock-free", 1);
    reset(0);
    took(1, "A list retired after deferred device work was not clean");
    snapshot();

    // This thread's draw-repeat filter is settled by a locked Reset of any list.
    require(manager->register_command_list(natives[1], owner.key, generations[1]), "Register the second list");
    reset(1);
    took(0, "The second list's first Reset did not take the lock", 1);
    require(manager->register_source_candidate(owner.key, source, source_generation, desc, Model::legacy_rt), "Register a source");
    for (unsigned draw = 0; draw < 3; ++draw)
      manager->observe_source_draw_after(natives[0], generations[0], 1, &source, &source_generation, true, 7);
    snapshot();
    reset(1);
    took(0, "A Reset with this thread's draw-repeat filter armed was lock-free", 1);
    snapshot();
    reset(1);
    took(1, "A clean list's Reset after the settle took the lock");
    reset(0);
    took(0, "A Reset after source draws skipped the retirement", 1);
    snapshot();

    // A session reset forgets every mark; the next Reset adopts the session.
    reset(0);
    took(1, "A list retired after its draws was not clean");
    require(manager->reset_session(owner.key) != 0, "Reset the session");
    reset(0);
    took(0, "A Reset after a session reset kept the previous session", 1);
    snapshot();
    reset(0);
    took(1, "A list retired in the new session was not clean");

    // Destroy and slot reuse: a stale slot never names another list.
    const auto stale = slots[0];
    manager->destroy_command_list(natives[0], generations[0]);
    require(manager->successful_reset(natives[0], generations[0], fast ? stale : Manager::NoListSlot) == Manager::NoListSlot,
            "A Reset of a destroyed list found a slot");
    took(0, "A Reset of a destroyed list was lock-free", 1);
    snapshot();
    require(manager->register_command_list(natives[2], owner.key, generations[2]), "Register a list in the freed slot");
    require(!fast || manager->list_indices_.at(natives[2]) == stale, "The freed slot was not reused");
    reset(2);
    took(0, "A reused slot's first Reset did not take the lock", 1);
    require(manager->successful_reset(natives[0], generations[0], fast ? stale : Manager::NoListSlot) == Manager::NoListSlot &&
                manager->successful_reset(natives[0], generations[2], fast ? stale : Manager::NoListSlot) == Manager::NoListSlot,
            "A stale slot retired the list that reused it");
    took(0, "A stale slot took the lock-free path", 2);
    reset(2);
    took(1, "The reusing list's clean Reset took the lock");
    snapshot();
    generations[0] = 34;  // A new list object at the destroyed list's address.
    require(manager->register_command_list(natives[0], owner.key, generations[0]), "Register a list at a recycled address");
    slots[0] = fast ? stale : Manager::NoListSlot;
    reset(0);
    took(0, "A recycled address took a stale slot's lock-free path", 1);
    reset(0);
    took(1, "The recycled address's clean Reset took the lock");
    snapshot();
    return snapshots;
  };
  const auto fast = scenario(true);
  const auto locked = scenario(false);
  require(fast.size() == locked.size(), "The clean-Reset scenarios took different steps");
  for (std::size_t step = 0; step < fast.size(); ++step) {
    if (fast[step] != locked[step])
      for (std::size_t field = 0; field < std::min(fast[step].size(), locked[step].size()); ++field)
        if (fast[step][field] != locked[step][field])
          std::fprintf(stderr, "clean Reset step %zu field %zu: %llu (lock-free) != %llu (locked)\n", step, field,
                       static_cast<unsigned long long>(fast[step][field]), static_cast<unsigned long long>(locked[step][field]));
    require(fast[step] == locked[step], "A lock-free Reset left a different state than the locked Reset");
  }
}
// An ordered batch that cannot take a lock within the submit budget escapes,
// and its wait1000 wait is reported against the lock it waited for.
void ordered_waits_name_their_lock() {
  namespace ht = taxi_camera::hook_timing;
  void* device_table[]{reinterpret_cast<void*>(&identity), reinterpret_cast<void*>(&reference), reinterpret_cast<void*>(&reference)};
  Device device{device_table};
  auto queue_table = queue_vtable();
  Queue queue{queue_table.data(), &device};
  auto* native_queue = reinterpret_cast<ID3D12CommandQueue*>(&queue);
  taxi_camera::SceneHandoff handoff;
  auto manager = std::make_unique<Manager>(handoff);
  auto& owner = manager->devices_[0];
  owner.key = 7;
  owner.native = reinterpret_cast<ID3D12Device*>(&device);
  owner.active = true;
  manager->published_devices_[0].store(owner.native);
  std::uintptr_t marker = 0;
  auto* known = reinterpret_cast<ID3D12GraphicsCommandList*>(&marker);
  auto& recording = manager->lists_[0];
  recording.native = known;
  recording.device_key = 7;
  recording.object_generation = 27;
  recording.session_generation = owner.session_generation;
  recording.packets = 1;  // Owned work: never proven unrelated.
  manager->list_indices_.emplace(known, 0);
  manager->publish_list(recording);
  const auto expired = [](ht::WaitLock lock) { return ht::lock_waits[ht::lock_wait_cell(ht::submit_wait, lock)].expired.load(); };
  const auto escape_behind = [&](auto& mutex, ht::WaitLock waited, const char* label) {
    const auto before = manager->statistics();
    const auto manager_expired = expired(ht::manager_lock), submission_expired = expired(ht::submission_lock);
    std::unique_lock held(mutex);
    auto completed = std::async(std::launch::async, [&] {
      ID3D12CommandList* batch[]{known};
      const auto receipt = manager->before_submission(native_queue, 1, batch);
      if (!receipt)
        manager->forwarded_unordered(native_queue);
      return receipt;
    });
    const bool returned = completed.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
    held.unlock();
    const auto receipt = completed.get();
    const auto after = manager->statistics();
    require(returned && !receipt && after.contended_submissions == before.contended_submissions + 1 &&
                after.unordered_submissions == before.unordered_submissions + 1,
            "An ordered batch behind a held lock did not escape within its budget");
    require(expired(ht::manager_lock) == manager_expired + (waited == ht::manager_lock) &&
                expired(ht::submission_lock) == submission_expired + (waited == ht::submission_lock),
            label);
  };
  escape_behind(manager->mutex_, ht::manager_lock, "A wait1000 wait on mutex_ was not reported against the manager");
  escape_behind(manager->submission_mutex_, ht::submission_lock, "A wait1000 wait on submission_mutex_ was not reported against it");
  require(!queue.signals && !queue.waits, "An escaped batch queued a timeline operation");
}
void run() {
  void* device_table[]{reinterpret_cast<void*>(&identity), reinterpret_cast<void*>(&reference), reinterpret_cast<void*>(&reference)};
  Device device{device_table};
  auto queue_table = queue_vtable();
  // Both queues outlive the manager, which may hold a reference to either.
  Queue queue{queue_table.data(), &device}, helper_queue{queue_table.data(), &device};
  auto* native_queue = reinterpret_cast<ID3D12CommandQueue*>(&queue);
  taxi_camera::SceneHandoff handoff;
  auto manager = std::make_unique<Manager>(handoff);
  auto& owner = manager->devices_[0];
  owner.key = 7;
  owner.native = reinterpret_cast<ID3D12Device*>(&device);
  owner.active = true;
  manager->published_devices_[0].store(owner.native);
  // The fixture uses a recording sink, never a D3D12 device or GPU submission.
  std::array<std::uintptr_t, 3> markers{};
  auto* known = reinterpret_cast<ID3D12GraphicsCommandList*>(&markers[0]);
  auto* unknown = reinterpret_cast<ID3D12GraphicsCommandList*>(&markers[1]);
  auto* helper_list = reinterpret_cast<ID3D12GraphicsCommandList*>(&markers[2]);
  auto& recording = manager->lists_[0];
  recording.native = known;
  recording.device_key = 7;
  recording.object_generation = 19;
  recording.session_generation = owner.session_generation;  // A current-session recording, not a stale one.
  manager->list_indices_.emplace(known, 0);
  manager->publish_list(recording);
  auto& helper_recording = manager->lists_[1];
  helper_recording.native = helper_list;
  helper_recording.device_key = 7;
  helper_recording.object_generation = 20;
  helper_recording.session_generation = owner.session_generation;
  manager->list_indices_.emplace(helper_list, 1);
  manager->publish_list(helper_recording);
  Discovery discovery{manager.get()};
  require(manager->set_unknown_list_observer(discover, &discovery), "Install CPU-only discovery callback");
  // Uncontended: related recordings open an exact ordered transaction.
  const auto ordered = [&](ID3D12GraphicsCommandList* list, bool expect_receipt) {
    manager->publish_list(recording);
    ID3D12CommandList* batch[]{list};
    const auto receipt = manager->before_submission(native_queue, 1, batch);
    if (receipt)
      manager->after_submission(native_queue, receipt);
    else
      manager->forwarded_unordered(native_queue);  // The wrapper pairs every zero receipt.
    require((receipt != 0) == expect_receipt, "Related recordings retain exact transaction admission");
  };
  // Another thread holds submission serialization for longer than the
  // simulator thread's budget. The submit thread must return within that
  // budget: unrelated recordings bypass, everything else escapes without a
  // receipt (no Wait is queued) and publishes its invalidation after the forward.
  const auto held_lock = [&](ID3D12GraphicsCommandList* list, bool expect_discovery, bool expect_escape) {
    manager->publish_list(recording);
    discovery.called.store(false, std::memory_order_release);
    const auto before = manager->statistics();
    std::unique_lock held(manager->submission_mutex_);
    std::promise<void> entered;
    auto started = entered.get_future();
    auto completed = std::async(std::launch::async, [&] {
      ID3D12CommandList* batch[]{list};
      entered.set_value();
      const auto receipt = manager->before_submission(native_queue, 1, batch);
      if (receipt)
        manager->after_submission(native_queue, receipt);
      else
        manager->forwarded_unordered(native_queue);
      return receipt;
    });
    started.wait();
    const bool returned_while_locked = completed.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
    const bool discovered_while_locked = discovery.called.load(std::memory_order_acquire);
    held.unlock();  // Always release before checking, including regression failure.
    const auto receipt = completed.get();
    const auto after = manager->statistics();
    require(returned_while_locked, "A simulator thread was parked on held submission serialization past its budget");
    require(receipt == 0, "A batch that could not be ordered within budget invented a receipt");
    require(discovered_while_locked == expect_discovery, "Unknown discovery runs before acquiring submission serialization");
    require((after.unordered_submissions == before.unordered_submissions + 1) == expect_escape &&
                (after.contended_submissions == before.contended_submissions + 1) == expect_escape,
            "Escaped ordering was not counted exactly once, or an unrelated bypass was counted as contention");
  };
  held_lock(known, false, false);
  require(!queue.signals && !queue.waits, "Unrelated submission queues no timeline operations");
  const taxi_camera::source_state::Key source{0x1234, 1};
  require(owner.source_states.register_source(source, taxi_camera::source_state::Model::legacy_rt), "Seed ordered source-state evidence");
  {
    taxi_camera::source_state::Recording seed;
    require(seed.append({source, taxi_camera::source_state::Effect::Kind::draw}) && owner.source_states.apply(seed) &&
                owner.source_states.state(source).drawn,
            "Seed draw evidence on the fixture source");
  }
  recording.source_touched = true;
  const auto escape_before = manager->statistics();
  held_lock(known, false, true);
  manager->apply_deferred();
  // Part 10: an escaped source recording retires the live model in place and
  // rearms retained_rt on the same drain; only the draw evidence is lost. No
  // Tracker::invalidate_all, so CaptureProgress has nothing to wait 500 ms for.
  {
    const auto state = owner.source_states.state(source);
    const auto after = manager->statistics();
    require(state.model == taxi_camera::source_state::Model::legacy_rt && !state.drawn && !owner.failed,
            "A source recording escaped within budget must retire and rearm the source model in place after its forward");
    require(after.wipes == escape_before.wipes && after.source_retirements == escape_before.source_retirements + 1 &&
                (after.last_retirement_origins & Manager::OriginRefusedCompleted) &&
                after.retirement_origin_counts[1] == escape_before.retirement_origin_counts[1] + 1 &&
                after.retirement_restored == escape_before.retirement_restored + 1,
            "An escaped source recording did not count one scoped retirement through refused_completed");
  }
  require(!queue.signals && !queue.waits, "An escaped batch queued a timeline operation");
  require(owner.source_states.rearm_retained_rt() == 0, "The in-place rearm left a retained RT model unrestored");
  recording.source_touched = false;
  recording.packets = 1;
  held_lock(known, false, true);
  recording.packets = 0;
  require(!queue.signals && !queue.waits, "An escaped capture batch queued a timeline operation");
  // A consumer recording escaped by OUR expired budget keeps the device: the
  // source model is retired and rearmed in place and counted, unlike a
  // helper-contended escape, which still fails the device.
  recording.consumer = true;
  held_lock(known, false, true);
  manager->apply_deferred();
  require(!owner.failed && manager->statistics().unordered_consumers == 1,
          "A budget-expiry escape of a consumer recording failed the device for the session");
  require(owner.source_states.state(source).model == taxi_camera::source_state::Model::legacy_rt &&
              manager->statistics().wipes == escape_before.wipes &&
              (manager->statistics().last_retirement_origins & Manager::OriginUnorderedConsumer),
          "A budget-expiry consumer escape wiped the source model instead of retiring it in place");
  require(owner.source_states.rearm_retained_rt() == 0, "The consumer-escape rearm left a retained RT model unrestored");
  ordered(known, true);
  recording.consumer = false;
  recording.source_touched = true;
  ordered(known, true);
  recording.source_touched = false;
  recording.packets = 1;
  ordered(known, true);
  recording.packets = 0;
  require(queue.signals == 3 && queue.waits == 2 && !queue.future_wait && owner.last_signal == 3,
          "Consumer/source/capture paths preserve ordered Wait and Signal receipts");
  recording.awaiting_native_reset = true;
  ordered(known, false);
  held_lock(known, false, true);
  recording.awaiting_native_reset = false;
  manager->apply_deferred();
  require(owner.source_states.state(source).model == taxi_camera::source_state::Model::legacy_rt &&
              owner.source_states.rearm_retained_rt() == 0,
          "An escaped unobserved recording was not retired and rearmed in place");
  const auto wipes_before_unknown = manager->statistics().wipes;
  const auto retirements_before_unknown = manager->statistics().source_retirements;
  held_lock(unknown, true, true);
  manager->apply_deferred();
  // An escaped batch with an unregistered list is a bounded escape like any
  // other: its recordings were never applied, so the retained RT model is the
  // last positively observed one and is restored in place.
  require(owner.source_states.state(source).model == taxi_camera::source_state::Model::legacy_rt && !owner.failed,
          "An escaped unknown list wiped the source model instead of retiring it in place");
  require(manager->statistics().wipes == wipes_before_unknown &&
              manager->statistics().source_retirements == retirements_before_unknown + 1 &&
              (manager->statistics().last_retirement_origins & Manager::OriginRefusedCompleted),
          "An escaped unknown list did not name its retirement origin");
  require(queue.signals == 3 && queue.waits == 2, "Unobserved recordings never invent a receipt");
  ordered(unknown, false);
  // The ordered path still has the list in hand and knows nothing about its
  // recording: this remains a genuine wipe through unknown_lists_no_owner.
  require(owner.source_states.state(source).model == taxi_camera::source_state::Model::unknown &&
              manager->statistics().wipes == wipes_before_unknown + 1 &&
              manager->statistics().last_wipe_site == Manager::WipeSite::unknown_lists_no_owner,
          "An ordered batch with an unregistered list did not wipe through unknown_lists_no_owner");

  // A contended evidence callback never wipes the model: the skipped evidence
  // is deferred and replayed onto its exact recording by the next lock holder.
  {
    using taxi_camera::source_state::Model;
    require(owner.source_states.rearm_retained_rt() == 1, "Restore source fixture before the contended-evidence checks");
    auto* const source_resource = reinterpret_cast<ID3D12Resource*>(source.handle);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 736;
    desc.Height = 251;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    require(manager->register_source_candidate(owner.key, source_resource, source.generation, desc, Model::legacy_rt),
            "Register the fixture source as a candidate");
    const auto model = [&] { return owner.source_states.state(source).model; };
    D3D12_RESOURCE_BARRIER into_rt{};
    into_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    into_rt.Transition = {source_resource, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET};
    // Runs the call on another thread while this thread holds the manager lock
    // past every evidence budget; the call must return and report contention.
    const auto contended = [&](auto&& call, const char* label) {
      std::unique_lock held(manager->mutex_);
      auto result = std::async(std::launch::async, [&] {
        call();
        return Manager::last_call_contended();
      });
      const bool returned = result.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
      held.unlock();
      const bool was_contended = result.get();
      require(returned && was_contended, label);
    };
    const auto before = manager->statistics();
    contended([&] { manager->observe_source_legacy(known, recording.object_generation, into_rt); },
              "A barrier observer was parked on the held manager lock past its budget");
    contended(
        [&] {
          manager->invalidate_source_recording(known, recording.object_generation, true,
                                               taxi_camera::engine_hook::render_boundary::InvalidationPassState);
        },
        "An invalidation observer was parked on the held manager lock past its budget");
    auto after = manager->statistics();
    require(after.contended_evidence == before.contended_evidence + 2 && after.deferred_evidence == before.deferred_evidence + 2,
            "Contended evidence callbacks were not counted and deferred");
    require(after.wipes == before.wipes && model() == Model::legacy_rt, "A contended evidence callback wiped the source model");
    manager->apply_deferred();
    after = manager->statistics();
    require(after.wipes == before.wipes && model() == Model::legacy_rt, "Replaying deferred evidence wiped the source model");
    require(recording.source_touched && !recording.source_effects.invalid && recording.source_effects.count == 1 &&
                recording.source_effects.effects[0].key == source &&
                recording.source_effects.effects[0].kind == taxi_camera::source_state::Effect::Kind::legacy_rt,
            "Deferred barrier evidence was not replayed onto its recording");
    require(after.ignored_source_recordings == before.ignored_source_recordings + 1,
            "A deferred PassState report on a list that never named a published source was not ignored");
    ordered(known, true);
    require(model() == Model::legacy_rt && !owner.failed && manager->statistics().wipes == before.wipes,
            "Executing a recording with replayed evidence wiped the model");

    // A Reset skipped on its thread retires before any later evidence on the
    // same list and before any transaction; the completed recording keeps the model.
    const auto resets = manager->statistics().resets;
    contended([&] { manager->successful_reset(known, recording.object_generation); },
              "successful_reset was parked on the held manager lock past its budget");
    after = manager->statistics();
    require(after.deferred_retirements == before.deferred_retirements + 1 && after.resets == resets, "A contended Reset was not deferred");
    require(after.wipes == before.wipes && model() == Model::legacy_rt, "A deferred Reset wiped the source model");
    manager->observe_source_legacy(known, recording.object_generation, into_rt);  // Uncontended: drains the Reset first.
    require(manager->statistics().resets == resets + 1 && recording.source_touched && recording.source_effects.count == 1,
            "The deferred Reset did not retire the recording before later evidence was appended");
    ordered(known, true);
    require(model() == Model::legacy_rt && !owner.failed && manager->statistics().wipes == before.wipes,
            "A deferred Reset followed by normal completion wiped the model");

    // Ring overflow is the one skipped path that may still wipe: a lost entry
    // may have been a Reset, so every current recording becomes inapplicable.
    {
      const auto overflow_before = manager->statistics();
      std::unique_lock held(manager->mutex_);
      auto flood = std::async(std::launch::async, [&] {
        for (unsigned i = 0; i < 260; ++i)
          manager->observe_source_legacy(known, recording.object_generation, into_rt);
      });
      const bool returned = flood.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
      held.unlock();
      flood.get();
      require(returned, "Flooding contended evidence parked the recording thread");
      manager->apply_deferred();
      const auto overflowed = manager->statistics();
      require(overflowed.deferred_overflows == overflow_before.deferred_overflows + 1, "Ring overflow was not counted");
      require(overflowed.wipes == overflow_before.wipes + 1 && overflowed.last_wipe_site == Manager::WipeSite::deferred_sources &&
                  (overflowed.last_wipe_origins & Manager::OriginDeferredOverflow),
              "Ring overflow did not wipe exactly once through deferred_sources with its origin");
      require(recording.source_effects.invalid && model() == Model::unknown, "Ring overflow left a possibly stale recording applicable");
      recording.source_effects.reset();
      recording.source_touched = false;
      helper_recording.source_effects.reset();
      helper_recording.source_touched = false;
      manager->publish_list(recording);
      manager->publish_list(helper_recording);
      require(owner.source_states.rearm_retained_rt() == 1, "Restore source fixture after the overflow");
    }
    recording.source_touched = false;
    recording.source_effects.reset();
    manager->publish_list(recording);
  }

  auto* helper_native = reinterpret_cast<ID3D12CommandQueue*>(&helper_queue);
  recording.source_touched = true;
  manager->publish_list(recording);
  ID3D12CommandList* batch[]{known};
  const auto receipt = manager->before_submission(native_queue, 1, batch);
  require(receipt != 0, "Begin an ordinary source receipt before cross-queue helper");
  ID3D12CommandList* helper_batch[]{helper_list};
  std::unique_lock native_tail(manager->mutex_);
  auto helper = std::async(std::launch::async, [&] {
    const auto result = manager->before_submission(helper_native, 1, helper_batch);
    if (result)
      manager->after_submission(helper_native, result);
    return result;
  });
  const bool helper_returned = helper.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
  auto private_work = std::async(std::launch::async, [&] {
    const auto result = manager->begin_private_submission(owner.key, helper_native);
    if (result.receipt)
      manager->end_private_submission(result.receipt);
    return result;
  });
  const bool private_returned = private_work.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
  // Always release the original receipt before assertions, even on regression.
  native_tail.unlock();
  manager->after_submission(native_queue, receipt);
  const auto helper_receipt = helper.get();
  const auto private_result = private_work.get();
  require(helper_returned && !helper_receipt && !helper_queue.signals && !helper_queue.waits,
          "A proven unrelated helper on another queue cannot wait for its owner Execute or metadata lock");
  require(private_returned && private_result.deferred && !private_result.receipt,
          "Private composition defers without queuing work behind a held submission");
  require(!owner.failed, "Harmless cross-queue contention does not fail the device");

  const auto promptly = [&](auto&& action, const char* message) {
    std::unique_lock held(manager->mutex_);
    auto result = std::async(std::launch::async, action);
    const bool returned = result.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
    held.unlock();
    result.get();
    require(returned, message);
  };
  promptly(
      [&] {
        manager->submission_refused_batch(native_queue, taxi_camera::engine_hook::queue_submit::Refusal::contended_submission, 1, batch);
      },
      "Exact contended refusal does not wait on manager lock held by native tail Execute");
  promptly([&] { manager->submission_refused(native_queue, taxi_camera::engine_hook::queue_submit::Refusal::contended_submission); },
           "Legacy contended refusal also returns while native tail holds the manager lock");
  promptly(
      [&] {
        const auto result = manager->begin_private_submission(owner.key, helper_native);
        require(result.deferred && !result.receipt, "Private metadata contention is retryable");
      },
      "Private composition cannot wait for native tail metadata lock");
  manager->apply_deferred();
  require(!owner.failed, "Source-only refusals recover without permanently failing capture");
  // The legacy contended refusal above published refused_contended: retired
  // and rearmed in place, so nothing is left for a later rearm to restore.
  require(owner.source_states.state(source).model == taxi_camera::source_state::Model::legacy_rt &&
              (manager->statistics().last_retirement_origins & Manager::OriginRefusedContended) &&
              owner.source_states.rearm_retained_rt() == 0,
          "A legacy contended refusal wiped the source model instead of retiring it in place");
  const auto retirements_before_completion = manager->statistics().source_retirements;
  const auto post_token =
      manager->submission_refused_batch(native_queue, taxi_camera::engine_hook::queue_submit::Refusal::contended_submission, 1, batch);
  manager->apply_deferred();
  require(owner.source_states.state(source).model == taxi_camera::source_state::Model::legacy_rt &&
              manager->statistics().source_retirements == retirements_before_completion,
          "Source refusal is not prematurely consumed before native forward");
  manager->submission_refused_completed(post_token);
  manager->apply_deferred();
  require(owner.source_states.state(source).model == taxi_camera::source_state::Model::legacy_rt &&
              manager->statistics().source_retirements == retirements_before_completion + 1 &&
              (manager->statistics().last_retirement_origins & Manager::OriginRefusedCompleted),
          "Source completion did not retire the proof in place after the exact native batch returned");

  // Reproduce the classification-to-notification gap: mark the exact recording,
  // pause before its wake flag is posted, then Reset before deferred processing.
  recording.packets = 1;
  manager->packets_[0].assigned = true;
  manager->packets_[0].device_key = owner.key;
  manager->publish_list(recording);
  auto& publication = manager->published_lists_[0];
  constexpr std::uint64_t escaped = 1u << 28;
  const auto old_word = publication.effects.fetch_or(escaped);
  manager->deferred_recordings_.store(false);  // Notifier has not posted its wake bit.
  manager->retire_native_list(recording, false);  // retire_list resets fields; this publishes once.
  require(manager->packets_[0].quarantined && manager->packets_[0].assigned && manager->packets_[0].retired,
          "Reset consumes the exact recording mark before its packet can be recycled");
  manager->packets_[1].assigned = true;
  manager->packets_[1].device_key = owner.key;
  recording.packets = 2;
  manager->publish_list(recording);
  auto stale = old_word;
  require(!publication.effects.compare_exchange_strong(stale, old_word | escaped),
          "A delayed notice cannot mark the replacement recording version");
  manager->deferred_recordings_.store(true);  // Old notifier now posts its wake bit.
  manager->apply_deferred();
  require(!manager->packets_[1].quarantined && !owner.failed, "Delayed wake cannot quarantine replacement packet or unrelated device");
  manager->submission_refused_batch(native_queue, taxi_camera::engine_hook::queue_submit::Refusal::contended_submission, 1, batch);
  manager->apply_deferred();
  require(manager->packets_[1].quarantined && !owner.failed,
          "Exact escaped snapshot quarantines only its packet while source capture remains recoverable");
  recording.consumer = true;
  manager->publish_list(recording);
  manager->submission_refused_batch(native_queue, taxi_camera::engine_hook::queue_submit::Refusal::contended_submission, 1, batch);
  manager->apply_deferred();
  require(owner.failed && manager->packets_[0].assigned && manager->packets_[1].assigned,
          "Lost stable-output consumer ordering freezes that device and retains its GPU storage");
  auto& exhausted = manager->published_lists_[1];
  exhausted.effects.store(0xffffffff00000000ull | (1u << 24));
  manager->publish_list(helper_recording);
  const auto saturated = exhausted.effects.load();
  manager->publish_list(helper_recording);
  require((saturated >> 32) == UINT32_MAX && (saturated & (1u << 29)) && exhausted.effects.load() == saturated &&
              !manager->classify_unobserved(helper_native, 1, helper_batch).unrelated,
          "Publication version exhaustion stays permanently conservative instead of wrapping to an ABA match");
  reshade_present_gate();
  receipts_without_source_work();
  clean_reset_fast_path();
  ordered_waits_name_their_lock();
  std::printf(
      "PASS CPU-only submission locks: cross-queue helpers, nonblocking refusals/private deferral, exact retirement marks, GPU "
      "guards and lock-free clean Resets.\n");
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
void tail_legacy(void* context,
                 ID3D12GraphicsCommandList* list,
                 std::uint64_t generation,
                 const D3D12_RESOURCE_BARRIER& value,
                 std::uint32_t) noexcept {
  static_cast<TailContext*>(context)->manager->observe_source_legacy(list, generation, value);
}
void tail_enhanced(void* context,
                   ID3D12GraphicsCommandList7* list,
                   std::uint64_t generation,
                   const D3D12_TEXTURE_BARRIER& value,
                   std::uint32_t) noexcept {
  static_cast<TailContext*>(context)->manager->observe_source_enhanced(list, generation, value);
}
void tail_draw(void* context, ID3D12GraphicsCommandList* list, std::uint64_t generation, bool allowed) noexcept {
  static_cast<TailContext*>(context)->manager->after_source_draw(list, generation, allowed);
}
// Forwards to a native queue and records the list count of every
// ExecuteCommandLists call made through it. GetDevice reports the native
// device, so the manager accepts it as the receipt's queue.
class CountingQueue final : public ID3D12CommandQueue {
 public:
  explicit CountingQueue(ID3D12CommandQueue* native) noexcept : native_(native) {}
  std::vector<UINT> executions;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override { return native_->QueryInterface(iid, result); }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override { return --references_; }  // Never deleted: lives until exit.
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
    return native_->GetPrivateData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
    return native_->SetPrivateData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override {
    return native_->SetPrivateDataInterface(guid, data);
  }
  HRESULT STDMETHODCALLTYPE SetName(const WCHAR* name) override { return native_->SetName(name); }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** device) override { return native_->GetDevice(iid, device); }
  void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource* resource,
                                            UINT region_count,
                                            const D3D12_TILED_RESOURCE_COORDINATE* starts,
                                            const D3D12_TILE_REGION_SIZE* sizes,
                                            ID3D12Heap* heap,
                                            UINT range_count,
                                            const D3D12_TILE_RANGE_FLAGS* flags,
                                            const UINT* offsets,
                                            const UINT* counts,
                                            D3D12_TILE_MAPPING_FLAGS mapping) override {
    native_->UpdateTileMappings(resource, region_count, starts, sizes, heap, range_count, flags, offsets, counts, mapping);
  }
  void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource* destination,
                                          const D3D12_TILED_RESOURCE_COORDINATE* destination_start,
                                          ID3D12Resource* source,
                                          const D3D12_TILED_RESOURCE_COORDINATE* source_start,
                                          const D3D12_TILE_REGION_SIZE* size,
                                          D3D12_TILE_MAPPING_FLAGS flags) override {
    native_->CopyTileMappings(destination, destination_start, source, source_start, size, flags);
  }
  void STDMETHODCALLTYPE ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists) override {
    executions.push_back(count);
    native_->ExecuteCommandLists(count, lists);
  }
  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void* data, UINT size) override { native_->SetMarker(metadata, data, size); }
  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void* data, UINT size) override { native_->BeginEvent(metadata, data, size); }
  void STDMETHODCALLTYPE EndEvent() override { native_->EndEvent(); }
  HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* fence, UINT64 value) override { return native_->Signal(fence, value); }
  HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* fence, UINT64 value) override { return native_->Wait(fence, value); }
  HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* frequency) override { return native_->GetTimestampFrequency(frequency); }
  HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* gpu, UINT64* cpu) override { return native_->GetClockCalibration(gpu, cpu); }
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  using ID3D12CommandQueue::GetDesc;
  D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE GetDesc(D3D12_COMMAND_QUEUE_DESC* result) override {
    *result = native_->GetDesc();
    return result;
  }
#else
  D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return native_->GetDesc(); }
#endif

 private:
  ID3D12CommandQueue* native_;
  ULONG references_ = 1;
};
static_assert(Manager::TailFeeds >= taxi_camera::native_camera::kMaxCameraFeeds, "A receipt must be able to capture every camera feed");
// Three published feeds drawn in ONE receipt, as when a split-display profile's
// three views share an ExecuteCommandLists (not yet seen live). Every feed's
// tail is recorded, the three run in one ExecuteCommandLists after the
// application's, and each snapshot holds its own feed's last draw; a two-list
// batch left the third snapshot unwritten but published. A manager and handoff
// of its own keep the two-feed receipt counts unchanged. Needs the boundary
// observer enabled (render_boundary::operational).
void three_feed_tail(ID3D12Device* device, DrawFixture& draw) {
  constexpr std::uint64_t DeviceKey = 77, Generation = 7070;
  auto* handoff = new taxi_camera::SceneHandoff;
  auto* manager = new Manager(*handoff);  // Lives until exit, like the receipts above.
  handoff->register_device(DeviceKey);
  require(manager->register_device(DeviceKey, device), "Register three-feed device");
  Commands producer, consumer;
  producer.initialize(device);
  consumer.initialize(device);
  auto* queue = new CountingQueue(producer.queue.p);
  require(manager->register_command_list(producer.list.p, DeviceKey, Generation), "Register three-feed producer");
  Ref<ID3D12GraphicsCommandList7> list7;
  check(producer.list->QueryInterface(IID_PPV_ARGS(list7.put())), "Get three-feed producer interface7");
  Ref<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = 3;
  check(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(rtvs.put())), "Create three-feed RTV heap");
  const auto rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  constexpr UINT width = 736;
  constexpr std::array<UINT, 3> heights{251, 496, 496};  // Nose, left and right panes of the split profiles.
  constexpr std::array<std::uint64_t, 3> ids{39265, 39266, 39267};
  constexpr float colors[3][4] = {{0.25f, 0.5f, 0.75f, 1}, {2, 4, 0.125f, 1}, {0.125f, 1, 2, 1}};
  constexpr std::array<std::uint32_t, 3> words{0x340u | (0x380u << 11) | (0x1d0u << 22), 0x400u | (0x440u << 11) | (0x180u << 22),
                                               0x300u | (0x3c0u << 11) | (0x200u << 22)};
  std::array<Ref<ID3D12Resource>, 3> sources, readbacks;
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3> handles{};
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 3> footprints{};
  std::array<UINT64, 3> readback_bytes{};
  for (unsigned feed = 0; feed < 3; ++feed) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = heights[feed];
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto default_heap = heap(D3D12_HEAP_TYPE_DEFAULT);
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                          IID_PPV_ARGS(sources[feed].put())),
          "Create three-feed source");
    handoff->register_resource(DeviceKey, reinterpret_cast<std::uint64_t>(sources[feed].p), ids[feed]);
    require(manager->register_source_candidate(DeviceKey, sources[feed].p, ids[feed], desc, taxi_camera::source_state::Model::legacy_rt),
            "Register three-feed candidate");
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
                                          IID_PPV_ARGS(readbacks[feed].put())),
          "Create three-feed readback");
  }
  handoff->begin_scene();
  const auto ticket = handoff->begin_capture();
  require(handoff->publish(ticket, {45, 3}, {9101, 9102, 9103},
                           {reinterpret_cast<std::uint64_t>(sources[0].p), reinterpret_cast<std::uint64_t>(sources[1].p),
                            reinterpret_cast<std::uint64_t>(sources[2].p)}),
          "Publish three feeds");
  manager->begin_source_tracking();
  for (unsigned feed = 0; feed < 3; ++feed) {
    ID3D12Resource* target = sources[feed].p;
    const float first_color[]{1, 0, 1, 1};
    draw.record(list7.p, handles[feed], first_color, false, width, heights[feed]);
    manager->observe_source_draw_after(producer.list.p, Generation, 1, &target, &ids[feed], true);
    draw.record(list7.p, handles[feed], colors[feed], false, width, heights[feed]);
    manager->observe_source_draw_after(producer.list.p, Generation, 1, &target, &ids[feed], true);
  }
  check(producer.list->Close(), "Close three-feed recording");
  ID3D12CommandList* batch[]{producer.list.p};
  const auto receipt = manager->before_submission(queue, 1, batch);
  require(receipt != 0, "Three drawn feeds opened no ordered receipt");
  queue->ExecuteCommandLists(1, batch);  // The application's own call, between before and after.
  manager->after_submission(queue, receipt);
  const auto statistics = manager->statistics();
  require(statistics.tail_submissions == 3 && statistics.tail_captures == 3 && statistics.submissions == 1 &&
              std::strcmp(statistics.tail_status, "captured") == 0,
          "One receipt did not capture all three drawn feeds");
  for (unsigned feed = 0; feed < 3; ++feed)
    require(statistics.phases[feed].captures[static_cast<std::size_t>(taxi_camera::capture_phase::Kind::after_multi)] == 1,
            "A feed's capture was not counted after its complete render");
  require(queue->executions == std::vector<UINT>{1, 3},
          "The receipt's three tails did not run in one ExecuteCommandLists after the application's");
  std::array<Manager::Frame, 3> frames{};
  std::size_t frame_count = 0;
  wait([&] {
    frame_count += manager->poll_completed_frames(frames.data() + frame_count, frames.size() - frame_count);
    return frame_count == frames.size();
  });
  unsigned seen = 0;
  for (const auto& result : frames) {
    const auto feed = result.match.feed;
    require(feed < 3 && !(seen & (1u << feed)) && result.order.submission == 1, "Bad or repeated three-feed snapshot");
    seen |= 1u << feed;
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
  check(consumer.list->Close(), "Close three-feed consumer");
  const auto consume = manager->begin_private_submission(DeviceKey, consumer.queue.p);
  require(consume.receipt != 0, "Begin three-feed consumer");
  ID3D12CommandList* read = consumer.list.p;
  consumer.queue->ExecuteCommandLists(1, &read);
  require(manager->end_private_submission(consume.receipt), "Submit three-feed consumer");
  for (const auto& result : frames)
    require(manager->finish_consumption(result.token, consume.fence, consume.value), "Finish three-feed consumer lease");
  Ref<ID3D12Fence> completed;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(completed.put())), "Create three-feed fence");
  check(consumer.queue->Signal(completed.p, 1), "Signal three-feed completion");
  wait([&] { return completed->GetCompletedValue() >= 1; });
  for (unsigned feed = 0; feed < 3; ++feed) {
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(readback_bytes[feed])};
    check(readbacks[feed]->Map(0, &range, &mapped), "Map three-feed pixels");
    bool exact = true;
    for (UINT y = 0; y < heights[feed]; ++y)
      for (UINT x = 0; x < width; ++x) {
        std::uint32_t pixel = 0;
        std::memcpy(
            &pixel,
            static_cast<const std::uint8_t*>(mapped) + footprints[feed].Offset + UINT64(y) * footprints[feed].Footprint.RowPitch + x * 4,
            4);
        exact &= pixel == words[feed];
      }
    const D3D12_RANGE empty{0, 0};
    readbacks[feed]->Unmap(0, &empty);
    require(exact, "A three-feed snapshot is not its own feed's last draw");
  }
  require(SUCCEEDED(device->GetDeviceRemovedReason()), "Three-feed GPU work removed device");
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
  {
    // Feeds of different sizes alternate. A capture must reuse the idle packet
    // that already holds its shape, then an empty slot, and release another
    // shape's texture only last; first-fit recreated one per alternation.
    const auto shape = [](UINT64 width, UINT height) {
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = width;
      desc.Height = height;
      desc.DepthOrArraySize = desc.MipLevels = 1;
      desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      desc.SampleDesc.Count = 1;
      return desc;
    };
    const auto nose = shape(64, 32), tail = shape(32, 32);
    auto& packets = manager->packets_;
    const auto last = packets.size() - 1;
    for (const auto& [index, desc] : {std::pair{std::size_t{0}, nose}, std::pair{std::size_t{1}, tail}}) {
      check(packets[index].gpu.initialize(device.p, desc), "Initialize a shaped idle packet");
      packets[index].description = desc;
      packets[index].device_key = DeviceKey;
    }
    auto order = manager->reuse_order(DeviceKey, tail);
    require(order[0] == 1 && order[1] == 2 && order[last] == 0, "Tail capture reuses its idle shape before an empty slot or the nose's");
    order = manager->reuse_order(DeviceKey, nose);
    require(order[0] == 0 && order[1] == 2 && order[last] == 1, "Nose capture reuses its idle shape and leaves the tail's");
    order = manager->reuse_order(DeviceKey + 1, nose);
    require(order[0] == 2 && order[last - 1] == 0 && order[last] == 1, "Another device's idle packets are reused only after empty slots");
    unsigned seen = 0;
    for (const auto index : order)
      seen |= 1u << index;
    require(seen == (1u << packets.size()) - 1, "Reuse order visits every packet slot once");
    for (const std::size_t index : {0u, 1u}) {
      require(packets[index].gpu.release_idle(), "Release the shaped test packet");
      packets[index].description = {};
      packets[index].device_key = 0;
    }
  }
  require(manager->source_rate_ == taxi_camera::kDefaultCameraRate, "Capture rate default remains the shipped default");
  for (const auto setting : std::array<std::array<std::uint32_t, 2>, 9>{
           {{0, 1}, {1, 1}, {2, 2}, {4, 4}, {14, 14}, {20, 20}, {60, 60}, {61, 60}, {0xffffffffu, 60}}}) {
    // Parked floors go below the moving minimum, so capture spacing follows the
    // schedule's 1..60 clamp rather than the saved camera_rate's 5..60.
    manager->set_source_rate(setting[0]);
    require(manager->source_rate_ == setting[1], "Capture rate follows the shared 1..60 schedule clamp");
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
  callbacks.recording_invalidated = [](void* raw, ID3D12GraphicsCommandList* native, std::uint64_t generation,
                                       std::uint32_t reasons) noexcept {
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
      check(device10->CreateCommittedResource3(&default_heap, D3D12_HEAP_FLAG_NONE, &desc1, D3D12_BARRIER_LAYOUT_RENDER_TARGET, nullptr,
                                               nullptr, 0, nullptr, IID_PPV_ARGS(sources[feed].put())),
            "Create persistent source directly in enhanced RT layout");
    } else {
      check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            born_render_target ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(sources[feed].put())),
            "Create persistent tail source");
    }
    auto* marker = new SourceLifetimeMarker(queue_context->source_deaths);
    const GUID lifetime_id{0xe7bd8641, 0xea37, 0x451c, {0x8f, 0x64, 0x46, 0xe4, 0x8a, 0x39, 0x22, 0x80}};
    check(sources[feed]->SetPrivateDataInterface(lifetime_id, marker), "Attach source destruction witness");
    marker->Release();
    handoff->register_resource(DeviceKey, reinterpret_cast<std::uint64_t>(sources[feed].p), ids[feed]);
    const auto initial_model = !born_render_target ? taxi_camera::source_state::Model::unknown
                               : enhanced          ? taxi_camera::source_state::Model::enhanced_rt
                                                   : taxi_camera::source_state::Model::legacy_rt;
    require(manager->register_source_candidate(DeviceKey, sources[feed].p, ids[feed], desc, initial_model),
            "Register exact pane candidate");
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
                                          IID_PPV_ARGS(readbacks[feed].put())),
          "Create tail test readback");
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
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                          IID_PPV_ARGS(unrelated_source.put())),
          "Create unrelated pane-sized pass target");
    require(manager->register_source_candidate(DeviceKey, unrelated_source.p, UnrelatedGeneration, desc,
                                               taxi_camera::source_state::Model::legacy_rt),
            "Register unrelated pass target");
    // The source prefilter is exact. An application target whose filter bits
    // are set (a hash collision, or bits of a retired candidate) is not a
    // source, so its draws never take the manager lock; a retired candidate's
    // bits are cleared when collect() rebuilds the filter.
    const auto live_union = [&](std::size_t word) {
      std::uint64_t bits = 0;
      for (const auto& source : manager->sources_)
        if (source.native && Manager::source_filter_bits(source.native).word == word)
          bits |= Manager::source_filter_bits(source.native).mask;
      return bits;
    };
    auto* colliding = reinterpret_cast<ID3D12Resource*>(std::uintptr_t{0x7ff012345670});  // Never dereferenced.
    const auto colliding_bits = Manager::source_filter_bits(colliding);
    manager->source_filter_[colliding_bits.word].fetch_or(colliding_bits.mask);
    require(!manager->may_be_source(colliding), "A colliding filter bit made an application target a camera source");
    require(manager->may_be_source(sources[0].p) && manager->may_be_source(sources[1].p) && manager->may_be_source(unrelated_source.p),
            "A registered candidate failed the exact source check");
    Ref<ID3D12Resource> retired_source;
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                          IID_PPV_ARGS(retired_source.put())),
          "Create short-lived pane-sized target");
    constexpr std::uint64_t RetiredGeneration = UnrelatedGeneration + 1;
    require(manager->register_source_candidate(DeviceKey, retired_source.p, RetiredGeneration, desc,
                                               taxi_camera::source_state::Model::legacy_rt) &&
                manager->may_be_source(retired_source.p),
            "Register short-lived candidate");
    const auto retired_bits = Manager::source_filter_bits(retired_source.p);
    manager->unregister_source_candidate(DeviceKey, retired_source.p, RetiredGeneration);
    require(!manager->may_be_source(retired_source.p), "A retired candidate stayed a source before collection");
    {
      const std::lock_guard lock(manager->mutex_);
      manager->collect();
    }
    require((manager->source_filter_[retired_bits.word].load() & retired_bits.mask & ~live_union(retired_bits.word)) == 0 &&
                (manager->source_filter_[colliding_bits.word].load() & colliding_bits.mask & ~live_union(colliding_bits.word)) == 0,
            "Collecting a retired candidate kept stale filter bits");
    require(manager->may_be_source(sources[0].p) && manager->may_be_source(sources[1].p) && manager->may_be_source(unrelated_source.p),
            "Rebuilding the filter dropped a live candidate");
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
  require(manager->statistics().capture_copy_gpu.samples == 0, "GPU capture timing default inactive");
  manager->set_gpu_timing_enabled(true);
  {
    const auto model = [&](unsigned feed) {
      return manager->device(DeviceKey)->source_states.state({reinterpret_cast<std::uint64_t>(sources[feed].p), ids[feed]}).model;
    };
    const auto before_left = model(0);
    const auto before_right = model(1);
    require(before_left != taxi_camera::source_state::Model::unknown && before_right != taxi_camera::source_state::Model::unknown,
            "Camera sources had no RT evidence before the PassState check");
    Commands bystander;
    bystander.initialize(device.p);
    constexpr std::uint64_t BystanderGeneration = Generation + 9;
    require(manager->register_command_list(bystander.list.p, DeviceKey, BystanderGeneration),
            "Register list that never observed published camera sources");
    auto* untouched = manager->list(bystander.list.p);
    require(untouched, "Bystander recording missing");
    const auto ignored_before = manager->statistics().ignored_source_recordings;
    const auto require_ignored = [&](std::uint32_t reasons, const char* label) {
      const auto ignored = manager->statistics().ignored_source_recordings;
      manager->invalidate_source_recording(bystander.list.p, BystanderGeneration, true, reasons);
      require(!untouched->source_effects.invalid && !untouched->source_touched, label);
      require(manager->statistics().ignored_source_recordings == ignored + 1, "Ignored limited report was not counted");
      require(manager->statistics().last_invalidation_reasons == reasons, "Ignored limited report did not record its reasons");
      ID3D12CommandList* batch = bystander.list.p;
      require(manager->before_submission(bystander.queue.p, 1, &batch) == 0, label);
      require(model(0) == before_left && model(1) == before_right, label);
    };
    require_ignored(Boundary::InvalidationPassBegin,
                    "PassBegin on a list that never named a published camera source prepared a global wipe");
    require_ignored(Boundary::InvalidationPassState,
                    "PassState on a list that never named a published camera source prepared a global wipe");
    require_ignored(Boundary::InvalidationUnobservedWork,
                    "Unsupported command on a list that never named a published camera source prepared a global wipe");
    manager->invalidate_source_recording(bystander.list.p, BystanderGeneration, true, Boundary::InvalidationBarrierBatch);
    require(untouched->source_effects.invalid && untouched->source_touched,
            "Barrier invalidation no longer discards an uncertain recording");
    manager->successful_reset(bystander.list.p, BystanderGeneration);
    manager->invalidate_source_recording(bystander.list.p, BystanderGeneration, true,
                                         Boundary::InvalidationPassBegin | Boundary::InvalidationObserverDisabled);
    require(untouched->source_effects.invalid && untouched->source_touched, "Observer-disabled PassBegin no longer discards the recording");
    require(manager->statistics().ignored_source_recordings == ignored_before + 3, "Discarding reports were counted as ignored");
    manager->successful_reset(bystander.list.p, BystanderGeneration);
    manager->invalidate_source_recording(bystander.list.p, BystanderGeneration, true,
                                         Boundary::InvalidationUnobservedWork | Boundary::InvalidationResetFailed);
    require(untouched->source_effects.invalid && untouched->source_touched,
            "Reset failure combined with an unsupported command no longer discards the recording");
    manager->successful_reset(bystander.list.p, BystanderGeneration);
    const auto require_scoped = [&](std::uint32_t reasons, const char* label) {
      require(untouched->source_effects.append(
                  {{reinterpret_cast<std::uint64_t>(sources[0].p), ids[0]}, taxi_camera::source_state::Effect::Kind::legacy_rt}),
              label);
      manager->invalidate_source_recording(bystander.list.p, BystanderGeneration, true, reasons);
      require(!untouched->source_effects.invalid, label);
      require(untouched->source_touched, label);
      bool retired = false;
      for (std::size_t index = 0; index < untouched->source_effects.count; ++index) {
        const auto& effect = untouched->source_effects.effects[index];
        retired |= effect.key.handle == reinterpret_cast<std::uint64_t>(sources[0].p) &&
                   effect.kind == taxi_camera::source_state::Effect::Kind::pass_other;
      }
      require(retired, label);
      require(model(0) == before_left && model(1) == before_right, label);
      manager->successful_reset(bystander.list.p, BystanderGeneration);
    };
    require_scoped(Boundary::InvalidationPassBegin, "PassBegin globally invalidated a recording that named one published camera source");
    require_scoped(Boundary::InvalidationPassState, "PassState globally invalidated a recording that named one published camera source");
    require_scoped(Boundary::InvalidationUnobservedWork,
                   "Unsupported command globally invalidated a recording that named one published camera source");
  }
  DrawFixture draw;
  draw.initialize(device.p, DXGI_FORMAT_R11G11B10_FLOAT);
  constexpr float colors[2][2][4] = {{{0.25f, 0.5f, 0.75f, 1}, {2, 4, 0.125f, 1}}, {{1, 0, 0.5f, 1}, {0.125f, 1, 2, 1}}};
  constexpr std::array<std::array<std::uint32_t, 2>, 2> words{
      {{0x340u | (0x380u << 11) | (0x1d0u << 22), 0x400u | (0x440u << 11) | (0x180u << 22)},
       {0x3c0u | (0x1c0u << 22), 0x300u | (0x3c0u << 11) | (0x200u << 22)}}};
  std::uint64_t checked_pixels = 0;
  for (unsigned frame = 0; frame < 3; ++frame) {
    Sleep(70);  // Production15..20Hz cap also applies to this real GPU fixture.
    if (frame == 1) {
      const auto before_idle = manager->statistics().tail_captures;
      manager->set_capture_enabled(false);
      producer.queue->ExecuteCommandLists(1, &original);
      drain(producer.queue.p);
      require(manager->statistics().tail_captures == before_idle, "Idle admitted new capture tails");
      manager->set_capture_enabled(true);
    }
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
    require(
        manager->device(DeviceKey)->source_states.state({reinterpret_cast<std::uint64_t>(unrelated_source.p), UnrelatedGeneration}).model ==
            taxi_camera::source_state::Model::other,
        "Scoped target invalidation was not applied to its exact source");
    require(!queue_context->reset_failed && queue_context->receipt_lease_proven,
            "Receipt failed to retain actual sources across Reset and application reference release");
    std::array<Manager::Frame, 2> frames{};
    std::size_t frame_count = 0;
    wait([&] {
      frame_count += manager->poll_completed_frames(frames.data() + frame_count, frames.size() - frame_count);
      return frame_count == 2;
    });
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
          std::memcpy(
              &pixel,
              static_cast<const std::uint8_t*>(mapped) + footprints[feed].Offset + UINT64(y) * footprints[feed].Footprint.RowPitch + x * 4,
              4);
          require(pixel == words[frame == 0 ? 0 : 1][feed], "Tail snapshot is not the last actual draw's packed RGB");
          ++checked_pixels;
        }
      const D3D12_RANGE empty{0, 0};
      readbacks[feed]->Unmap(0, &empty);
    }
  }
  require(manager->statistics().tail_captures == 6, "Unexpected persistent/replay tail captures");
  const auto capture_timing = manager->statistics().capture_copy_gpu;
  require(capture_timing.samples == 6 && capture_timing.rejected == 0 && capture_timing.total_ms >= capture_timing.maximum_ms,
          "Private tail timing did not measure each completed copy exactly once");
  require(manager->statistics().capture_copy_gpu.samples == 6, "Tail timing polling double counted samples");
  manager->set_gpu_timing_enabled(false);
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
  {
    // Capture spacing. The schedule opens a feed on GetTickCount64 at observer
    // time, so a parked render it made due can reach the tail a little inside
    // the interval: it is captured, not discarded. A render well inside the
    // interval, a second batch of a captured render and, from rate 10 up, any
    // render inside the full interval are refused.
    struct Case {
      std::uint32_t rate;
      std::uint64_t elapsed_us;
      bool captured;
      const char* label;
    };
    const auto steady_us = [] {
      return static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    const auto submit = [&] {
      const auto before = manager->statistics().tail_captures;
      producer.queue->ExecuteCommandLists(1, &original);
      return manager->statistics().tail_captures - before;
    };
    for (const auto& item : {Case{2, 250000, false, "A parked render at half the interval was captured"},
                             Case{2, 450000, true, "A due parked render 0.9 of the interval after the last capture was discarded"},
                             Case{1, 800000, false, "The rate 1 tolerance exceeded 125 ms"},
                             Case{1, 900000, true, "A due rate 1 render inside the 125 ms tolerance was discarded"},
                             Case{5, 160000, true, "A due rate 5 render inside the tolerance was discarded"},
                             Case{10, 76000, false, "The rate 10 capture interval was shortened"}}) {
      manager->set_source_rate(item.rate);
      const auto previous = steady_us() - item.elapsed_us;
      manager->last_tail_us_[0] = manager->last_tail_us_[1] = previous;
      const auto captured = submit();
      if (!item.captured) {
        require(captured == 0 && std::strcmp(manager->statistics().tail_status, "sample_interval") == 0, item.label);
        require(manager->last_tail_us_[0] == previous && manager->last_tail_us_[1] == previous, item.label);
        continue;
      }
      require(captured == 2 && std::strcmp(manager->statistics().tail_status, "captured") == 0, item.label);
      require(submit() == 0 && std::strcmp(manager->statistics().tail_status, "sample_interval") == 0,
              "A feed was captured twice within one interval");
      drain(producer.queue.p);
      std::size_t completed = 0;
      wait([&] {
        completed += manager->poll_completed_frames(unused.data() + completed, unused.size() - completed);
        return completed >= 2;
      });
      for (std::size_t i = 0; i < completed; ++i)
        require(manager->discard_frame(unused[i].token), "Discard spacing-check frame");
    }
    manager->set_source_rate(20);
  }
  // Capture phase: the simulator renders a camera view in two submissions,
  // one draw (deferred lighting) and then several (sky, clouds, lights).
  // The tail holds the first and copies the second. The observed lists stay
  // alive until the boundary observation is removed.
  Commands deferred, forward, rt_exit;
  {
    deferred.initialize(device.p);
    forward.initialize(device.p);
    rt_exit.initialize(device.p);
    constexpr std::uint64_t DeferredGeneration = Generation + 30, ForwardGeneration = Generation + 31, ExitGeneration = Generation + 32;
    require(manager->register_command_list(deferred.list.p, DeviceKey, DeferredGeneration) &&
                manager->register_command_list(forward.list.p, DeviceKey, ForwardGeneration) &&
                manager->register_command_list(rt_exit.list.p, DeviceKey, ExitGeneration),
            "Register capture-phase lists");
    require(Boundary::register_list(deferred.list.p, DeferredGeneration, callbacks).ready &&
                Boundary::register_list(forward.list.p, ForwardGeneration, callbacks).ready &&
                Boundary::register_list(rt_exit.list.p, ExitGeneration, callbacks).ready,
            "Observe capture-phase lists");
    Ref<ID3D12GraphicsCommandList7> deferred7, forward7;
    check(deferred.list->QueryInterface(IID_PPV_ARGS(deferred7.put())), "Get deferred interface7");
    check(forward.list->QueryInterface(IID_PPV_ARGS(forward7.put())), "Get forward interface7");
    for (unsigned feed = 0; feed < 2; ++feed) {
      ID3D12Resource* target = sources[feed].p;
      const float first_color[]{1, 0, 1, 1};
      manager->stage_source_draw(deferred.list.p, DeferredGeneration, 1, &target, &ids[feed]);
      draw.record(deferred7.p, handles[feed], colors[0][feed], false, width, heights[feed]);
      manager->stage_source_draw(forward.list.p, ForwardGeneration, 1, &target, &ids[feed]);
      draw.record(forward7.p, handles[feed], first_color, false, width, heights[feed]);
      manager->stage_source_draw(forward.list.p, ForwardGeneration, 1, &target, &ids[feed]);
      draw.record(forward7.p, handles[feed], colors[1][feed], false, width, heights[feed]);
    }
    // A scoped target report moves both sources out of their RT model.
    for (unsigned feed = 0; feed < 2; ++feed) {
      ID3D12Resource* target = sources[feed].p;
      manager->invalidate_source_targets(rt_exit.list.p, ExitGeneration, 1, &target, &ids[feed]);
    }
    check(deferred.list->Close(), "Close deferred-lighting recording");
    check(forward.list->Close(), "Close forward recording");
    check(rt_exit.list->Close(), "Close render-target exit recording");
    ID3D12CommandList* deferred_list = deferred.list.p;
    ID3D12CommandList* forward_list = forward.list.p;
    ID3D12CommandList* exit_list = rt_exit.list.p;
    ID3D12CommandList* both[]{deferred_list, forward_list};
    using taxi_camera::capture_phase::Kind;
    using taxi_camera::source_state::Effect;
    using taxi_camera::source_state::Model;
    const auto source_key = [&](unsigned feed) {
      return taxi_camera::source_state::Key{reinterpret_cast<std::uint64_t>(sources[feed].p), ids[feed]};
    };
    const auto rt_model = manager->device(DeviceKey)->source_states.state(source_key(0)).model;
    require(rt_model == Model::legacy_rt || rt_model == Model::enhanced_rt, "Capture-phase sources are not render targets");
    const auto scenario = [&](std::initializer_list<std::pair<UINT, ID3D12CommandList* const*>> batches, unsigned word, Kind kind,
                              auto between, const char* label) {
      Sleep(70);  // Past the 20 Hz interval.
      const auto before = manager->statistics();
      std::size_t index = 0;
      for (const auto& [count, lists] : batches) {
        producer.queue->ExecuteCommandLists(count, lists);
        if (!index && kind != Kind::after_multi)
          require(std::strcmp(manager->statistics().tail_status, "awaiting_complete_render") == 0, label);
        between(index++);
      }
      drain(producer.queue.p);
      const auto after = manager->statistics();
      for (unsigned feed = 0; feed < 2; ++feed) {
        const auto& was = before.phases[feed];
        const auto& now = after.phases[feed];
        require(now.captures[static_cast<std::size_t>(kind)] == was.captures[static_cast<std::size_t>(kind)] + 1, label);
        require(after.tail_captures == before.tail_captures + 2, label);
        require(now.batches == was.batches + batches.size(), label);
      }
      std::array<Manager::Frame, 2> frames{};
      std::size_t frame_count = 0;
      wait([&] {
        frame_count += manager->poll_completed_frames(frames.data() + frame_count, frames.size() - frame_count);
        return frame_count == 2;
      });
      check(consumer.allocator->Reset(), "Reset phase consumer allocator");
      check(consumer.list->Reset(consumer.allocator.p, nullptr), "Reset phase consumer list");
      for (const auto& result : frames) {
        const auto feed = result.match.feed;
        require(feed < 2, "Bad phase feed identity");
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
      check(consumer.list->Close(), "Close phase consumer");
      const auto consume = manager->begin_private_submission(DeviceKey, consumer.queue.p);
      require(consume.receipt != 0, "Begin phase consumer");
      ID3D12CommandList* read = consumer.list.p;
      consumer.queue->ExecuteCommandLists(1, &read);
      require(manager->end_private_submission(consume.receipt), "Submit phase consumer");
      for (const auto& result : frames)
        require(manager->finish_consumption(result.token, consume.fence, consume.value), "Finish phase consumer lease");
      drain(consumer.queue.p);
      for (unsigned feed = 0; feed < 2; ++feed) {
        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(readback_bytes[feed])};
        check(readbacks[feed]->Map(0, &range, &mapped), "Map phase pixels");
        std::uint32_t pixel = 0;
        std::memcpy(&pixel,
                    static_cast<const std::uint8_t*>(mapped) + footprints[feed].Offset +
                        UINT64(heights[feed] / 2) * footprints[feed].Footprint.RowPitch + width / 2 * 4,
                    4);
        require(pixel == words[word][feed], label);
        const D3D12_RANGE empty{0, 0};
        readbacks[feed]->Unmap(0, &empty);
      }
    };
    const auto none = [](std::size_t) {};
    const auto retire = [&](std::size_t batch) {
      if (batch)
        return;
      const std::lock_guard lock(manager->mutex_);
      manager->retire_sources(*manager->device(DeviceKey), 0);
    };
    // The batch after the hold is due but not recorded (a rate drop leaves it
    // inside the interval); the next deferred batch must not count as after_hold.
    const auto rate_drop = [&](std::size_t batch) { manager->set_source_rate(batch ? 20 : 1); };
    // A batch whose tail sees the sources out of their RT model, then RT again
    // without a tail, as a later batch's transition would leave them.
    const auto leave_rt = [&](std::size_t batch) {
      if (batch)
        return;
      producer.queue->ExecuteCommandLists(1, &exit_list);
      const std::lock_guard lock(manager->mutex_);
      auto& states = manager->device(DeviceKey)->source_states;
      require(states.state(source_key(0)).model == Model::other && states.state(source_key(1)).model == Model::other,
              "The scoped target report did not leave the RT model");
      taxi_camera::source_state::Recording entry;
      for (unsigned feed = 0; feed < 2; ++feed)
        require(entry.append({source_key(feed), rt_model == Model::legacy_rt ? Effect::Kind::legacy_rt : Effect::Kind::enhanced_rt}),
                "Record RT re-entry");
      require(states.apply(entry), "Apply RT re-entry");
    };
    // A model lost without a tail and restored by the stall-recovery rearm.
    const auto rearm = [&](std::size_t batch) {
      if (batch)
        return;
      {
        const std::lock_guard lock(manager->mutex_);
        manager->device(DeviceKey)->source_states.retire_live_models();
      }
      require(manager->rearm_source_states() >= 2, "Rearm did not restore both sources");
    };
    scenario({{1, &deferred_list}, {1, &forward_list}}, 1, Kind::after_hold, none, "Held: the tail did not copy the forward batch");
    scenario({{2, both}}, 1, Kind::after_multi, none, "A complete one-submission render was held");
    scenario({{1, &deferred_list}, {1, &deferred_list}}, 0, Kind::after_hold, none, "A single-writer view stalled");
    scenario({{1, &deferred_list}, {1, &deferred_list}}, 0, Kind::forced, retire, "A forgotten hold held again");
    scenario({{1, &deferred_list}, {1, &forward_list}, {1, &deferred_list}}, 0, Kind::forced, rate_drop,
             "An unrecorded capture kept its hold for the next render's deferred batch");
    scenario({{1, &deferred_list}, {1, &deferred_list}}, 0, Kind::forced, leave_rt, "A hold survived its source leaving the RT state");
    scenario({{1, &deferred_list}, {1, &deferred_list}}, 0, Kind::forced, rearm, "A hold survived a source-state rearm");
    for (unsigned feed = 0; feed < 2; ++feed)
      require(manager->statistics().phases[feed].held == 6 && manager->statistics().phases[feed].max_draws >= 3, "Held/max counts");
  }
  const auto phase_checked_captures = manager->statistics().tail_captures;
  // Unknown actual recordings must destroy global model proof, even if empty.
  const auto wipes_before_unknown = manager->statistics().wipes;
  check(unknown.list->Close(), "Close unknown list");
  ID3D12CommandList* unregistered = unknown.list.p;
  producer.queue->ExecuteCommandLists(1, &unregistered);
  Sleep(70);
  producer.queue->ExecuteCommandLists(1, &original);
  drain(producer.queue.p);
  require(manager->statistics().tail_captures == phase_checked_captures, "Unregistered submission left stale state proof usable");
  require(manager->statistics().wipes > wipes_before_unknown &&
              manager->statistics().wipe_counts[static_cast<std::size_t>(Manager::WipeSite::unknown_lists_no_owner)] > 0,
          "An unregistered list's wipe was not attributed to unknown_lists_no_owner");
  three_feed_tail(device.p, draw);
  require(Boundary::remove().protection_restored, "Remove boundary observation");
  producer.queue->ExecuteCommandLists(1, &original);
  drain(producer.queue.p);
  require(manager->statistics().tail_captures == phase_checked_captures &&
              std::strcmp(manager->statistics().tail_status, "observer_disabled") == 0 &&
              manager->statistics().last_wipe_site == Manager::WipeSite::observer_disabled,
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
      "\"tail_device_reuse\":true,\"scoped_target_invalidation\":true,\"idle_reset_recovery\":true,\"three_feed_tail\":true,"
      "\"baseline_tail_captures\":6,\"gpu_timing_samples\":6,\"replay_tail_captures\":%llu,\"checks\":%u}\n",
      warp_requested ? "true" : "false", enhanced ? "true" : "false", born_render_target ? "true" : "false",
      static_cast<unsigned long long>(checked_pixels), static_cast<unsigned long long>(rate_checked_captures),
      static_cast<unsigned long long>(rate_checked_captures - 6), checks);
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
