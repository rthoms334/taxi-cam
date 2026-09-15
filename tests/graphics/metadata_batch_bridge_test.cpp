// Exercise actual production metadata handlers without a simulator or GPU.
#define TAXI_METADATA_BATCH_VALIDATION
#include <chrono>
#include <cstdio>
#include <stdexcept>
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
}  // namespace
int main() {
  namespace win = taxi_camera::standalone;
  namespace boundary = taxi_camera::engine_hook::render_boundary;
  try {
    state_reentry_checks();
    descriptor_identity_checks();
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
