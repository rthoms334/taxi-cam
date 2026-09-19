// Exercise actual production metadata handlers without a simulator or GPU.
#define TAXI_METADATA_BATCH_VALIDATION
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>
#include "../../src/bridge/d3d12_bridge.cpp"

namespace {
unsigned checks{};
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}

// Only the three IUnknown ABI slots are exercised by device identity checks.
// Own stand-ins let the CPU fixture count COM traffic without a native device.
struct Identity final : IUnknown {
  Identity* canonical = this;
  unsigned queries = 0;
  ULONG references = 1;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    ++queries;
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown))
      return E_NOINTERFACE;
    *out = static_cast<IUnknown*>(canonical);
    canonical->AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override { return --references; }
  ID3D12Device* device() { return reinterpret_cast<ID3D12Device*>(static_cast<IUnknown*>(this)); }
};
unsigned simple_forwards{}, range_forwards{};
void STDMETHODCALLTYPE
forward_simple(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE) {
  ++simple_forwards;
}
void STDMETHODCALLTYPE forward_ranges(ID3D12Device*,
                                      UINT,
                                      const D3D12_CPU_DESCRIPTOR_HANDLE*,
                                      const UINT*,
                                      UINT,
                                      const D3D12_CPU_DESCRIPTOR_HANDLE*,
                                      const UINT*,
                                      D3D12_DESCRIPTOR_HEAP_TYPE) {
  ++range_forwards;
}
void descriptor_identity_checks() {
  namespace win = taxi_camera::standalone;
  auto& r = win::registry();
  Identity expected, alias, foreign;
  alias.canonical = &expected;
  r.device = expected.device();
  r.ready = true;
  require(win::same_device(expected.device()) && expected.queries == 0, "Exact retained interface incurred COM identity traffic");
  require(win::same_device(alias.device()) && alias.queries == 1 && expected.queries == 1,
          "Alternate interface did not retain canonical IUnknown identity proof");
  require(!win::same_device(foreign.device()) && !win::same_device(nullptr), "Foreign/null device admitted");
  require(expected.references == 1 && alias.references == 1 && foreign.references == 1, "Identity checks leaked COM references");
  win::descriptor_copy_simple.original = reinterpret_cast<void*>(&forward_simple);
  win::descriptor_copy.original = reinterpret_cast<void*>(&forward_ranges);
  const auto expected_queries = expected.queries, foreign_queries = foreign.queries;
  for (auto type : {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER}) {
    win::descriptors_simple(foreign.device(), 1, {0x100}, {0x200}, type);
    win::descriptors(foreign.device(), 0, nullptr, nullptr, 0, nullptr, nullptr, type);
  }
  require(simple_forwards == 2 && range_forwards == 2, "Irrelevant descriptor calls were not forwarded exactly once");
  require(expected.queries == expected_queries && foreign.queries == foreign_queries,
          "Irrelevant descriptor heaps incurred COM identity queries");
  r.dsv_stride = 32;
  r.dsvs[0x200] = DXGI_FORMAT_D32_FLOAT;
  win::descriptors_simple(expected.device(), 1, {0x100}, {0x200}, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  require(r.dsvs[0x100] == DXGI_FORMAT_D32_FLOAT, "Fast identity path lost selected-device DSV metadata");
  r.dsvs.erase(0x100);
  win::descriptors_simple(foreign.device(), 1, {0x100}, {0x200}, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  require(!r.dsvs.contains(0x100), "Foreign descriptor copy contaminated metadata");
  const D3D12_CPU_DESCRIPTOR_HANDLE destination{0x100}, source{0x200};
  win::descriptors(alias.device(), 1, &destination, nullptr, 1, &source, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  require(r.dsvs[0x100] == DXGI_FORMAT_D32_FLOAT, "Alternate canonical interface lost range-copy metadata");
  require(simple_forwards == 4 && range_forwards == 3, "Relevant descriptor calls were not forwarded exactly once");
  require(expected.references == 1 && alias.references == 1 && foreign.references == 1, "Descriptor observation leaked COM references");
  r.dsvs.clear();
  r.ready = false;
  r.device = nullptr;
  win::descriptor_copy_simple.original = nullptr;
  win::descriptor_copy.original = nullptr;
}
void submission_close_endpoint_checks() {
  namespace win = taxi_camera::standalone;
  auto& r = win::registry();
  const auto old_ready = r.ready.load();
  const auto old_verified = r.close_forward_verified;
  const auto old_key = r.key;
  const auto* old_profile = r.profile;
  auto item = std::make_shared<win::List>();
  item->native = reinterpret_cast<ID3D12GraphicsCommandList*>(0x770000);
  item->id = 770;
  auto target = std::make_shared<win::Resource>();
  target->native = reinterpret_cast<ID3D12Resource*>(0x880000);
  target->id = 880;
  target->desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  target->desc.Width = 768;
  target->desc.Height = 1024;
  target->desc.DepthOrArraySize = 1;
  target->desc.MipLevels = 5;
  target->desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  target->desc.SampleDesc.Count = 1;
  target->desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  item->submission_proof.reset(1, true);
  item->submission_proof.observe_legacy({reinterpret_cast<std::uint64_t>(target->native), target->id}, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        D3D12_RESOURCE_STATE_COMMON, 0);
  item->submission_proof.gpu_work();  // Same original list consumes the display after the leading exit.
  item->submission_proof.close(1, true);
  item->closed_recording.store(1, std::memory_order_release);
  r.lists[item->native] = item;
  r.resources[target->native] = target;
  r.profile = &taxi_camera::profiles::A380;
  r.ready = true;
  r.key = 0;  // No runtime device/patch exists. Tokens above must never reach COM.
  ID3D12CommandList* batch[]{item->native};
  const auto previous_plans = r.queue_patch_plans.load();
  {
    r.close_forward_verified = false;
    taxi_camera::SceneCaptureManager::DisplaySubmissionPlan plan;
    win::plan_display_submission(nullptr, nullptr, 1, batch, plan);
    require(!plan.count && !plan.current() && !target->submission_activity && r.queue_patch_plans == previous_plans,
            "Unverified downstream Close admitted a plan or submission activity despite incomplete final-work proof");
  }
  {
    r.close_forward_verified = true;
    taxi_camera::SceneCaptureManager::DisplaySubmissionPlan plan;
    win::plan_display_submission(nullptr, nullptr, 1, batch, plan);
    require(target->submission_activity == 1 && !target->draws,
            "Verified native Close did not credit leading-exit evidence before later work independently of native draws");
    require(!plan.count && !plan.current() && r.queue_patch_plans == previous_plans,
            "Metadata-only fixture manufactured an owned GPU plan without a ready patch");
  }
  r.lists.erase(item->native);
  r.resources.erase(target->native);
  r.key = old_key;
  r.profile = old_profile;
  r.close_forward_verified = old_verified;
  r.ready = old_ready;
  std::puts("PASS submission Close endpoint: unknown forwarding chain refuses plans/activity; native endpoint credits proven exits.");
}
void submission_profile_filter_checks() {
  namespace win = taxi_camera::standalone;
  using Proof = win::PfdSubmissionProof;
  auto& r = win::registry();
  const auto old_ready = r.ready.load();
  const auto old_backfill = r.live_backfill.exchange(false);
  const auto old_epoch = r.observation_epoch.load();
  r.observation_epoch = 2;  // Metadata remains observed without PFD runtime/COM traffic.
  r.ready = true;
  std::array<std::uint64_t, 16> old_filter{};
  for (std::size_t i = 0; i < old_filter.size(); ++i)
    old_filter[i] = r.display_filter[i].exchange(UINT64_MAX);  // Force every negative-filter lookup to collide.
  auto list = std::make_shared<win::List>();
  list->native = reinterpret_cast<ID3D12GraphicsCommandList*>(0x991000);
  list->id = 991;
  list->ready = true;
  r.lists[list->native] = list;
  std::vector<std::shared_ptr<win::Resource>> resources;
  const auto make_resource = [&](UINT16 mips, DXGI_FORMAT format) {
    auto item = std::make_shared<win::Resource>();
    item->id = 10000 + resources.size();
    item->native = reinterpret_cast<ID3D12Resource*>(0xa00000 + resources.size() * 0x100);
    item->desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D,     0, 768, 1024, 1, mips, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    r.resources[item->native] = item;
    resources.push_back(item);
    return item;
  };
  const auto target = make_resource(5, DXGI_FORMAT_R8G8B8A8_UNORM);
  const Proof::Key key{reinterpret_cast<std::uint64_t>(target->native), target->id};
  const auto observe = [&](const std::shared_ptr<win::Resource>& resource, UINT mip,
                           D3D12_RESOURCE_BARRIER_FLAGS flags = D3D12_RESOURCE_BARRIER_FLAG_NONE) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = flags;
    barrier.Transition = {resource->native, mip, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    win::observe_legacy(nullptr, list->native, list->id, barrier, win::boundary::ScopeEnabled);
  };
  list->submission_proof.reset(1, true);
  for (unsigned i = 0; i < Proof::maximum_resources + 1; ++i) {
    const auto unrelated = make_resource(i & 1 ? 2 : 5, i & 1 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT);
    observe(unrelated, 0);
  }
  observe(target, 0);
  list->submission_proof.close(1, true);
  require(list->submission_proof.complete(1) && list->submission_proof.prefix_candidate(key, 1) &&
              list->submission_proof.prefix_candidate(0u, 1).key == key && !list->submission_proof.prefix_candidate(1u, 1),
          "Bloom collisions with nonprofile mip/format consumed bounded candidate slots or invalidated exact display proof");
  list->submission_proof.reset(2, true);
  for (UINT mip = 1; mip < 5; ++mip)
    observe(target, mip);
  observe(target, 0);
  list->submission_proof.close(2, true);
  require(list->submission_proof.candidate(key, 2) && list->submission_proof.prefix_candidate(key, 2),
          "Valid independent FBW lower-mip transitions poisoned exact base-mip exit proof");
  list->submission_proof.reset(3, true);
  observe(target, 4);
  list->submission_proof.close(3, true);
  require(
      list->submission_proof.complete(3) && !list->submission_proof.prefix_candidate(key, 3) && !list->submission_proof.candidate(key, 3),
      "Lower-mip-only activity became base-mip evidence");
  list->submission_proof.reset(4, true);
  observe(target, 0);
  observe(target, 5);  // Outside the admitted resource's real mip range.
  list->submission_proof.close(4, true);
  require(!list->submission_proof.complete(4), "Out-of-range mip was silently treated as harmless lower-mip metadata");
  list->submission_proof.reset(5, true);
  observe(target, 0);
  observe(target, 4, D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
  list->submission_proof.close(5, true);
  require(!list->submission_proof.complete(5), "Lower-mip filtering erased split-barrier uncertainty");
  list->submission_proof.reset(6, true);
  observe(target, 0);
  D3D12_RESOURCE_BARRIER alias{};
  alias.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  win::observe_legacy(nullptr, list->native, list->id, alias, win::boundary::ScopeEnabled);
  list->submission_proof.close(6, true);
  require(!list->submission_proof.complete(6), "Exact profile filtering erased wildcard alias uncertainty");
  r.lists.erase(list->native);
  for (const auto& resource : resources)
    r.resources.erase(resource->native);
  for (std::size_t i = 0; i < old_filter.size(); ++i)
    r.display_filter[i] = old_filter[i];
  r.observation_epoch = old_epoch;
  r.live_backfill = old_backfill;
  r.ready = old_ready;
  win::known_lists = {};
  std::puts("PASS submission metadata filtering: exact profile admission, FBW lower mips, split/alias and bounds.");
}
unsigned work_forwards{};
template <unsigned Slot, class Signature, class Action>
struct WorkCall;
template <unsigned Slot, class C, class... Args, class Action>
struct WorkCall<Slot, void (STDMETHODCALLTYPE C::*)(Args...), Action> {
  using Hook = taxi_camera::standalone::StateHook<Slot, void (STDMETHODCALLTYPE C::*)(Args...), Action>;
  static void STDMETHODCALLTYPE forward(C*, Args...) { ++work_forwards; }
  static void run(ID3D12GraphicsCommandList* native) { Hook::invoke(forward, static_cast<C*>(native), Args{}...); }
};
void submission_gpu_classification_checks() {
  namespace win = taxi_camera::standalone;
  using Proof = win::PfdSubmissionProof;
  auto& r = win::registry();
  const auto old_ready = r.ready.load();
  const auto old_epoch = r.observation_epoch.load();
  const auto old_backfill = r.live_backfill.exchange(false);
  r.ready = true;
  r.observation_epoch = 2;  // Discovery must keep this proof even while camera capture is idle.
  auto list = std::make_shared<win::List>();
  list->native = reinterpret_cast<ID3D12GraphicsCommandList*>(0x992000);
  list->id = 992;
  list->ready = true;
  r.lists[list->native] = list;
  const Proof::Key key{0xa10000, 20000};
  const auto run = [&](UINT operation, bool disjoint, auto call, bool native_forward = true) {
    const auto generation = ++list->recording;
    list->submission_proof.reset(generation, true);
    list->queries.reset(true);
    const auto previous_forwards = work_forwards;
    call();
    require(work_forwards == previous_forwards + (native_forward ? 1 : 0) && !win::owned_depth,
            "GPU hook did not forward once or retained its reentry guard");
    list->submission_proof.observe_legacy(
        key, D3D12_RESOURCE_STATE_RENDER_TARGET,
        static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    list->submission_proof.close(generation, true);
    const Proof::Recording batch[]{{&list->submission_proof, generation}};
    require(Proof::batch_allows(batch, 1) && static_cast<bool>(list->submission_proof.prefix_candidate(key, generation)) == disjoint,
            "Actual idle GPU handler misclassified interference before a leading RT exit");
    require(!list->submission_proof.candidate(key, generation), "Actual GPU handler admitted a barrier-only tail copy");
    require(list->submission_proof.first_gpu_work() == operation &&
                list->submission_proof.prefix_blocker() == (disjoint ? UINT_MAX : operation),
            "Actual GPU handler lost the first-operation/blocker distinction");
  };
#define WORK_CASE(Slot, Interface, Method, Action, Disjoint) \
  run(Slot, Disjoint, [&] { WorkCall<Slot, decltype(&Interface::Method), win::Action>::run(list->native); })
  WORK_CASE(14, ID3D12GraphicsCommandList, Dispatch, ComputeGpuWork, true);
  WORK_CASE(15, ID3D12GraphicsCommandList, CopyBufferRegion, StateDisjointGpuWork, true);
  WORK_CASE(18, ID3D12GraphicsCommandList, CopyTiles, StateDisjointGpuWork, true);
  WORK_CASE(19, ID3D12GraphicsCommandList, ResolveSubresource, StateDisjointGpuWork, true);
  WORK_CASE(47, ID3D12GraphicsCommandList, ClearDepthStencilView, StateDisjointGpuWork, true);
  WORK_CASE(48, ID3D12GraphicsCommandList, ClearRenderTargetView, ClearRenderTarget, false);
  WORK_CASE(49, ID3D12GraphicsCommandList, ClearUnorderedAccessViewUint, ClearUnorderedAccess<49>, true);
  WORK_CASE(50, ID3D12GraphicsCommandList, ClearUnorderedAccessViewFloat, ClearUnorderedAccess<50>, true);
  WORK_CASE(54, ID3D12GraphicsCommandList, ResolveQueryData, StateDisjointGpuWork, true);
  WORK_CASE(60, ID3D12GraphicsCommandList1, AtomicCopyBufferUINT, GpuWork, false);
  WORK_CASE(61, ID3D12GraphicsCommandList1, AtomicCopyBufferUINT64, GpuWork, false);
  WORK_CASE(64, ID3D12GraphicsCommandList1, ResolveSubresourceRegion, StateDisjointGpuWork, true);
  WORK_CASE(66, ID3D12GraphicsCommandList2, WriteBufferImmediate, StateDisjointGpuWork, true);
  WORK_CASE(72, ID3D12GraphicsCommandList4, BuildRaytracingAccelerationStructure, StateDisjointGpuWork, true);
  WORK_CASE(73, ID3D12GraphicsCommandList4, EmitRaytracingAccelerationStructurePostbuildInfo, StateDisjointGpuWork, true);
  WORK_CASE(74, ID3D12GraphicsCommandList4, CopyRaytracingAccelerationStructure, StateDisjointGpuWork, true);
  WORK_CASE(76, ID3D12GraphicsCommandList4, DispatchRays, ComputeGpuWork, true);
  WORK_CASE(79, ID3D12GraphicsCommandList6, DispatchMesh, GpuWork, false);
#undef WORK_CASE
  using Begin = WorkCall<52, decltype(&ID3D12GraphicsCommandList::BeginQuery), win::QueryBegin>;
  using End = WorkCall<53, decltype(&ID3D12GraphicsCommandList::EndQuery), win::QueryEnd>;
  auto* heap = reinterpret_cast<ID3D12QueryHeap*>(0xb10000);  // Comparison only; never dereferenced.
  run(53, true, [&] {
    End::Hook::invoke(End::forward, list->native, heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    require(list->queries.known_empty(), "Timestamp EndQuery changed independent query admission");
  });
  for (const auto type : {D3D12_QUERY_TYPE_OCCLUSION, D3D12_QUERY_TYPE_PIPELINE_STATISTICS}) {
    run(52, true, [&] {
      Begin::Hook::invoke(Begin::forward, list->native, heap, type, 0);
      require(list->queries.active_count() == 1, "Paired query scope was lost during submission proof classification");
    });
    run(53, true, [&] {
      list->queries.begin(reinterpret_cast<std::uint64_t>(heap), type, 0);
      End::Hook::invoke(End::forward, list->native, heap, type, 0);
      require(list->queries.known_empty(), "Completed paired query retained an active scope");
    });
  }
  run(16, true, [&] { win::copy_texture(nullptr, list->native, list->id, nullptr, 0, 0, 0, nullptr, nullptr, false); }, false);
  run(17, true, [&] { win::copy_resource(nullptr, list->native, list->id, nullptr, nullptr, false); }, false);
  run(12, false, [&] { win::after_draw(nullptr, list->native, list->id, false); }, false);
  r.lists.erase(list->native);
  r.live_backfill = old_backfill;
  r.observation_epoch = old_epoch;
  r.ready = old_ready;
  win::known_lists = {};
  std::puts("PASS idle submission GPU handlers: timestamp/paired queries, state-disjoint work, RT writers and exact forwarding.");
}
using ClearHook =
    taxi_camera::standalone::StateHook<11, decltype(&ID3D12GraphicsCommandList::ClearState), taxi_camera::standalone::ClearState>;
unsigned outer_clears{}, inner_clears{};
void STDMETHODCALLTYPE inner_clear(ID3D12GraphicsCommandList*, ID3D12PipelineState*) {
  ++inner_clears;
}
void STDMETHODCALLTYPE outer_clear(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) {
  ++outer_clears;
  // Model a native runtime forwarding through the patched method again.
  ClearHook::invoke(inner_clear, list, pipeline);
}
void state_reentry_checks() {
  namespace win = taxi_camera::standalone;
  auto& r = win::registry();
  auto* native = reinterpret_cast<ID3D12GraphicsCommandList*>(0x5000);
  auto item = std::make_shared<win::List>();
  item->native = native;
  item->id = 50;
  item->ready = true;
  item->pfd_dirty = true;
  item->pending_rt = {true, true};
  item->count = 2;
  r.lists[native] = item;
  r.ready = true;
  const auto before = r.clear_states.load();
  ClearHook::invoke(outer_clear, native, nullptr);
  require(outer_clears == 1 && inner_clears == 1, "Reentrant runtime chain forwards each implementation exactly once");
  require(r.clear_states == before + 1, "Reentrant ClearState is observed once, not once per runtime layer");
  require(!item->pfd_dirty && !item->pending_rt[0] && !item->pending_rt[1] && !item->count && item->recording == 1,
          "Outer ClearState clears pending bindings without starting another recording");
  require(!win::owned_depth, "Reentrant state-call guard is released");
  {
    const win::OwnedWork owned;
    ClearHook::invoke(outer_clear, native, nullptr);
    require(win::owned_depth == 1, "Caller-owned state-call guard is preserved");
  }
  r.ready = false;
  ClearHook::invoke(outer_clear, native, nullptr);
  require(outer_clears == 3 && inner_clears == 3 && r.clear_states == before + 1 && !win::owned_depth,
          "Owned or inactive forwarding cannot create observations or leak nesting depth");
  r.lists.erase(native);
  std::puts("PASS state-hook reentry: one observation, exact forwarding, pending bindings cleared, guards retained.");
}
using PipelineHook =
    taxi_camera::standalone::StateHook<25, decltype(&ID3D12GraphicsCommandList::SetPipelineState), taxi_camera::standalone::Pipeline>;
unsigned pipeline_forwards{}, reset_forwards{};
unsigned target_forwards{};
unsigned unsafe_forwards{};
using TargetsHook =
    taxi_camera::standalone::StateHook<46, decltype(&ID3D12GraphicsCommandList::OMSetRenderTargets), taxi_camera::standalone::Targets>;
void STDMETHODCALLTYPE
forward_targets(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*) {
  ++target_forwards;
}
using UnsupportedHook =
    taxi_camera::standalone::StateHook<27, decltype(&ID3D12GraphicsCommandList::ExecuteBundle), taxi_camera::standalone::Unsupported>;
using PredicationHook =
    taxi_camera::standalone::StateHook<55, decltype(&ID3D12GraphicsCommandList::SetPredication), taxi_camera::standalone::Predication>;
void STDMETHODCALLTYPE forward_bundle(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*) {
  ++unsafe_forwards;
}
void STDMETHODCALLTYPE forward_predication(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, D3D12_PREDICATION_OP) {
  ++unsafe_forwards;
}
void STDMETHODCALLTYPE forward_pipeline(ID3D12GraphicsCommandList*, ID3D12PipelineState*) {
  ++pipeline_forwards;
}
enum class ResetResult { success, fail, cross_idle };
ResetResult reset_result{};
HRESULT STDMETHODCALLTYPE forward_reset(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*) {
  ++reset_forwards;
  if (reset_result == ResetResult::cross_idle) {
    taxi_camera::standalone::set_graphics_observation_demand(false);
    taxi_camera::standalone::set_graphics_observation_demand(true);
  }
  return reset_result == ResetResult::fail ? E_FAIL : S_OK;
}
void lookup_benchmark(ID3D12GraphicsCommandList* native) {
  namespace win = taxi_camera::standalone;
  constexpr unsigned iterations = 250000;
  win::set_graphics_diagnostics_enabled(false);
  std::array<std::array<double, 4>, 2> samples{};
  std::array<unsigned, 2> counts{};
  std::uint64_t checksum = 0;
  for (bool uncached : {true, false}) {
    win::bypass_known_list_cache = uncached;
    for (unsigned i = 0; i < 20000; ++i)
      checksum += win::find_list(native)->id;
  }
  // A is the actual registry path with cache reuse disabled only in this CPU
  // fixture. B is the unchanged production cache. ABBA repeats reduce drift;
  // no wall-time threshold is a correctness assertion or simulator FPS claim.
  for (const auto uncached : {true, false, false, true, true, false, false, true}) {
    win::bypass_known_list_cache = uncached;
    const auto before = win::registry_lookup_calls;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < iterations; ++i)
      checksum += win::find_list(native)->id;
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const auto locks = win::registry_lookup_calls - before;
    require(locks == (uncached ? iterations : 0), "Lookup benchmark must exercise all registry locks or all validated cache hits");
    samples[uncached][counts[uncached]++] = elapsed;
    std::printf("{\"lookupMode\":\"%s\",\"operations\":%u,\"registryLocks\":%llu,\"elapsedMs\":%.6f,\"diagnostics\":false}\n",
                uncached ? "locked" : "cached", iterations, locks, elapsed);
  }
  for (auto& sample : samples)
    std::sort(sample.begin(), sample.end());
  std::printf(
      "{\"lookupBenchmark\":\"isolated_ABBA_ABBA\",\"operationsPerBatch\":%u,\"lockedMedianMs\":%.6f,"
      "\"cachedMedianMs\":%.6f,\"checksum\":%llu}\n",
      iterations, (samples[1][1] + samples[1][2]) / 2, (samples[0][1] + samples[0][2]) / 2, checksum);
  win::bypass_known_list_cache = false;
  win::known_lists = {};
}
void known_list_and_idle_checks() {
  namespace win = taxi_camera::standalone;
  auto& r = win::registry();
  auto* native = reinterpret_cast<ID3D12GraphicsCommandList*>(0x6000);
  auto item = std::make_shared<win::List>();
  item->native = native;
  item->id = 60;
  item->ready = true;
  item->observation_epoch = r.observation_epoch.load();
  r.lists[native] = item;
  r.ready = true;
  win::known_lists = {};
  lookup_benchmark(native);
  win::set_graphics_diagnostics_enabled(true);
  constexpr unsigned iterations = 100000;
  win::metadata_lookup_calls = win::registry_lookup_calls = 0;
  const auto active_start = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < iterations; ++i)
    PipelineHook::invoke(forward_pipeline, native, nullptr);
  const auto active_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - active_start).count();
  require(pipeline_forwards == iterations && win::metadata_lookup_calls == iterations && win::registry_lookup_calls == 1,
          "Known production setters must forward each operation and acquire registry once per stable cached recording");
  require(r.list_cache_hits == iterations - 1 && r.list_registry_lookups == 1, "Opt-in cache diagnostics disagree with lock acquisitions");
  ++item->recording;
  require(win::find_list(native) == item && win::registry_lookup_calls == 2, "Reset must invalidate the cached recording generation");
  std::uint64_t thread_locks = 0;
  std::thread other([&] {
    win::find_list(native);
    win::find_list(native);
    thread_locks = win::registry_lookup_calls;
  });
  other.join();
  require(thread_locks == 1, "Known-list cache must be thread-local and independently populate");
  // A cache hit may inspect a generation while another observer sees a Reset.
  // No non-atomic recording fields are touched by this lookup-only worker.
  std::thread reset_observer([&] {
    for (unsigned i = 0; i < 10000; ++i)
      win::find_list(native);
  });
  for (unsigned i = 0; i < 10000; ++i)
    item->recording.fetch_add(1, std::memory_order_release);
  reset_observer.join();
  require(win::find_list(native) == item, "Atomic generation changes must retain the same live object identity");

  const auto old_epoch = item->observation_epoch.load();
  const auto invalidations = r.observation_invalidations.load();
  win::set_graphics_observation_demand(false);
  win::set_graphics_observation_demand(false);
  require(!win::recording_observed(*item) && r.observation_invalidations == invalidations + 1,
          "One idle transition must invalidate old recordings, and repeated idle publication must be a no-op");
  const auto idle_lookups = win::metadata_lookup_calls, idle_locks = win::registry_lookup_calls;
  const auto idle_start = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < iterations; ++i)
    PipelineHook::invoke(forward_pipeline, native, nullptr);
  const auto idle_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - idle_start).count();
  require(pipeline_forwards == iterations * 2 && win::metadata_lookup_calls == idle_lookups && win::registry_lookup_calls == idle_locks &&
              r.idle_state_bypasses == iterations,
          "Settled idle production setters must forward exactly once without any list lookup or registry lock");
  auto target = std::make_shared<win::Resource>();
  target->native = reinterpret_cast<ID3D12Resource*>(0x6100);
  target->id = 61;
  constexpr D3D12_CPU_DESCRIPTOR_HANDLE handle{0x6120};
  r.rtvs[handle.ptr] = {target, DXGI_FORMAT_R8G8B8A8_UNORM, 0, handle.ptr};
  TargetsHook::invoke(forward_targets, native, 1, &handle, FALSE, nullptr);
  require(target_forwards == 1 && item->count == 1 && item->targets[0].resource == target && !item->snapshot_rtvs,
          "Idle OM state must retain real target binding for discovery without creating descriptor snapshots");
  const auto old_sources = win::runtime::manager().statistics().source_draws;
  const auto old_draws = r.draws.load();
  win::after_draw(nullptr, native, item->id, true);
  require(
      target->draws == 1 && r.draws == old_draws && !item->pfd_dirty && win::runtime::manager().statistics().source_draws == old_sources,
      "Idle draw must retain autodetection activity without dirty PFD admission or unrelated source counters");
  ClearHook::invoke(inner_clear, native, nullptr);
  win::after_draw(nullptr, native, item->id, true);
  require(item->count == 0 && target->draws == 1, "Idle ClearState must end target activity without retaining stale RTV bindings");
  r.rtvs.erase(handle.ptr);
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {target->native, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET};
  const auto before_metadata = win::metadata_lookup_calls;
  for (unsigned i = 0; i < 1000; ++i)
    win::observe_legacy(nullptr, native, item->id, barrier, win::boundary::ScopeEnabled);
  require(win::metadata_lookup_calls == before_metadata && r.idle_callback_bypasses >= 1000,
          "Idle source-only barrier callbacks must bypass all bridge PFD list lookups");
  const win::PfdCopyProof::Key target_key{0x6100, 61};
  const auto seed_proof = [&] {
    item->copy_proof.reset(true);
    item->copy_proof.observe_transition(target_key, win::PfdCopyProof::Mode::legacy_rt, "test_rt");
    item->copy_proof.after_draw(target_key);
  };
  seed_proof();
  UnsupportedHook::invoke(forward_bundle, native, nullptr);
  require(unsafe_forwards == 1 && item->copy_proof.mode(target_key) == win::PfdCopyProof::Mode::unknown,
          "Idle unsupported work must still reach conservative invalidation and forward once");
  seed_proof();
  PredicationHook::invoke(forward_predication, native, nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  require(unsafe_forwards == 2 && item->copy_proof.mode(target_key) == win::PfdCopyProof::Mode::legacy_rt,
          "Idle null predication must preserve unconditional-work semantics");
  PredicationHook::invoke(forward_predication, native, target->native, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  require(unsafe_forwards == 3 && item->copy_proof.mode(target_key) == win::PfdCopyProof::Mode::unknown,
          "Idle actual predication must still invalidate recording safety and forward once");
  win::set_graphics_observation_demand(true);
  require(!win::recording_observed(*item) && item->observation_epoch == old_epoch,
          "Resume alone must not revive recording state omitted while idle");
  ClearHook::invoke(inner_clear, native, nullptr);
  require(!win::recording_observed(*item), "ClearState must not renew an idle recording's native Reset proof");
  item->pfd_dirty = true;
  item->pending_rt = {true, true};
  win::stage_pfd(native, item->id);
  win::drain_pfds(native, item->id, true);
  require(item->pfd_dirty, "Stale recording admission must return before consuming any pending shader state");
  win::list_reset.original = reinterpret_cast<void*>(&forward_reset);
  reset_result = ResetResult::success;
  require(win::reset(native, nullptr, nullptr) == S_OK && win::recording_observed(*item),
          "Observed successful native Reset must renew proof");
  reset_result = ResetResult::cross_idle;
  require(win::reset(native, nullptr, nullptr) == S_OK && !win::recording_observed(*item),
          "A native Reset spanning idle and resume must remain stale");
  reset_result = ResetResult::success;
  require(win::reset(native, nullptr, nullptr) == S_OK && win::recording_observed(*item), "Next wholly observed Reset must recover");
  reset_result = ResetResult::fail;
  require(win::reset(native, nullptr, nullptr) == E_FAIL && !item->ready, "Failed native Reset cannot admit a recording");
  require(reset_forwards == 4, "Native Reset must forward exactly once per request");
  win::list_reset.original = nullptr;

  auto replacement = std::make_shared<win::List>();
  replacement->native = native;
  replacement->id = 62;
  item->alive = false;
  r.lists[native] = replacement;
  require(win::find_list(native) == replacement, "Retirement/address reuse must reject cached metadata even while old object is retained");
  std::weak_ptr<win::List> dead = replacement;
  replacement->alive = false;
  r.lists.erase(native);
  replacement.reset();
  require(dead.expired() && !win::find_list(native), "Known-list cache must not retain list metadata after retirement and cleanup");
  item->alive = true;
  r.lists[native] = item;
  require(win::find_list(native) == item, "A previous lookup miss must not hide a newly registered incarnation");
  r.lists.erase(native);
  std::array<std::shared_ptr<win::List>, 9> many{};
  win::known_lists = {};
  for (unsigned i = 0; i < many.size(); ++i) {
    many[i] = std::make_shared<win::List>();
    many[i]->native = reinterpret_cast<ID3D12GraphicsCommandList*>(0x7000 + 0x100 * i);
    many[i]->id = 70 + i;
    r.lists[many[i]->native] = many[i];
    win::find_list(many[i]->native);
  }
  const auto before_eviction = win::registry_lookup_calls;
  require(win::find_list(many[0]->native) == many[0] && win::registry_lookup_calls == before_eviction + 1,
          "Bounded cache eviction must use a fresh registry lookup");
  for (const auto& list : many)
    r.lists.erase(list->native);
  win::known_lists = {};
  win::set_graphics_diagnostics_enabled(false);
  r.ready = false;
  std::printf(
      "{\"stateCallsPerMode\":%u,\"activeRegistryLocks\":1,\"idleRegistryLocks\":0,\"activeMs\":%.3f,\"idleMs\":%.3f,"
      "\"timingDiagnosticOnly\":true,\"nativeGpuCalls\":0}\n",
      iterations, active_ms, idle_ms);
}
void display_session_reset_checks() {
  namespace win = taxi_camera::standalone;
  auto& r = win::registry();
  const auto old_ready = r.ready.load();
  const auto old_verified = r.close_forward_verified;
  const auto old_epoch = r.observation_epoch.load();
  const auto old_floor = r.session_recording_floor.load();
  const auto old_key = r.key;
  const auto* old_profile = r.profile;
  auto item = std::make_shared<win::List>();
  item->native = reinterpret_cast<ID3D12GraphicsCommandList*>(0x790000);
  item->id = 790;
  auto target = std::make_shared<win::Resource>();
  target->native = reinterpret_cast<ID3D12Resource*>(0x890000);
  target->id = 890;
  target->desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  target->desc.Width = 768;
  target->desc.Height = 1024;
  target->desc.DepthOrArraySize = 1;
  target->desc.MipLevels = 5;
  target->desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  target->desc.SampleDesc.Count = 1;
  target->desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  r.ready = r.close_forward_verified = true;
  r.key = 0;
  r.profile = &taxi_camera::profiles::A380;
  r.observation_epoch = 10;  // Already idle: a full reset must still invalidate the recording.
  item->observation_epoch = 10;
  item->submission_proof.reset(1, true);
  item->submission_proof.observe_legacy({reinterpret_cast<std::uint64_t>(target->native), target->id}, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        D3D12_RESOURCE_STATE_COMMON, 0);
  item->submission_proof.close(1, true);
  item->closed_recording = 1;
  r.lists[item->native] = item;
  r.resources[target->native] = target;
  r.routes.select_explicit({target->id, 0});
  r.active_mask = r.calibration_mask = 1;
  target->draws = 8;
  target->submission_activity = 9;
  win::refresh_selected(r);
  const auto generation = r.queue_patch_generation.load();
  win::reset_display_session();
  require(!r.routes.targets[0] && !r.active_mask && !r.calibration_mask && !r.selected_resources[0] && !target->draws &&
              !target->submission_activity && r.queue_patch_generation > generation,
          "Full session reset kept routes, activity or display demand from the previous flight");
  require(
      r.resources.at(target->native) == target && r.lists.at(item->native) == item && target->alive && item->submission_proof.complete(1),
      "Control-thread reset destroyed native lifetime metadata or mutated a replayable recording");
  ID3D12CommandList* batch[]{item->native};
  const auto outcome = static_cast<unsigned>(win::DisplaySubmissionOutcome::generation_changed);
  const auto rejected = r.queue_outcomes[outcome].load();
  taxi_camera::SceneCaptureManager::DisplaySubmissionPlan old_plan;
  win::plan_display_submission(nullptr, nullptr, 1, batch, old_plan);
  require(!old_plan.count && !target->submission_activity && r.queue_outcomes[outcome] == rejected + 1,
          "Old closed display proof survived full reset and credited new-flight activity");
  // Model the proof published by a wholly observed new native Reset/Close.
  item->observation_epoch = r.observation_epoch.load();
  taxi_camera::SceneCaptureManager::DisplaySubmissionPlan fresh_plan;
  win::plan_display_submission(nullptr, nullptr, 1, batch, fresh_plan);
  require(target->submission_activity == 1 && r.queue_outcomes[outcome] == rejected + 1,
          "Fresh-session recording could not rediscover a retained live display");
  win::reset_display_session();
  require(!target->submission_activity && item->observation_epoch < r.session_recording_floor,
          "Repeated reset while idle failed to invalidate the next recording");
  r.lists.erase(item->native);
  r.resources.erase(target->native);
  r.observation_epoch = old_epoch;
  r.session_recording_floor = old_floor;
  r.key = old_key;
  r.profile = old_profile;
  r.ready = old_ready;
  r.close_forward_verified = old_verified;
}
void inventory_checks() {
  namespace win = taxi_camera::standalone;
  auto& r = win::registry();
  std::array<std::shared_ptr<win::Resource>, 40> resources{};
  for (unsigned i = 0; i < resources.size(); ++i) {
    auto item = std::make_shared<win::Resource>();
    item->native = reinterpret_cast<ID3D12Resource*>(0x10000 + i * 0x100);
    item->id = 100 + i;
    item->draws = (i * 17) % resources.size();
    item->desc.Width = r.profile->width;
    item->desc.Height = r.profile->height;
    item->desc.MipLevels = static_cast<UINT16>(r.profile->mips ? r.profile->mips : 1);
    item->desc.Format = static_cast<DXGI_FORMAT>(r.profile->formats[0]);
    r.resources[item->native] = item;
    resources[i] = std::move(item);
  }
  const auto complete = win::pfd_inventory();
  require(complete.size() == resources.size(), "UI inventory must retain the full resource set beyond the IPC display limit");
  require(std::is_sorted(complete.begin(), complete.end(), [](const auto& a, const auto& b) { return a.draws > b.draws; }),
          "Inventory must preserve descending draw ranking after sorting outside the lock");
  auto direct = std::make_unique<taxi_camera::PfdTargetDetector>();
  auto from_ui = std::make_unique<taxi_camera::PfdTargetDetector>();
  direct->configure(*r.profile);
  from_ui->configure(*r.profile);
  for (std::uint64_t time : {1000, 2000, 3000, 4000}) {
    resources[3]->draws += 100;
    resources[17]->draws += 100;
    const auto snapshot = win::pfd_inventory_locked(r), sorted = win::pfd_inventory();
    const auto& a = direct->observe(snapshot.data(), snapshot.size(), time);
    const auto& b = from_ui->observe(sorted.data(), sorted.size(), time);
    require(a.valid == b.valid && a.targets == b.targets && a.stable_windows == b.stable_windows,
            "Detector outcome must be identical for complete unsorted metadata and UI-ranked inventory");
  }
  require(direct->snapshot().valid, "Complete unsorted inventory must still produce an active stable target pair");
  for (const auto& item : resources)
    r.resources.erase(item->native);
}
void unbound_clear_suffix_checks() {
  namespace win = taxi_camera::standalone;
  using Proof = win::PfdSubmissionProof;
  auto& r = win::registry();
  const auto old_ready = r.ready.load();
  const auto old_mask = r.selected_mask.load();
  const auto old_native = r.selected_native[0].load();
  r.ready = true;
  auto list = std::make_shared<win::List>();
  list->native = reinterpret_cast<ID3D12GraphicsCommandList*>(0x993000);
  list->id = 993;
  list->ready = true;
  r.lists[list->native] = list;
  auto resource = std::make_shared<win::Resource>();
  resource->native = reinterpret_cast<ID3D12Resource*>(0xa20000);
  resource->id = 21000;
  const SIZE_T handle = 0x441000;
  win::View view;
  view.resource = resource;
  view.rtv = handle;
  r.rtvs[handle] = view;
  r.selected_native[0].store(resource->native);
  r.selected_mask.store(1);
  const Proof::Key key{reinterpret_cast<std::uint64_t>(resource->native), resource->id};
  const auto clear_generation = ++list->recording;
  list->submission_proof.reset(clear_generation, true);
  list->count = 0;
  using Hook = win::StateHook<48, decltype(&ID3D12GraphicsCommandList::ClearRenderTargetView), win::ClearRenderTarget>;
  Hook::invoke([](ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*) {}, list->native,
               D3D12_CPU_DESCRIPTOR_HANDLE{handle}, nullptr, 0u, nullptr);
  list->submission_proof.close(clear_generation, true);
  require(list->submission_proof.suffix_candidate(key, clear_generation).state_after == D3D12_RESOURCE_STATE_RENDER_TARGET,
          "Unbound ClearRenderTargetView of a selected display did not become a suffix");
  const auto pass_generation = ++list->recording;
  list->submission_proof.reset(pass_generation, true);
  D3D12_RENDER_PASS_RENDER_TARGET_DESC target{};
  target.cpuDescriptor = {handle};
  target.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
  target.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
  win::pass_targets(nullptr, list->native, list->id, 1, &target, nullptr);
  list->submission_proof.close(pass_generation, true);
  require(list->submission_proof.suffix_candidate(key, pass_generation).state_after == D3D12_RESOURCE_STATE_RENDER_TARGET,
          "Render-pass clear of a selected display did not become a suffix");
  const auto preserve_generation = ++list->recording;
  list->submission_proof.reset(preserve_generation, true);
  target.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
  win::pass_targets(nullptr, list->native, list->id, 1, &target, nullptr);
  list->submission_proof.close(preserve_generation, true);
  require(!list->submission_proof.suffix_candidate(key, preserve_generation),
          "A preserving render pass invented a suffix for the selected display");
  r.rtvs.erase(handle);
  r.lists.erase(list->native);
  r.selected_mask.store(old_mask);
  r.selected_native[0].store(old_native);
  r.ready = old_ready;
}
}  // namespace
int main() {
  namespace win = taxi_camera::standalone;
  namespace boundary = taxi_camera::engine_hook::render_boundary;
  try {
    state_reentry_checks();
    descriptor_identity_checks();
    submission_close_endpoint_checks();
    submission_profile_filter_checks();
    submission_gpu_classification_checks();
    unbound_clear_suffix_checks();
    known_list_and_idle_checks();
    display_session_reset_checks();
    inventory_checks();
    auto& r = win::registry();
    auto* native = reinterpret_cast<ID3D12GraphicsCommandList*>(0x1000);
    auto* first_native = reinterpret_cast<ID3D12Resource*>(0x2000);
    auto* second_native = reinterpret_cast<ID3D12Resource*>(0x3000);
    auto* unrelated_native = reinterpret_cast<ID3D12Resource*>(0x4000);
    auto item = std::make_shared<win::List>();
    item->native = native;
    item->id = 40;
    item->ready = true;
    auto first = std::make_shared<win::Resource>(), second = std::make_shared<win::Resource>();
    first->native = first_native;
    first->id = 1001;
    second->native = second_native;
    second->id = 1002;
    r.lists[native] = item;
    r.resources[first_native] = first;
    r.resources[second_native] = second;
    r.routes.select_explicit({1001, 1002});
    r.active_mask = 3;
    win::refresh_selected(r);
    const win::PfdCopyProof::Key first_key{0x2000, 1001}, second_key{0x3000, 1002};
    constexpr auto scope = boundary::ScopeEnabled | boundary::ScopePriorGpuWork;
    const auto initialize = [&] {
      item->copy_proof.reset(true);
      item->pfd_transition = false;
      item->targets[0].resource = first;
      item->count = 1;
      item->pending_pfds[0].resource = first;
      item->pending_pfds[1].resource = second;
      item->pending_rt = {true, true};
    };
    std::vector<D3D12_RESOURCE_BARRIER> barriers(10171);
    for (auto& barrier : barriers) {
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition = {unrelated_native, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    }
    barriers[17] = {};
    barriers[17].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[17].UAV.pResource = unrelated_native;
    barriers[5000].Transition = {first_native, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET};
    barriers.back().Transition = {second_native, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    std::array<double, 2> ms{};
    std::array<std::uint64_t, 2> lookups{};
    for (unsigned batched = 0; batched < 2; ++batched) {
      initialize();
      win::metadata_lookup_calls = 0;
      const auto start = std::chrono::steady_clock::now();
      if (batched)
        win::metadata_begin(nullptr, native, item->id);
      for (unsigned n = 0; n < barriers.size(); ++n) {
        win::observe_legacy(nullptr, native, item->id, barriers[n], scope);
        if (n == 4999)
          require(!item->pfd_transition && item->pending_rt[0] && item->pending_rt[1], "Unrelated metadata changed PFD bindings");
      }
      if (batched)
        win::metadata_end(nullptr, native, item->id);
      ms[batched] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      lookups[batched] = win::metadata_lookup_calls;
      require(item->pfd_transition && !item->pending_rt[0] && !item->pending_rt[1],
              "Matching transitions did not invalidate pending RTT evidence");
      require(item->copy_proof.mode(first_key) == win::PfdCopyProof::Mode::unknown, "Pre-forward metadata became capture permission");
      item->copy_proof.after_draw(first_key);
      item->copy_proof.after_draw(second_key);
      require(item->copy_proof.mode(first_key) == win::PfdCopyProof::Mode::legacy_rt &&
                  item->copy_proof.mode(second_key) == win::PfdCopyProof::Mode::unknown,
              "Batched ordered model differs from direct metadata delivery");
    }
    require(lookups[0] == (barriers.size() - 1) * 3 && lookups[1] == 1, "Production bridge did not resolve once per metadata batch");
    require(!win::metadata_batches.current(native, item->id), "Production metadata end retained its scope");
    initialize();
    D3D12_TEXTURE_BARRIER texture{};
    texture.pResource = first_native;
    texture.LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    texture.AccessAfter = D3D12_BARRIER_ACCESS_RENDER_TARGET;
    texture.Subresources = {0, 1, 0, 1, 0, 1};
    win::metadata_lookup_calls = 0;
    win::metadata_begin(nullptr, native, item->id);
    win::observe_enhanced(nullptr, reinterpret_cast<ID3D12GraphicsCommandList7*>(native), item->id, texture, scope);
    win::metadata_end(nullptr, native, item->id);
    item->copy_proof.after_draw(first_key);
    require(win::metadata_lookup_calls == 1 && item->copy_proof.mode(first_key) == win::PfdCopyProof::Mode::enhanced_rt &&
                !item->pending_rt[0] && item->pending_rt[1],
            "Enhanced metadata did not preserve matching/unrelated semantics");

    win::metadata_begin(nullptr, native, item->id);
    ++item->recording;  // Same fields renewed by the observed successful native Reset.
    item->copy_proof.reset(true);
    win::metadata_lookup_calls = 0;
    win::observe_legacy(nullptr, native, item->id, barriers[5000], scope);
    require(win::metadata_lookup_calls == 3, "Reset recording mismatch did not use a fresh registry lookup");
    auto replacement = std::make_shared<win::List>();
    replacement->native = native;
    replacement->id = 41;
    replacement->copy_proof.reset(true);
    item->alive = false;
    r.lists[native] = replacement;
    win::observe_legacy(nullptr, native, 40, barriers[5000], scope);
    replacement->copy_proof.after_draw(first_key);
    require(replacement->copy_proof.mode(first_key) == win::PfdCopyProof::Mode::unknown,
            "Retired cached generation crossed into replacement List");
    win::metadata_end(nullptr, native, 40);
    win::metadata_begin(nullptr, native, 41);
    win::observe_legacy(nullptr, native, 41, barriers[5000], scope);
    win::metadata_end(nullptr, native, 41);
    replacement->copy_proof.after_draw(first_key);
    require(replacement->copy_proof.mode(first_key) == win::PfdCopyProof::Mode::legacy_rt,
            "Fresh generation failed to establish its own proof");
    r.selected_mask = 0;
    win::metadata_begin(nullptr, native, 41);
    win::observe_legacy(nullptr, native, 41, barriers[5000], scope);
    win::metadata_end(nullptr, native, 41);
    replacement->copy_proof.after_draw(first_key);
    require(replacement->copy_proof.mode(first_key) == win::PfdCopyProof::Mode::unknown, "Deselection failed to clear retained proof");
    std::printf(
        "{\"checks\":%u,\"barriers\":%zu,\"unscopedLookups\":%llu,\"batchedLookups\":%llu,\"unscopedMs\":%.3f,\"batchedMs\":%.3f,"
        "\"nativeGpuCalls\":0}\n",
        checks, barriers.size(), lookups[0], lookups[1], ms[0], ms[1]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
