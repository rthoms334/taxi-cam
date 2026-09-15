// Reuse the existing isolated COM fixture without duplicating it.
#define main ordinary_boundary_test_main
#include "../hooks/render_boundary_observer_test.cpp"
#undef main

#ifdef TAXI_SELECTED_BARRIER_FIX
std::array<ID3D12Resource*, 2> proof_targets{};
UINT proof_target_count = 2, proof_selector_calls = 0, proof_selector_capacity = 0;
bool proof_retire = false;
UINT proof_selector(void*, ID3D12GraphicsCommandList* list, std::uint64_t generation, ID3D12Resource** targets, UINT capacity) noexcept {
  ++proof_selector_calls;
  proof_selector_capacity = capacity;
  for (UINT i = 0; i < capacity && i < proof_targets.size(); ++i)
    targets[i] = proof_targets[i];
  if (proof_retire)
    obs::unregister_list(list, generation);
  return proof_target_count;
}
#endif
int main() {
  try {
    std::array<void*, 81> table{};
    table[0] = reinterpret_cast<void*>(&query);
    table[2] = reinterpret_cast<void*>(&release);
    table[8] = reinterpret_cast<void*>(&type);
    table[12] = reinterpret_cast<void*>(&original_draw);
    table[13] = reinterpret_cast<void*>(&original_draw_indexed);
    table[16] = reinterpret_cast<void*>(&original_copy_texture);
    table[17] = reinterpret_cast<void*>(&original_copy_resource);
    table[26] = reinterpret_cast<void*>(&original_legacy);
    table[68] = reinterpret_cast<void*>(&original_begin);
    table[69] = reinterpret_cast<void*>(&original_end);
    table[80] = reinterpret_cast<void*>(&original_enhanced);
    Fake object{table.data()};
    auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(&object);
    auto* first = reinterpret_cast<ID3D12Resource*>(0x1000);
    auto* second = reinterpret_cast<ID3D12Resource*>(0x2000);
    auto supplied = callbacks;
#ifdef TAXI_SELECTED_BARRIER_FIX
    proof_targets = {first, second};
    supplied.selected_legacy_targets = proof_selector;
#endif
    require(obs::register_list(list, 1, supplied).ready, "Register isolated native mock");
    list->DrawInstanced(3, 1, 0, 0);
    auto check = [&](const D3D12_RESOURCE_BARRIER* data, UINT count, unsigned expected, const char* label) {
      auto originals = evidence.legacy, delivered = evidence.legacy_callbacks;
      evidence.order_count = 0;
      list->ResourceBarrier(count, data);
      require(evidence.legacy == originals + 1 && evidence.legacy_pointer == data && evidence.legacy_count == count,
              "Exact once original forwarding");
      require(evidence.legacy_callbacks == delivered + expected, label);
      if (expected)
        require(evidence.order[0] == 1 && evidence.order[expected] == 2, "Callbacks occur before exact native original");
    };
    auto small = legacy_transition(first);
    check(&small, 1, 1, "Small batch positive control");
    std::vector<D3D12_RESOURCE_BARRIER> large(7105);
    for (UINT i = 0; i < large.size(); ++i)
      large[i] = legacy_transition(reinterpret_cast<ID3D12Resource*>(0x4000 + i * 8));
    large[7103] = legacy_transition(first);
    large[7104] = legacy_transition(second);
#ifndef TAXI_SELECTED_BARRIER_FIX
    auto before = evidence.legacy_callbacks, originals = evidence.legacy;
    list->ResourceBarrier(static_cast<UINT>(large.size()), large.data());
    const auto delivered = evidence.legacy_callbacks - before;
    require(evidence.legacy == originals + 1 && evidence.legacy_pointer == large.data() && evidence.legacy_count == 7105,
            "Large baseline exact forwarding");
    require(delivered == 0, "Baseline no longer reproduces reported limit");
    std::printf(
        "{\"baseline_reproduced\":true,\"small_delivered\":1,\"large_count\":7105,\"large_delivered\":%u,\"original_forwarded_once\":true}"
        "\n",
        delivered);
#else
    check(large.data(), 7105, 2, "Both late selected RT exits must be delivered");
    require(proof_selector_calls == 1 && proof_selector_capacity == 2, "Selector once with exact capacity two");
    const auto pristine = large;
    large[0] = legacy_transition(first);
    check(large.data(), 7105, 2, "Duplicate selected exits delivered only once per target");
    large[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    check(large.data(), 7105, 1, "Prior selected transition blocks later RT exit");
    large[0] = legacy_transition(first);
    large[0].Transition.Subresource = 1;
    check(large.data(), 7105, 1, "Prior different subresource blocks later RT exit");
    for (const auto split : {D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY, D3D12_RESOURCE_BARRIER_FLAG_END_ONLY}) {
      large[0] = legacy_transition(first);
      large[0].Flags = split;
      check(large.data(), 7105, 1, "Prior selected split blocks that resource");
    }
    large = pristine;
    large[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
    check(large.data(), 7105, 2, "Unrelated split does not invent global uncertainty");
    for (unsigned variant = 0; variant < 3; ++variant) {
      large = pristine;
      large[0] = {};
      large[0].Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
      large[0].Aliasing.pResourceBefore = variant == 0 ? first : reinterpret_cast<ID3D12Resource*>(0x9000);
      large[0].Aliasing.pResourceAfter = variant == 0 ? second : reinterpret_cast<ID3D12Resource*>(0xA000);
      if (variant == 2)
        large[0].Aliasing = {nullptr, nullptr};
      check(large.data(), 7105, 0, "Earlier related/unrelated/wildcard alias refuses remaining targets");
    }
    large = pristine;
    for (unsigned variant = 0; variant < 3; ++variant) {
      auto bad = legacy_transition(first);
      if (variant == 0)
        bad.Type = static_cast<D3D12_RESOURCE_BARRIER_TYPE>(99);
      if (variant == 1)
        bad.Flags = static_cast<D3D12_RESOURCE_BARRIER_FLAGS>(8);
      if (variant == 2)
        bad.Transition.pResource = nullptr;
      std::array<D3D12_RESOURCE_BARRIER, 2> small_bad{small, bad};
      check(small_bad.data(), 2, 0, "Small batch later malformed metadata blocks earlier valid target");
      large = pristine;
      large[7104] = bad;
      check(large.data(), 7105, 0, "Large batch later malformed metadata blocks earlier valid target");
    }
    std::array<D3D12_RESOURCE_BARRIER, 2> prior_split{legacy_transition(first), small};
    prior_split[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
    check(prior_split.data(), 2, 0, "Small prior split blocks same-resource RT exit");
    prior_split[0] = {};
    prior_split[0].Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    check(prior_split.data(), 2, 0, "Small wildcard alias blocks RT exit");
    auto enhanced_check = [&](const D3D12_BARRIER_GROUP* data, UINT count, unsigned expected, const char* label) {
      const auto originals = evidence.enhanced, delivered = evidence.enhanced_callbacks;
      evidence.order_count = 0;
      reinterpret_cast<ID3D12GraphicsCommandList7*>(list)->Barrier(count, data);
      require(evidence.enhanced == originals + 1 && evidence.enhanced_pointer == data && evidence.enhanced_count == count,
              "Enhanced exact once original forwarding");
      require(evidence.enhanced_callbacks == delivered + expected, label);
      if (expected)
        require(evidence.order[0] == 5 && evidence.order[expected] == 4, "Enhanced callback before original");
    };
    std::array<D3D12_TEXTURE_BARRIER, 2> textures{texture_transition(first), texture_transition(second)};
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 2;
    group.pTextureBarriers = textures.data();
    enhanced_check(&group, 1, 2, "Enhanced positive control");
    textures[0].pResource = second;
    textures[0].SyncBefore = D3D12_BARRIER_SYNC_SPLIT;
    enhanced_check(&group, 1, 0, "Enhanced prior same-resource split blocks RT exit");
    textures[0] = texture_transition(first);
    textures[0].Flags = D3D12_TEXTURE_BARRIER_FLAG_DISCARD;
    enhanced_check(&group, 1, 0, "Enhanced prior discard blocks RT exit");
    for (unsigned variant = 0; variant < 2; ++variant) {
      textures = {texture_transition(first), texture_transition(second)};
      if (variant == 0)
        textures[1].Flags = static_cast<D3D12_TEXTURE_BARRIER_FLAGS>(8);
      else
        textures[1].pResource = nullptr;
      enhanced_check(&group, 1, 0, "Enhanced later malformed metadata blocks earlier valid target");
    }
    textures = {texture_transition(first), texture_transition(second)};
    std::array<D3D12_BARRIER_GROUP, 2> groups{group, group};
    groups[1].Type = static_cast<D3D12_BARRIER_TYPE>(99);
    enhanced_check(groups.data(), 2, 0, "Enhanced malformed group refuses whole batch");
    large = pristine;
    proof_target_count = 1;
    check(large.data(), 7105, 1, "Valid single selection");
    for (const auto bad_count : {0u, 3u}) {
      proof_target_count = bad_count;
      check(large.data(), 7105, 0, "Zero/overcapacity selection refused");
    }
    proof_target_count = 2;
    for (const auto bad_targets : {std::array<ID3D12Resource*, 2>{nullptr, second}, std::array<ID3D12Resource*, 2>{first, nullptr},
                                   std::array<ID3D12Resource*, 2>{first, first}}) {
      proof_targets = bad_targets;
      check(large.data(), 7105, 0, "Null/duplicate selection refused");
    }
    proof_targets = {first, second};
    const auto before_bad_span = proof_selector_calls;
    check(nullptr, 7105, 0, "Null span refuses before selector");
    check(reinterpret_cast<const D3D12_RESOURCE_BARRIER*>(UINTPTR_MAX - 7), 7105, 0, "Overflowing span refuses before selector");
    check(nullptr, obs::maximum_legacy_metadata_barriers + 1, 0, "Overcap count refuses before selector");
    require(proof_selector_calls == before_bad_span, "Invalid metadata cannot invoke selector");
    obs::successful_reset(list, 1);
    auto selectors = proof_selector_calls;
    check(large.data(), 7105, 0, "No prior work refuses selected injection");
    require(proof_selector_calls == selectors, "No prior work refuses before selection");
    list->DrawInstanced(3, 1, 0, 0);
    auto* pass_list = reinterpret_cast<ID3D12GraphicsCommandList4*>(list);
    pass_list->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    selectors = proof_selector_calls;
    check(large.data(), 7105, 0, "Active pass refuses selected injection");
    require(proof_selector_calls == selectors, "Active pass refuses before selection");
    pass_list->EndRenderPass();
    check(large.data(), 7105, 2, "Completed ordinary pass restores admission");
    pass_list->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
    pass_list->EndRenderPass();
    check(large.data(), 7105, 0, "Suspended pass refuses selected injection");
    obs::successful_reset(list, 1);
    list->DrawInstanced(3, 1, 0, 0);
    obs::invalidate_recording(list, 1);
    check(large.data(), 7105, 0, "Explicit invalid recording refuses selected injection");
    obs::successful_reset(list, 1);
    list->DrawInstanced(3, 1, 0, 0);
    proof_retire = true;
    check(large.data(), 7105, 0, "Selector generation retirement refuses all callbacks");
    proof_retire = false;
    require(obs::register_list(list, 1, supplied).ready, "Reregister after selector retirement");
    list->DrawInstanced(3, 1, 0, 0);
    evidence.retire = true;
    check(large.data(), 7105, 1, "First callback retirement excludes second selected callback");
    evidence.retire = false;
    selectors = proof_selector_calls;
    check(large.data(), 7105, 0, "Unknown list forwards without selection");
    require(proof_selector_calls == selectors, "Unknown list selector was not called");
    require(obs::register_list(list, 1, supplied).ready, "Reregister after callback retirement");
    auto incompatible = supplied;
    incompatible.selected_legacy_targets = nullptr;
    require(!obs::register_list(list, 1, incompatible).ready, "Selector participates in immutable callback identity");
    std::printf(
        "{\"passed\":true,\"checks\":%llu,\"large_count\":7105,\"selected_delivered\":2,\"small_and_enhanced_uncertainty_refused\":true,"
        "\"original_forwarded_once\":true}\n",
        static_cast<unsigned long long>(checks));
#endif
    obs::unregister_list(list, 1);
    const auto removed = obs::remove();
    require(std::string(removed.status) == "removed" && removed.protection_restored && !obs::operational(),
            "Restore isolated original slots");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL isolated proof: %s\n", error.what());
    return 1;
  }
}
