// Exercise actual production metadata handlers without a simulator or GPU.
#define TAXI_METADATA_BATCH_VALIDATION
#include "d3d12_bridge.cpp"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
unsigned checks{};
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
}
int main() {
  namespace win = taxi_camera::standalone;
  namespace boundary = taxi_camera::engine_hook::render_boundary;
  try {
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
    barriers.back().Transition = {second_native, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
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
      require(item->pfd_transition && !item->pending_rt[0] && !item->pending_rt[1], "Matching transitions did not invalidate pending RTT evidence");
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
                !item->pending_rt[0] && item->pending_rt[1], "Enhanced metadata did not preserve matching/unrelated semantics");

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
    require(replacement->copy_proof.mode(first_key) == win::PfdCopyProof::Mode::legacy_rt, "Fresh generation failed to establish its own proof");
    r.selected_mask = 0;
    win::metadata_begin(nullptr, native, 41);
    win::observe_legacy(nullptr, native, 41, barriers[5000], scope);
    win::metadata_end(nullptr, native, 41);
    replacement->copy_proof.after_draw(first_key);
    require(replacement->copy_proof.mode(first_key) == win::PfdCopyProof::Mode::unknown, "Deselection failed to clear retained proof");
    std::printf("{\"checks\":%u,\"barriers\":%zu,\"unscopedLookups\":%llu,\"batchedLookups\":%llu,\"unscopedMs\":%.3f,\"batchedMs\":%.3f,\"nativeGpuCalls\":0}\n",
                checks, barriers.size(), lookups[0], lookups[1], ms[0], ms[1]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}