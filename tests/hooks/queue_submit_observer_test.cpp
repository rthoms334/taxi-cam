#include "../../src/hooks/queue_submit_observer.hpp"

#include <dxgi1_4.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
namespace qs = taxi_camera::engine_hook::queue_submit;
unsigned checks = 0;
unsigned fail_readonly = 0;
unsigned fail_writable = 0;
void** replace_slot = nullptr;
void* replacement = nullptr;

void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}

void success(HRESULT value, const char* message) {
  require(SUCCEEDED(value), message);
}

using Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
struct ExpectedCall {
  ID3D12CommandQueue* queue = nullptr;
  UINT count = 0;
  ID3D12CommandList* const* lists = nullptr;
  UINT augmented_count = 0;
  ID3D12CommandList* const* augmented_lists = nullptr;
};
thread_local ExpectedCall expected;
thread_local std::uint64_t expected_receipt = 0;

void* slot_value(void** slot) noexcept {
  // The C++ optimizer may otherwise fold a known class's constant vtable entry.
  void* value = nullptr;
  SIZE_T copied = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), slot, &value, sizeof(value), &copied) || copied != sizeof(value))
    std::terminate();
  return value;
}

void invoke(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
  const auto saved = expected;
  expected = {queue, count, lists};
  const auto table = *reinterpret_cast<void***>(queue);
  reinterpret_cast<Execute>(slot_value(table + 10))(queue, count, lists);
  expected = saved;
}

struct Context {
  std::atomic<unsigned> before{0}, after{0}, refused{0}, contended{0}, unordered{0}, bad{0}, phase{0};
  std::atomic<unsigned> calls_at_contention{0};
  std::atomic<std::uint64_t> sequence{0};
  std::atomic<bool> recurse_before{false}, recurse_after{false};
  bool receipt_enabled = true;
};

class MockQueue : public ID3D12CommandQueue {
 public:
  std::atomic<ULONG> references{1};
  std::atomic<unsigned> calls{0}, bad{0};
  Context* context = nullptr;
  std::atomic<bool>* hold_until = nullptr;
  std::atomic<bool>* entered_hold = nullptr;
  UINT hold_count = 0;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override { return --references; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { return E_NOINTERFACE; }
  void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource*,
                                            UINT,
                                            const D3D12_TILED_RESOURCE_COORDINATE*,
                                            const D3D12_TILE_REGION_SIZE*,
                                            ID3D12Heap*,
                                            UINT,
                                            const D3D12_TILE_RANGE_FLAGS*,
                                            const UINT*,
                                            const UINT*,
                                            D3D12_TILE_MAPPING_FLAGS) override {}
  void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource*,
                                          const D3D12_TILED_RESOURCE_COORDINATE*,
                                          ID3D12Resource*,
                                          const D3D12_TILED_RESOURCE_COORDINATE*,
                                          const D3D12_TILE_REGION_SIZE*,
                                          D3D12_TILE_MAPPING_FLAGS) override {}
  void STDMETHODCALLTYPE ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists) override {
    ++calls;
    if (expected.queue != this || (expected.augmented_count ? expected.augmented_count : expected.count) != count ||
        (!expected.augmented_count && expected.lists != lists))
      ++bad;
    if (expected.augmented_count) {
      if (lists == expected.lists)
        ++bad;
      for (UINT i = 0; i < count && i < expected.augmented_count; ++i)
        if (lists[i] != expected.augmented_lists[i])
          ++bad;
    }
    if (context) {
      unsigned phase = 1;
      context->phase.compare_exchange_strong(phase, 2);
    }
    if (hold_until && count == hold_count) {
      if (entered_hold)
        entered_hold->store(true, std::memory_order_release);
      while (!hold_until->load(std::memory_order_acquire))
        SwitchToThread();
    } else {
      SwitchToThread();
    }
  }
  void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
  void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
  void STDMETHODCALLTYPE EndEvent() override {}
  HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence*, UINT64) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence*, UINT64) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64*, UINT64*) override { return E_NOTIMPL; }
  D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE GetDesc(D3D12_COMMAND_QUEUE_DESC* output) override {
    *output = {};
    return output;
  }
  // A later interface extension must survive untouched. Production never copies
  // this or any other vtable into a shorter synthetic interface.
  virtual unsigned extension() { return 0x76543210u; }
};

class OtherQueue final : public MockQueue {
 public:
  unsigned extension() override { return 0x12345678u; }
};

std::uint64_t before(void* opaque, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept {
  auto& context = *static_cast<Context*>(opaque);
  ++context.before;
  if (context.phase.exchange(1) != 0 || expected.queue != queue || expected.count != count || expected.lists != lists)
    ++context.bad;
  expected_receipt = ++context.sequence;
  if (context.recurse_before.exchange(false))
    invoke(queue, count, lists);
  if (qs::disable_queue(queue).status != qs::Status::reentrant_operation)
    ++context.bad;
  return context.receipt_enabled ? expected_receipt : 0;
}

void after(void* opaque, ID3D12CommandQueue* queue, std::uint64_t receipt) noexcept {
  auto& context = *static_cast<Context*>(opaque);
  ++context.after;
  if (context.phase.exchange(0) != 2 || receipt != expected_receipt)
    ++context.bad;
  if (context.recurse_after.exchange(false)) {
    ID3D12CommandList* nested[] = {reinterpret_cast<ID3D12CommandList*>(0x12340)};
    invoke(queue, 1, nested);
  }
}

void refused(void* opaque, ID3D12CommandQueue*, qs::Refusal reason) noexcept {
  auto& context = *static_cast<Context*>(opaque);
  ++context.refused;
  if (reason == qs::Refusal::contended_submission)
    ++context.contended;
  else
    context.phase = 0;
}

qs::Callbacks callbacks(Context* context) {
  qs::Callbacks value{context, before, after, refused,
                      [](void* opaque, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept {
                        auto& current = *static_cast<Context*>(opaque);
                        if (expected.queue != queue || expected.count != count || expected.lists != lists)
                          ++current.bad;
                        current.calls_at_contention = static_cast<MockQueue*>(queue)->calls.load();
                        refused(opaque, queue, qs::Refusal::contended_submission);
                        return std::uint64_t{0};
                      }};
  value.forwarded_unordered = [](void* opaque, ID3D12CommandQueue* queue) noexcept {
    auto& current = *static_cast<Context*>(opaque);
    // Fires only after the exact original forward of a zero-receipt batch.
    if (expected.queue != queue || static_cast<MockQueue*>(queue)->calls.load() == 0)
      ++current.bad;
    ++current.unordered;
  };
  return value;
}

DWORD protection(void* address) {
  MEMORY_BASIC_INFORMATION region{};
  require(VirtualQuery(address, &region, sizeof(region)) == sizeof(region), "Cannot query the test slot page");
  return region.Protect;
}

void replace(void** slot, void* value) {
  DWORD prior = 0, discarded = 0;
  require(VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prior) != FALSE, "Cannot edit this process's own test table");
  InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), value);
  require(VirtualProtect(slot, sizeof(void*), prior, &discarded) != FALSE, "Cannot restore the test table protection");
}

void STDMETHODCALLTYPE foreign(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept {}

void mock() {
  auto* context = new Context;
  auto* queue = new MockQueue;
  queue->context = context;
  auto** table = *reinterpret_cast<void***>(queue);
  const auto original = slot_value(table + 10);
  const auto extension = slot_value(table + 19);
  const auto prior = protection(table + 10);
  require(qs::register_queue(nullptr, callbacks(context)).status == qs::Status::invalid_argument, "Null queue was accepted");
  require(qs::register_queue(queue, {}).status == qs::Status::invalid_argument, "Missing callbacks were accepted");
  require(qs::register_queue(reinterpret_cast<ID3D12CommandQueue*>(1), callbacks(context)).status == qs::Status::invalid_queue_memory,
          "Misaligned queue memory was accepted");
  const auto installed = qs::register_queue(queue, callbacks(context));
  require(
      installed.status == qs::Status::registered && installed.hook_installed && installed.queue_retained && installed.protection_restored,
      "Mock registration failed");
  require(queue->references == 2 && slot_value(table + 10) != original && slot_value(table + 19) == extension &&
              protection(table + 10) == prior,
          "Installation did not preserve table extension, reference count or protection");
  require(qs::register_queue(queue, callbacks(context)).status == qs::Status::already_registered && queue->references == 2,
          "Repeated registration changed ownership");
  require(qs::register_queue(queue, callbacks(new Context)).status == qs::Status::callback_mismatch, "Callback identity changed");
  require(qs::register_queue(new OtherQueue, callbacks(new Context)).status == qs::Status::different_vtable,
          "Another native vtable was admitted to this wrapper");
  ID3D12CommandList* lists[] = {reinterpret_cast<ID3D12CommandList*>(0x10000), reinterpret_cast<ID3D12CommandList*>(0x20000)};
  invoke(queue, 2, lists);
  require(queue->calls == 1 && queue->bad == 0 && context->before == 1 && context->after == 1 && context->bad == 0,
          "Typed Win64 arguments, call count or pre/post order changed");
  auto* unknown = new MockQueue;
  invoke(unknown, 0x87654321u, lists);
  require(unknown->calls == 1 && unknown->bad == 0 && context->before == 1, "Unknown queue was observed or altered");
  invoke(queue, qs::kMaximumCommandLists + 1, lists);
  invoke(queue, 0, lists);
  invoke(queue, 1, nullptr);
  require(queue->calls == 4 && context->before == 1 && context->refused == 3 && context->after == 1,
          "A refused batch was dropped, inspected beyond its bound or falsely completed");
  context->recurse_after = true;
  invoke(queue, 2, lists);
  require(queue->calls == 6 && context->before == 2 && context->after == 2 && context->refused == 3,
          "Private after-submit recursion was dropped, recursively observed or deadlocked");
  context->recurse_before = true;
  invoke(queue, 2, lists);
  require(queue->calls == 8 && context->before == 3 && context->after == 2 && context->refused == 4,
          "An ambiguous nested pre-submit batch published completion");

  std::atomic<bool> helper_finished{false};
  std::atomic<bool> entered_hold{false};
  queue->hold_until = &helper_finished;
  queue->entered_hold = &entered_hold;
  queue->hold_count = 2;
  std::thread helper([&] {
    while (!entered_hold.load(std::memory_order_acquire))
      SwitchToThread();
    ID3D12CommandList* local[] = {reinterpret_cast<ID3D12CommandList*>(0x30000)};
    invoke(queue, 1, local);
    helper_finished.store(true, std::memory_order_release);
  });
  invoke(queue, 2, lists);
  helper.join();
  queue->hold_until = nullptr;
  queue->entered_hold = nullptr;
  require(queue->calls == 10 && queue->bad == 0 && context->before == 4 && context->after == 3 && context->contended == 1 &&
              context->refused == 5 && context->bad == 0 && context->phase == 0 && context->calls_at_contention == 9,
          "A contended helper submit blocked on the owner thread or dropped a forwarded batch");
  // A zero receipt (unrelated, or ordering skipped within its wait budget)
  // forwards without after and then reports forwarded_unordered exactly once.
  require(context->unordered == 0, "forwarded_unordered fired for an observed or refused batch");
  context->receipt_enabled = false;
  context->phase = 0;
  invoke(queue, 2, lists);
  context->receipt_enabled = true;
  require(queue->calls == 11 && context->before == 5 && context->after == 3 && context->unordered == 1 && context->bad == 0,
          "A zero-receipt batch was completed, dropped or not reported as unordered");
  context->phase = 0;

  std::array<std::thread, 4> workers;
  for (auto& worker : workers)
    worker = std::thread([&] {
      ID3D12CommandList* local[] = {reinterpret_cast<ID3D12CommandList*>(0x30000)};
      for (unsigned i = 0; i < 200; ++i)
        invoke(queue, 1, local);
    });
  for (auto& worker : workers)
    worker.join();
  require(queue->calls == 811 && queue->bad == 0 && context->bad == 0 && context->phase == 0,
          "Concurrent queue submissions dropped or corrupted a forwarded batch");
  require(context->after >= 3 && context->before == context->after + 2 && context->refused == 4 + context->contended &&
              context->after + context->contended == 804,
          "Concurrent observation lost receipt pairing or double-counted a contended submit");
  const auto stats = qs::statistics(queue);
  require(stats.submissions + context->contended == 809 && stats.receipts == context->after && stats.refusals == context->refused,
          "Submission statistics are inconsistent");
  // Every wrapper entry for this registered queue, minus nested forwards, is a
  // presentation pulse; contended counts the helper submits that found the lock busy.
  require(stats.calls == 809 && stats.contended == context->contended && qs::total_statistics().calls >= stats.calls &&
              qs::total_statistics().contended >= stats.contended,
          "Presentation pulse or contention statistics are inconsistent");
  const auto observed_after = context->after.load();
  require(qs::disable_queue(queue).status == qs::Status::disabled, "Disabling the queue failed");
  invoke(queue, 2, lists);
  require(queue->calls == 812 && context->after == observed_after && queue->references == 2,
          "Disable observed work or released the retained queue");
  require(qs::statistics(queue).calls == 810, "A disabled queue's submit was not counted as a presentation pulse");
  require(qs::register_queue(queue, callbacks(context)).status == qs::Status::already_registered,
          "Identical registration could not resume");
  for (unsigned i = 1; i < qs::kMaximumQueues; ++i)
    require(qs::register_queue(new MockQueue, callbacks(new Context)).status == qs::Status::registered, "Bounded queue slot failed");
  auto* overflow = new MockQueue;
  require(qs::register_queue(overflow, callbacks(new Context)).status == qs::Status::queue_limit && overflow->references == 1,
          "Queue limit consumed an unowned reference");
  const auto wrapper = slot_value(table + 10);
  replace(table + 10, reinterpret_cast<void*>(&foreign));
  require(qs::remove().status == qs::Status::slot_changed && slot_value(table + 10) == reinterpret_cast<void*>(&foreign),
          "Removal overwrote a foreign replacement");
  replace(table + 10, wrapper);
  const auto removed = qs::remove();
  require(removed.status == qs::Status::removed && !removed.hook_installed && removed.protection_restored &&
              slot_value(table + 10) == original && protection(table + 10) == prior,
          "Owned-slot removal did not restore the original and page protection");
  require(qs::register_queue(queue, callbacks(context)).status == qs::Status::installation_consumed, "Removed installation was reused");
  invoke(queue, 2, lists);
  require(queue->calls == 813 && queue->bad == 0 && context->after == observed_after,
          "Removed hook still observed or omitted an original call");
}

// Another tool hooking the same slot after Taxi Cam (GlassOut's capture DLL):
// its hook forwards every call to whatever the slot held before it.
Execute chained_next = nullptr;
std::atomic<unsigned> chained_calls{0};
void STDMETHODCALLTYPE chaining(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept {
  ++chained_calls;
  chained_next(queue, count, lists);
}
Context* discovery_context = nullptr;
std::atomic<unsigned> discoveries{0};
qs::Status discovery_status = qs::Status::not_installed;
ID3D12CommandQueue* discover_instead = nullptr;
void discover(ID3D12CommandQueue* queue) noexcept {
  ++discoveries;
  discovery_status = qs::register_queue(discover_instead ? discover_instead : queue, callbacks(discovery_context)).status;
}

void chained() {
  auto* first_context = new Context;
  auto* first = new MockQueue;
  first->context = first_context;
  auto** table = *reinterpret_cast<void***>(first);
  require(qs::register_queue(first, callbacks(first_context)).status == qs::Status::registered, "First queue registration failed");
  chained_next = reinterpret_cast<Execute>(slot_value(table + 10));
  replace(table + 10, reinterpret_cast<void*>(&chaining));
  ID3D12CommandList* lists[] = {reinterpret_cast<ID3D12CommandList*>(0x10000)};
  invoke(first, 1, lists);
  require(chained_calls == 1 && first->calls == 1 && first_context->before == 1 && first_context->after == 1 && first_context->bad == 0,
          "A queue registered before the other hook lost its observation");
  // Outside a discovery nothing shows that the slot's holder forwards here.
  auto* unproven = new MockQueue;
  require(qs::register_queue(unproven, callbacks(new Context)).status == qs::Status::slot_changed && unproven->references == 1,
          "A queue was registered under another hook without a call reaching the wrapper");
  discovery_context = new Context;
  require(qs::set_unknown_queue_observer(discover), "Discovery callback was refused");
  // A discovery admits only the queue whose call reached the wrapper.
  auto* bystander = new MockQueue;
  discover_instead = unproven;
  invoke(bystander, 1, lists);
  require(discoveries == 1 && discovery_status == qs::Status::slot_changed && unproven->references == 1 && bystander->calls == 1 &&
              bystander->bad == 0 && discovery_context->before == 0,
          "A discovery registered a queue other than its own under another hook");
  discover_instead = nullptr;
  // A queue first seen after the other tool hooked (frame generation makes a
  // new one when a flight loads) is registered from its own first submit.
  auto* late = new MockQueue;
  late->context = discovery_context;
  invoke(late, 1, lists);
  require(discoveries == 2 && discovery_status == qs::Status::registered && late->references == 2 && late->calls == 1 && late->bad == 0 &&
              discovery_context->before == 1 && discovery_context->after == 1 && discovery_context->bad == 0,
          "A queue first seen under another hook was not registered and observed from its first submit");
  discovery_context->phase = 0;
  invoke(late, 1, lists);
  require(discoveries == 2 && late->calls == 2 && discovery_context->before == 2 && discovery_context->after == 2 &&
              discovery_context->bad == 0 && slot_value(table + 10) == reinterpret_cast<void*>(&chaining),
          "The late queue was discovered twice, lost observation or the other hook's slot was rewritten");
  require(qs::remove().status == qs::Status::slot_changed && slot_value(table + 10) == reinterpret_cast<void*>(&chaining),
          "Removal overwrote the other tool's hook");
}

struct AugmentationContext : Context {
  std::array<qs::Insertion, qs::kMaximumInsertions> insertions{};
  std::array<ID3D12CommandList*, qs::kMaximumCommandLists + qs::kMaximumInsertions> expected_lists{};
  UINT insertion_count = 0, expected_count = 0;
  std::atomic<unsigned> augment_calls{0}, result_calls{0}, last_inserted{0};
  bool recurse_augment = false, recurse_result = false;
};

UINT augment(void* opaque,
             ID3D12CommandQueue* queue,
             std::uint64_t receipt,
             UINT count,
             ID3D12CommandList* const* lists,
             qs::Insertion* output,
             UINT capacity) noexcept {
  auto& context = *static_cast<AugmentationContext*>(opaque);
  ++context.augment_calls;
  if (context.phase != 1 || receipt != expected_receipt || expected.queue != queue || expected.count != count || expected.lists != lists ||
      capacity != qs::kMaximumInsertions)
    ++context.bad;
  for (UINT i = 0; i < capacity; ++i)
    output[i] = context.insertions[i];
  if (context.recurse_augment) {
    context.recurse_augment = false;
    invoke(queue, count, lists);
  }
  return context.insertion_count;
}

void augmentation_result(void* opaque, ID3D12CommandQueue* queue, std::uint64_t receipt, UINT inserted) noexcept {
  auto& context = *static_cast<AugmentationContext*>(opaque);
  ++context.result_calls;
  context.last_inserted = inserted;
  if (receipt != expected_receipt || queue != expected.queue)
    ++context.bad;
  if (context.recurse_result) {
    context.recurse_result = false;
    invoke(queue, expected.count, expected.lists);
  } else if (inserted) {
    expected.augmented_count = context.expected_count;
    expected.augmented_lists = context.expected_lists.data();
  }
}

void augmentation() {
  auto* context = new AugmentationContext;
  auto* queue = new MockQueue;
  queue->context = context;
  auto observe = callbacks(context);
  observe.augment = augment;
  observe.augmentation_result = augmentation_result;
  require(qs::register_queue(queue, observe).status == qs::Status::registered, "Augmentation registration failed");
  auto mismatch = observe;
  mismatch.augmentation_result = nullptr;
  require(qs::register_queue(queue, mismatch).status == qs::Status::callback_mismatch,
          "Augmentation result identity changed after registration");
  mismatch = observe;
  mismatch.augment = nullptr;
  require(qs::register_queue(queue, mismatch).status == qs::Status::callback_mismatch,
          "Augmentation preparation identity changed after registration");
  auto* a = reinterpret_cast<ID3D12CommandList*>(0x10000);
  auto* b = reinterpret_cast<ID3D12CommandList*>(0x20000);
  auto* x = reinterpret_cast<ID3D12CommandList*>(0x30000);
  auto* y = reinterpret_cast<ID3D12CommandList*>(0x40000);
  auto* z = reinterpret_cast<ID3D12CommandList*>(0x50000);
  ID3D12CommandList* original[]{a, b};
  const auto run = [&](UINT accepted) {
    const auto calls = queue->calls.load(), after_count = context->after.load(), result_count = context->result_calls.load();
    invoke(queue, 2, original);
    require(queue->calls == calls + 1 && context->after == after_count + 1 && context->result_calls == result_count + 1 &&
                context->last_inserted == accepted && queue->bad == 0 && context->bad == 0 && original[0] == a && original[1] == b,
            "Augmentation changed application order, arguments, native call count or receipt pairing");
  };
  context->insertions = {{{0, x}, {1, y}}};
  context->expected_lists = {a, x, b};
  context->expected_count = 3;
  context->insertion_count = 1;
  run(1);
  context->expected_lists = {a, x, b, y};
  context->expected_count = 4;
  context->insertion_count = 2;
  run(2);
  context->insertions = {{{1, x}, {1, y}}};
  context->expected_lists = {a, b, x, y};
  run(2);
  context->insertion_count = 1;
  context->insertions = {{{0, x, true}, {}}};
  context->expected_lists = {x, a, b};
  context->expected_count = 3;
  run(1);
  context->insertion_count = 2;
  context->expected_count = 4;
  context->insertions = {{{0, x, true}, {0, y, false}}};
  context->expected_lists = {x, a, y, b};
  run(2);
  context->insertions = {{{0, x, true}, {0, y, true}}};
  context->expected_lists = {x, y, a, b};
  run(2);
  context->insertions = {{{0, x, false}, {1, y, true}}};
  context->expected_lists = {a, x, y, b};
  run(2);
  context->insertions = {{{0, x, false}, {0, y, true}}};
  run(0);
  context->insertions = {{{0, x, true}, {UINT_MAX, y, true}}};
  run(0);
  context->insertion_count = 0;
  run(0);
  context->insertion_count = qs::kMaximumInsertions + 1;
  run(0);
  // Per display side a shadow restore and a camera copy, plus a display snapshot.
  static_assert(qs::kMaximumInsertions == 7);
  context->insertion_count = 3;
  context->insertions = {{{0, x, true}, {0, y}, {1, z}}};
  context->expected_lists = {x, a, y, b, z};
  context->expected_count = 5;
  run(3);
  context->insertions = {{{0, x}, {0, y, true}, {1, z}}};
  run(0);  // Out of batch order.
  context->insertions = {{{0, x}, {1, y}, {1, x}}};
  run(0);  // Repeated inserted list.
  context->insertion_count = 2;
  context->expected_count = 4;
  context->insertions = {{{1, x}, {0, y}}};
  run(0);
  context->insertions = {{{0, x}, {2, y}}};
  run(0);
  context->insertions = {{{0, x}, {1, nullptr}}};
  run(0);
  context->insertions = {{{0, x}, {1, x}}};
  run(0);
  context->insertions = {{{0, x}, {1, b}}};
  run(0);
  context->insertions = {{{0, x}, {1, y}}};
  original[1] = nullptr;
  const auto calls_before_null = queue->calls.load();
  invoke(queue, 2, original);
  require(queue->calls == calls_before_null + 1 && context->last_inserted == 0 && queue->bad == 0,
          "A null original list was dereferenced or augmented");
  original[1] = b;

  std::array<ID3D12CommandList*, qs::kMaximumCommandLists> maximum{};
  for (UINT i = 0; i < maximum.size(); ++i)
    maximum[i] = reinterpret_cast<ID3D12CommandList*>(std::uintptr_t{0x100000} + i * 16);
  context->insertions = {{{0, x}, {qs::kMaximumCommandLists - 1, y}}};
  context->expected_lists[0] = maximum[0];
  context->expected_lists[1] = x;
  for (UINT i = 1; i < maximum.size(); ++i)
    context->expected_lists[i + 1] = maximum[i];
  context->expected_lists[qs::kMaximumCommandLists + 1] = y;
  context->expected_count = qs::kMaximumCommandLists + 2;
  invoke(queue, static_cast<UINT>(maximum.size()), maximum.data());
  require(queue->bad == 0 && context->bad == 0 && context->last_inserted == 2,
          "Maximum-size batch could not append both bounded insertions");
  context->insertions = {{{0, x, true}, {qs::kMaximumCommandLists - 1, y, false}}};
  context->expected_lists[0] = x;
  for (UINT i = 0; i < maximum.size(); ++i)
    context->expected_lists[i + 1] = maximum[i];
  context->expected_lists[qs::kMaximumCommandLists + 1] = y;
  invoke(queue, static_cast<UINT>(maximum.size()), maximum.data());
  require(queue->bad == 0 && context->bad == 0 && context->last_inserted == 2,
          "Maximum-size batch could not prepend and append while retaining all original entries");

  const auto skip_count = context->augment_calls.load();
  invoke(queue, qs::kMaximumCommandLists + 1, original);
  invoke(queue, 0, original);
  invoke(queue, 1, nullptr);
  context->receipt_enabled = false;
  invoke(queue, 2, original);
  context->phase = 0;
  context->receipt_enabled = true;
  require(context->augment_calls == skip_count && queue->bad == 0, "Refused or zero-receipt batch attempted augmentation");

  context->insertions = {{{0, x}, {1, y}}};
  context->expected_lists = {a, x, b, y};
  context->expected_count = 4;
  const auto after_before_recursion = context->after.load();
  const auto refusal_before_recursion = context->refused.load();
  context->recurse_before = true;
  invoke(queue, 2, original);
  require(
      context->augment_calls == skip_count && context->after == after_before_recursion && context->refused == refusal_before_recursion + 1,
      "Reentrant before callback was allowed to augment");
  context->recurse_augment = true;
  invoke(queue, 2, original);
  require(context->last_inserted == 0 && context->after == after_before_recursion && context->refused == refusal_before_recursion + 2 &&
              queue->bad == 0,
          "Reentrant augmentation was submitted or completed");
  context->recurse_result = true;
  invoke(queue, 2, original);
  require(context->after == after_before_recursion && context->refused == refusal_before_recursion + 3 && queue->bad == 0,
          "Reentrant acceptance callback did not fall back to exact native arguments");

  std::atomic<bool> helper_finished{false}, entered_hold{false};
  queue->hold_until = &helper_finished;
  queue->entered_hold = &entered_hold;
  queue->hold_count = 4;
  const auto augment_before_contention = context->augment_calls.load();
  std::thread helper([&] {
    while (!entered_hold.load(std::memory_order_acquire))
      SwitchToThread();
    ID3D12CommandList* local[]{a};
    invoke(queue, 1, local);
    helper_finished.store(true, std::memory_order_release);
  });
  invoke(queue, 2, original);
  helper.join();
  queue->hold_until = nullptr;
  queue->entered_hold = nullptr;
  require(context->augment_calls == augment_before_contention + 1 && context->contended == 1 && queue->bad == 0 && context->bad == 0 &&
              context->phase == 0,
          "Contended submission was augmented, blocked or lost while one augmented call forwarded");
  require(qs::remove().status == qs::Status::removed, "Augmentation hook removal failed");
}

void protection_failure(const std::string& mode) {
  auto* queue = new MockQueue;
  auto* context = new Context;
  queue->context = context;
  auto** slot = *reinterpret_cast<void***>(queue) + 10;
  const auto original = slot_value(slot);
  const auto prior = protection(slot);
  if (mode == "fail-writable") {
    fail_writable = 1;
    const auto failed = qs::register_queue(queue, callbacks(context));
    require(failed.status == qs::Status::protection_change_failed && !failed.hook_installed && failed.queue_retained &&
                failed.protection_restored && slot_value(slot) == original && queue->references == 2,
            "Initial protection failure did not leave the queue untouched");
  } else if (mode == "fail-install-restore") {
    fail_readonly = 2;
    const auto failed = qs::register_queue(queue, callbacks(context));
    require(failed.status == qs::Status::protection_restore_failed && failed.hook_installed && failed.queue_retained &&
                !failed.protection_restored && queue->references == 2,
            "Installed-but-unrestored hook was not explicitly reported");
    ID3D12CommandList* lists[] = {reinterpret_cast<ID3D12CommandList*>(0x10000)};
    invoke(queue, 1, lists);
    require(queue->calls == 1 && context->before == 0, "Unregistered partial installation did not forward unchanged");
    require(qs::restore_protection().status == qs::Status::protection_restore_failed,
            "A repeated protection restoration failure was forgotten");
    require(qs::restore_protection().status == qs::Status::protection_restored && protection(slot) == prior,
            "Saved original protection was not recovered");
  } else if (mode == "fail-cas-restore") {
    replace_slot = slot;
    replacement = reinterpret_cast<void*>(&foreign);
    fail_readonly = 1;
    const auto failed = qs::register_queue(queue, callbacks(context));
    require(failed.status == qs::Status::protection_restore_failed && !failed.hook_installed && failed.queue_retained &&
                !failed.protection_restored && slot_value(slot) == replacement,
            "CAS mismatch and failed restoration lost their separate state");
    require(
        qs::restore_protection().status == qs::Status::protection_restored && protection(slot) == prior && slot_value(slot) == replacement,
        "CAS mismatch recovery modified a foreign slot");
    replace(slot, original);
  }
  const auto registered = qs::register_queue(queue, callbacks(context));
  require((registered.status == qs::Status::registered || registered.status == qs::Status::already_registered) &&
              registered.queue_retained && registered.protection_restored && queue->references == 2,
          "Registration did not recover after the controlled failure");
  if (mode == "fail-remove-restore") {
    fail_readonly = 2;
    const auto failed = qs::remove();
    require(failed.status == qs::Status::protection_restore_failed && !failed.hook_installed && !failed.protection_restored &&
                slot_value(slot) == original,
            "Removal's failed protection restoration was not retained");
    require(qs::remove().status == qs::Status::protection_restore_failed, "Removal discarded its pending restoration");
    require(qs::remove().status == qs::Status::not_installed && protection(slot) == prior, "Removal retry did not repair the page");
  } else {
    require(qs::remove().status == qs::Status::removed && protection(slot) == prior, "Recovered registration could not be removed");
  }
}

struct GpuContext {
  ID3D12CommandList* expected = nullptr;
  ID3D12CommandList* private_list = nullptr;
  ID3D12Fence* fence = nullptr;
  unsigned before_count = 0, after_count = 0, refused_count = 0;
  HRESULT signal_result = E_FAIL;
};

std::uint64_t gpu_before(void* opaque, ID3D12CommandQueue*, UINT count, ID3D12CommandList* const* lists) noexcept {
  auto& context = *static_cast<GpuContext*>(opaque);
  ++context.before_count;
  return count == 1 && lists[0] == context.expected ? 1 : 0;
}

void gpu_after(void* opaque, ID3D12CommandQueue* queue, std::uint64_t receipt) noexcept {
  auto& context = *static_cast<GpuContext*>(opaque);
  ++context.after_count;
  // Required integration behavior: own compositor work bypasses observation,
  // then a native queue fence certifies both original and private submissions.
  queue->ExecuteCommandLists(1, &context.private_list);
  context.signal_result = queue->Signal(context.fence, receipt);
}

void gpu_refused(void* opaque, ID3D12CommandQueue*, qs::Refusal) noexcept {
  ++static_cast<GpuContext*>(opaque)->refused_count;
}

void gpu(bool warp) {
  IDXGIFactory4* factory = nullptr;
  IDXGIAdapter* adapter = nullptr;
  ID3D12Device* device = nullptr;
  success(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory creation failed");
  if (warp)
    success(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP enumeration failed");
  success(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12 device creation failed");
  ID3D12CommandQueue* queue = nullptr;
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  success(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "Native queue creation failed");
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12CommandAllocator* private_allocator = nullptr;
  ID3D12GraphicsCommandList* first = nullptr;
  ID3D12GraphicsCommandList* second = nullptr;
  success(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Allocator creation failed");
  success(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&private_allocator)),
          "Private allocator creation failed");
  success(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&first)),
          "First list creation failed");
  success(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, private_allocator, nullptr, IID_PPV_ARGS(&second)),
          "Second list creation failed");
  ID3D12Resource* upload = nullptr;
  ID3D12Resource* readback = nullptr;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 256;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  success(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&upload)),
          "Upload creation failed");
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  success(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&readback)),
          "Readback creation failed");
  std::uint8_t* bytes = nullptr;
  D3D12_RANGE none{0, 0};
  success(upload->Map(0, &none, reinterpret_cast<void**>(&bytes)), "Upload Map failed");
  for (unsigned i = 0; i < 256; ++i)
    bytes[i] = static_cast<std::uint8_t>(i ^ 0xa5);
  upload->Unmap(0, nullptr);
  first->CopyBufferRegion(readback, 0, upload, 0, 128);
  second->CopyBufferRegion(readback, 128, upload, 128, 128);
  success(first->Close(), "First Close failed");
  success(second->Close(), "Second Close failed");
  auto* context = new GpuContext;
  context->expected = first;
  context->private_list = second;
  success(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context->fence)), "Completion fence creation failed");
  ID3D12Fence* blocker = nullptr;
  success(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocker)), "Blocker creation failed");
  const auto registered = qs::register_queue(queue, {context, gpu_before, gpu_after, gpu_refused});
  require(registered.status == qs::Status::registered && registered.hook_installed && registered.queue_retained &&
              registered.protection_restored,
          "Real native queue registration failed");
  success(queue->Wait(blocker, 1), "GPU blocker enqueue failed");
  queue->ExecuteCommandLists(1, &context->expected);
  require(context->before_count == 1 && context->after_count == 1 && context->refused_count == 0,
          "Native callbacks or recursion are incorrect");
  success(context->signal_result, "Post-submit queue Signal failed");
  require(context->fence->GetCompletedValue() == 0, "Completion was fabricated while the GPU submission was still blocked");
  success(blocker->Signal(1), "Test-only CPU unblock failed");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(event != nullptr, "Completion event creation failed");
  success(context->fence->SetEventOnCompletion(1, event), "Completion event setup failed");
  require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0 && context->fence->GetCompletedValue() == 1, "GPU completion timed out");
  D3D12_RANGE written{0, 256};
  success(readback->Map(0, &written, reinterpret_cast<void**>(&bytes)), "Readback Map failed");
  for (unsigned i = 0; i < 256; ++i)
    require(bytes[i] == static_cast<std::uint8_t>(i ^ 0xa5),
            "Fence completed before original/private GPU copies produced the expected bytes");
  readback->Unmap(0, &none);
  require(qs::statistics(queue).submissions == 1 && qs::statistics(queue).receipts == 1,
          "Nested native private submission was observed twice");
  require(qs::remove().status == qs::Status::removed, "Real native queue hook removal failed");
  success(device->GetDeviceRemovedReason(), "D3D12 device was removed");
  CloseHandle(event);
  blocker->Release();
  first->Release();
  second->Release();
  allocator->Release();
  private_allocator->Release();
  upload->Release();
  readback->Release();
  queue->Release();  // Registration's separate reference stays retained.
  device->Release();
  if (adapter)
    adapter->Release();
  factory->Release();
}

}  // namespace

extern "C" BOOL taxi_queue_test_virtual_protect(void* address, SIZE_T size, DWORD access, DWORD* prior) noexcept {
  if ((access == PAGE_READONLY && fail_readonly != 0) || (access == PAGE_READWRITE && fail_writable != 0)) {
    if (access == PAGE_READONLY)
      --fail_readonly;
    else
      --fail_writable;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  const auto result = VirtualProtect(address, size, access, prior);
  if (result && access == PAGE_READWRITE && replace_slot) {
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(replace_slot), replacement);
    replace_slot = nullptr;
  }
  return result;
}

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "mock";
  try {
    if (mode == "mock")
      mock();
    else if (mode == "augment")
      augmentation();
    else if (mode == "chained")
      chained();
    else if (mode == "hardware" || mode == "warp")
      gpu(mode == "warp");
    else if (mode == "fail-writable" || mode == "fail-install-restore" || mode == "fail-cas-restore" || mode == "fail-remove-restore")
      protection_failure(mode);
    else
      throw std::runtime_error("Unknown standalone validation mode");
    std::printf("PASS: %u queue-submit observer checks (%s); this process only.\n", checks, mode.c_str());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks (%s): %s\n", checks, mode.c_str(), error.what());
    return 1;
  }
}
