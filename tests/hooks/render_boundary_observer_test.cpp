#include "../../src/hooks/render_boundary_observer.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace obs = taxi_camera::engine_hook::render_boundary;
namespace {
std::atomic<unsigned> protect_calls{0};
std::atomic<unsigned> query_calls{0}, rpm_calls{0};
unsigned metadata_begins = 0, metadata_ends = 0, metadata_depth = 0, metadata_errors = 0;
bool retire_metadata = false, retire_metadata_begin = false;
void metadata_begin(void*, ID3D12GraphicsCommandList* list, std::uint64_t generation) noexcept {
  if (retire_metadata_begin)
    obs::unregister_list(list, generation);
  ++metadata_begins;
  if (++metadata_depth != 1)
    ++metadata_errors;
}
void metadata_end(void*, ID3D12GraphicsCommandList*, std::uint64_t) noexcept {
  ++metadata_ends;
  if (metadata_depth != 1)
    ++metadata_errors;
  metadata_depth = 0;
}
unsigned fail_protect_call = 0;
unsigned fail_rpm_call = 0, short_rpm_call = 0;
SIZE_T last_rpm_bytes = 0;
std::uint64_t checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
struct Fake {
  void** table;
  void** after_begin = nullptr;
  void** after_end = nullptr;
};
using Legacy = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using Begin = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*,
                                       UINT,
                                       const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                                       const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,
                                       D3D12_RENDER_PASS_FLAGS);
using End = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*);
using Enhanced = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList7*, UINT, const D3D12_BARRIER_GROUP*);
struct Evidence {
  unsigned legacy = 0, enhanced = 0, begins = 0, ends = 0, releases = 0, legacy_callbacks = 0, enhanced_callbacks = 0;
  unsigned draws = 0, indexed_draws = 0;
  unsigned after_draws = 0, draw_originals_seen = 0, invalidations = 0, invalid_begins_seen = 0, invalid_barriers_seen = 0;
  unsigned invalid_raw_seen = 0;
  std::uint32_t invalid_reasons = 0;
  bool draw_allowed = false, nested_draw = false, retire_draw = false, invalidation_reentered = false, reenter_invalidation = false;
  unsigned active_ends = 0;
  unsigned raw_legacy = 0, raw_enhanced = 0, copy_resources = 0, copy_textures = 0, after_resources = 0, after_textures = 0;
  std::uint32_t raw_scope = 0;
  D3D12_RESOURCE_STATES raw_before = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_BARRIER_TYPE raw_type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  ID3D12Resource* raw_alias_before = nullptr;
  ID3D12Resource* raw_alias_after = nullptr;
  bool copy_allowed = false, retire_copy_original = false;
  ID3D12Resource* copy_source = nullptr;
  ID3D12Resource* copy_destination = nullptr;
  const D3D12_TEXTURE_COPY_LOCATION* source_location = nullptr;
  const D3D12_TEXTURE_COPY_LOCATION* destination_location = nullptr;
  const D3D12_BOX* copy_box = nullptr;
  std::array<UINT, 3> copy_offsets{};
  std::array<UINT, 4> draw_arguments{};
  INT vertex_offset = 0;
  UINT legacy_count = 0, enhanced_count = 0, begin_count = 0;
  const D3D12_RESOURCE_BARRIER* legacy_pointer = nullptr;
  const D3D12_BARRIER_GROUP* enhanced_pointer = nullptr;
  const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets = nullptr;
  const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth = nullptr;
  D3D12_RENDER_PASS_FLAGS pass_flags = D3D12_RENDER_PASS_FLAG_NONE;
  ID3D12GraphicsCommandList* forwarded_list = nullptr;
  ID3D12Resource* callback_resource = nullptr;
  std::uint64_t callback_generation = 0;
  std::array<unsigned, 64> order{};
  unsigned order_count = 0;
  bool nested = false, retire = false;
} evidence;
void note(unsigned value) {
  if (evidence.order_count < evidence.order.size())
    evidence.order[evidence.order_count++] = value;
}
bool interface_available = true;
void* interface_override = nullptr;
D3D12_COMMAND_LIST_TYPE list_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
HRESULT STDMETHODCALLTYPE query(ID3D12GraphicsCommandList* list, REFIID, void** result) {
  *result = interface_available ? (interface_override ? interface_override : list) : nullptr;
  return interface_available ? S_OK : E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE release(ID3D12GraphicsCommandList*) {
  ++evidence.releases;
  return 1;
}
D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE type(ID3D12GraphicsCommandList*) {
  return list_type;
}
void STDMETHODCALLTYPE original_draw(ID3D12GraphicsCommandList* list, UINT count, UINT instances, UINT first, UINT first_instance) {
  ++evidence.draws;
  evidence.forwarded_list = list;
  evidence.draw_arguments = {count, instances, first, first_instance};
  if (evidence.retire_draw)
    obs::unregister_list(list, 2);
}
void STDMETHODCALLTYPE
original_draw_indexed(ID3D12GraphicsCommandList* list, UINT count, UINT instances, UINT first, INT offset, UINT first_instance) {
  ++evidence.indexed_draws;
  evidence.forwarded_list = list;
  evidence.draw_arguments = {count, instances, first, first_instance};
  evidence.vertex_offset = offset;
}
void STDMETHODCALLTYPE original_legacy(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
  if (metadata_depth)
    ++metadata_errors;
  ++evidence.legacy;
  evidence.forwarded_list = list;
  evidence.legacy_count = count;
  evidence.legacy_pointer = barriers;
  note(2);
}
void STDMETHODCALLTYPE original_enhanced(ID3D12GraphicsCommandList7* list, UINT count, const D3D12_BARRIER_GROUP* groups) {
  if (metadata_depth)
    ++metadata_errors;
  ++evidence.enhanced;
  evidence.forwarded_list = list;
  evidence.enhanced_count = count;
  evidence.enhanced_pointer = groups;
  note(4);
}
void STDMETHODCALLTYPE original_begin(ID3D12GraphicsCommandList4* list,
                                      UINT count,
                                      const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets,
                                      const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth,
                                      D3D12_RENDER_PASS_FLAGS flags) {
  ++evidence.begins;
  evidence.forwarded_list = list;
  evidence.begin_count = count;
  evidence.targets = targets;
  evidence.depth = depth;
  evidence.pass_flags = flags;
  auto* object = reinterpret_cast<Fake*>(list);
  if (object->after_begin)
    object->table = object->after_begin;
}
void STDMETHODCALLTYPE original_end(ID3D12GraphicsCommandList4* list) {
  ++evidence.ends;
  evidence.forwarded_list = list;
  auto* object = reinterpret_cast<Fake*>(list);
  if (object->after_end)
    object->table = object->after_end;
}
void STDMETHODCALLTYPE original_active_end(ID3D12GraphicsCommandList4* list) {
  ++evidence.active_ends;
  original_end(list);
}
void STDMETHODCALLTYPE original_copy_resource(ID3D12GraphicsCommandList* list, ID3D12Resource* destination, ID3D12Resource* source) {
  ++evidence.copy_resources;
  evidence.forwarded_list = list;
  evidence.copy_destination = destination;
  evidence.copy_source = source;
  note(7);
  if (evidence.retire_copy_original)
    obs::unregister_list(list, 2);
}
void STDMETHODCALLTYPE original_copy_texture(ID3D12GraphicsCommandList* list,
                                             const D3D12_TEXTURE_COPY_LOCATION* destination,
                                             UINT x,
                                             UINT y,
                                             UINT z,
                                             const D3D12_TEXTURE_COPY_LOCATION* source,
                                             const D3D12_BOX* box) {
  ++evidence.copy_textures;
  evidence.forwarded_list = list;
  evidence.destination_location = destination;
  evidence.source_location = source;
  evidence.copy_box = box;
  evidence.copy_offsets = {x, y, z};
  note(9);
}
void callback_legacy(void*,
                     ID3D12GraphicsCommandList* list,
                     std::uint64_t generation,
                     const D3D12_RESOURCE_TRANSITION_BARRIER& value) noexcept {
  if (metadata_depth)
    ++metadata_errors;
  ++evidence.legacy_callbacks;
  evidence.callback_generation = generation;
  evidence.callback_resource = value.pResource;
  note(1);
  if (evidence.nested) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = value;
    list->ResourceBarrier(1, &barrier);
    note(3);
  }
  if (evidence.retire)
    obs::unregister_list(list, generation);
}
void callback_enhanced(void*, ID3D12GraphicsCommandList7* list, std::uint64_t generation, const D3D12_TEXTURE_BARRIER& value) noexcept {
  if (metadata_depth)
    ++metadata_errors;
  ++evidence.enhanced_callbacks;
  evidence.callback_generation = generation;
  evidence.callback_resource = value.pResource;
  note(5);
  if (evidence.nested) {
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1;
    group.pTextureBarriers = &value;
    list->Barrier(1, &group);
    note(6);
  }
  if (evidence.retire)
    obs::unregister_list(list, generation);
}
void raw_legacy(void*, ID3D12GraphicsCommandList* list, std::uint64_t generation, const D3D12_RESOURCE_BARRIER& barrier, std::uint32_t flags) noexcept {
  if (metadata_depth != 1)
    ++metadata_errors;
  if (retire_metadata)
    obs::unregister_list(list, generation);
  ++evidence.raw_legacy;
  evidence.raw_scope = flags;
  evidence.raw_type = barrier.Type;
  if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
    evidence.raw_alias_before = barrier.Aliasing.pResourceBefore;
    evidence.raw_alias_after = barrier.Aliasing.pResourceAfter;
  }
  evidence.raw_before =
      barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ? barrier.Transition.StateBefore : D3D12_RESOURCE_STATE_COMMON;
}
void raw_enhanced(void*, ID3D12GraphicsCommandList7*, std::uint64_t, const D3D12_TEXTURE_BARRIER&, std::uint32_t flags) noexcept {
  if (metadata_depth != 1)
    ++metadata_errors;
  ++evidence.raw_enhanced;
  evidence.raw_scope = flags;
}
void after_resource(void*,
                    ID3D12GraphicsCommandList* list,
                    std::uint64_t generation,
                    ID3D12Resource* destination,
                    ID3D12Resource* source,
                    bool allowed) noexcept {
  ++evidence.after_resources;
  evidence.callback_generation = generation;
  evidence.copy_allowed = allowed;
  note(8);
  if (evidence.nested)
    list->CopyResource(destination, source);
}
void after_texture(void*,
                   ID3D12GraphicsCommandList* list,
                   std::uint64_t generation,
                   const D3D12_TEXTURE_COPY_LOCATION* destination,
                   UINT x,
                   UINT y,
                   UINT z,
                   const D3D12_TEXTURE_COPY_LOCATION* source,
                   const D3D12_BOX* box,
                   bool allowed) noexcept {
  ++evidence.after_textures;
  evidence.callback_generation = generation;
  evidence.copy_allowed = allowed;
  note(10);
  if (evidence.nested)
    list->CopyTextureRegion(destination, x, y, z, source, box);
}
void after_draw(void*, ID3D12GraphicsCommandList* list, std::uint64_t generation, bool allowed) noexcept {
  ++evidence.after_draws;
  evidence.draw_allowed = allowed;
  evidence.draw_originals_seen = evidence.draws + evidence.indexed_draws;
  evidence.callback_generation = generation;
  if (evidence.nested_draw)
    list->DrawInstanced(3, 1, 0, 0);
}
void invalidated(void*, ID3D12GraphicsCommandList* list, std::uint64_t generation, std::uint32_t reasons) noexcept {
  ++evidence.invalidations;
  evidence.invalid_reasons = reasons;
  evidence.invalid_begins_seen = evidence.begins;
  evidence.invalid_barriers_seen = evidence.legacy + evidence.enhanced;
  evidence.invalid_raw_seen = evidence.raw_legacy + evidence.raw_enhanced;
  if (evidence.reenter_invalidation) {
    // Acquiring the observer registry lock here must not deadlock. This is only
    // a synthetic verification of the callback's unlocked lifecycle contract.
    obs::successful_reset(list, generation);
    evidence.invalidation_reentered = true;
  }
}
const obs::Callbacks callbacks{nullptr,        callback_legacy, callback_enhanced, raw_legacy, raw_enhanced,
                               after_resource, after_texture,   after_draw,        invalidated, nullptr, nullptr, nullptr, metadata_begin, metadata_end};
D3D12_RESOURCE_BARRIER legacy_transition(ID3D12Resource* resource) {
  D3D12_RESOURCE_BARRIER value{};
  value.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  value.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
  return value;
}
D3D12_TEXTURE_BARRIER texture_transition(ID3D12Resource* resource) {
  D3D12_TEXTURE_BARRIER value{};
  value.pResource = resource;
  value.SyncBefore = D3D12_BARRIER_SYNC_RENDER_TARGET;
  value.SyncAfter = D3D12_BARRIER_SYNC_PIXEL_SHADING;
  value.AccessBefore = D3D12_BARRIER_ACCESS_RENDER_TARGET;
  value.AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
  value.LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
  value.LayoutAfter = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
  value.Subresources.IndexOrFirstMipLevel = UINT_MAX;
  return value;
}
}  // namespace
extern "C" BOOL taxi_boundary_test_virtual_protect(void* address, SIZE_T bytes, DWORD access, PDWORD previous) noexcept {
  if (++protect_calls == fail_protect_call) {
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  return VirtualProtect(address, bytes, access, previous);
}
extern "C" SIZE_T taxi_boundary_test_virtual_query(const void* address, MEMORY_BASIC_INFORMATION* info, SIZE_T size) noexcept {
  ++query_calls;
  return VirtualQuery(address, info, size);
}
extern "C" BOOL taxi_boundary_test_read_process_memory(HANDLE process,
                                                       const void* address,
                                                       void* buffer,
                                                       SIZE_T count,
                                                       SIZE_T* read) noexcept {
  const auto index = ++rpm_calls;
  last_rpm_bytes = count;
  if (index == fail_rpm_call) {
    *read = 0;
    SetLastError(ERROR_PARTIAL_COPY);
    return FALSE;
  }
  const auto result = ReadProcessMemory(process, address, buffer, count, read);
  if (index == short_rpm_call && result)
    *read = count - 1;
  return result;
}
int main() {
  try {
    std::array<void*, 81> table{};
    std::array<std::array<void*, 81>, 9> active_tables{};
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
    Fake object{table.data()}, unknown{table.data()};
    auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(&object);
    auto* extended = reinterpret_cast<ID3D12GraphicsCommandList7*>(list);
    auto* resource = reinterpret_cast<ID3D12Resource*>(0x1000);
    auto* other_resource = reinterpret_cast<ID3D12Resource*>(0x2000);
    auto transition = legacy_transition(resource);
    require(!obs::register_list(nullptr, 1, callbacks).ready, "Null list accepted");
    require(!obs::register_list(list, 0, callbacks).ready, "Zero generation accepted");
    interface_available = false;
    require(!obs::register_list(list, 1, callbacks).ready, "Missing native interface7 accepted");
    interface_available = true;
    interface_override = &unknown;
    require(!obs::register_list(list, 1, callbacks).ready, "Different interface pointer accepted");
    interface_override = nullptr;
    list_type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    require(!obs::register_list(list, 1, callbacks).ready, "Compute list accepted");
    list_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    fail_protect_call = 1;
    auto result = obs::register_list(list, 1, callbacks);
    require(!result.ready && result.protection_restored && !obs::operational(), "Initial protection failure");
    fail_protect_call = protect_calls + 2;
    result = obs::register_list(list, 1, callbacks);
    require(!result.ready && !result.protection_restored && !obs::operational(), "Restore failure lost");
    list->ResourceBarrier(1, &transition);
    require(evidence.legacy == 1 && evidence.legacy_callbacks == 0 && evidence.legacy_pointer == &transition, "Partial install forwarding");
    fail_protect_call = protect_calls + 1;
    require(!obs::repair_protection().protection_restored, "Pending restore failure ignored");
    fail_protect_call = 0;
    require(obs::repair_protection().protection_restored, "Protection repair");
    require(obs::register_list(list, 1, callbacks).ready && obs::operational(), "Complete installation");
    list->DrawInstanced(17, 19, 23, 29);
    require(evidence.draws == 1 && evidence.forwarded_list == list && evidence.draw_arguments == std::array<UINT, 4>{17, 19, 23, 29},
            "Native DrawInstanced exact forwarding");
    auto changed_callbacks = callbacks;
    changed_callbacks.context = &object;
    require(!obs::register_list(list, 1, changed_callbacks).ready, "Changed callback context accepted");
    auto other_table = table;
    Fake other_object{other_table.data()};
    require(!obs::register_list(reinterpret_cast<ID3D12GraphicsCommandList*>(&other_object), 1, callbacks).ready,
            "Different vtable accepted");
    evidence.order_count = 0;
    list->ResourceBarrier(1, &transition);
    require(evidence.legacy == 2 && evidence.legacy_callbacks == 1 && evidence.legacy_pointer == &transition &&
                evidence.legacy_count == 1 && evidence.callback_resource == resource && evidence.callback_generation == 1 &&
                evidence.order[0] == 1 && evidence.order[1] == 2,
            "Legacy callback not before exact original");
    auto check_legacy = [&](UINT count, const D3D12_RESOURCE_BARRIER* values, unsigned expected, const char* message) {
      const auto before = evidence.legacy_callbacks, originals = evidence.legacy;
      list->ResourceBarrier(count, values);
      require(evidence.legacy_callbacks == before + expected && evidence.legacy == originals + 1 && evidence.legacy_pointer == values &&
                  evidence.legacy_count == count,
              message);
    };
    check_legacy(0, nullptr, 0, "Empty legacy forwarding");
    check_legacy(1, nullptr, 0, "Null legacy forwarding");
    std::vector<D3D12_RESOURCE_BARRIER> metadata_barriers(4097, transition);
    check_legacy(257, metadata_barriers.data(), 0, "Legacy batch limit");
    auto invalid = transition;
    invalid.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
    check_legacy(1, &invalid, 0, "Begin split legacy accepted");
    invalid.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;
    check_legacy(1, &invalid, 0, "End split legacy accepted");
    invalid = transition;
    invalid.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    check_legacy(1, &invalid, 0, "Unknown legacy state accepted");
    invalid = transition;
    invalid.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    check_legacy(1, &invalid, 0, "No state exit accepted");
    invalid = transition;
    invalid.Transition.pResource = nullptr;
    check_legacy(1, &invalid, 0, "Null native resource accepted");
    invalid = transition;
    invalid.Transition.Subresource = 1;
    check_legacy(1, &invalid, 0, "Other legacy subresource accepted");
    invalid.Transition.Subresource = 0;
    check_legacy(1, &invalid, 1, "Legacy sole subresource refused");
    std::array<D3D12_RESOURCE_BARRIER, 2> batch{transition, transition};
    check_legacy(2, batch.data(), 1, "Earlier same-resource transition not refused");
    batch[0] = legacy_transition(other_resource);
    check_legacy(2, batch.data(), 2, "Unrelated transition incorrectly refused");
    batch[0] = {};
    batch[0].Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    check_legacy(2, batch.data(), 0, "Earlier alias accepted");
    batch[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    batch[0].UAV.pResource = resource;
    check_legacy(2, batch.data(), 1, "Earlier UAV incorrectly changed layout proof");
    batch[0] = transition;
    batch[1] = {};
    batch[1].Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    check_legacy(2, batch.data(), 1, "Later alias incorrectly invalidates actual pre-batch state");
    evidence.nested = true;
    evidence.order_count = 0;
    const auto nested_before = evidence.legacy_callbacks, original_before = evidence.legacy;
    list->ResourceBarrier(1, &transition);
    evidence.nested = false;
    require(evidence.legacy_callbacks == nested_before + 1 && evidence.legacy == original_before + 2 && evidence.order[0] == 1 &&
                evidence.order[1] == 2 && evidence.order[2] == 3 && evidence.order[3] == 2 && evidence.legacy_pointer == &transition,
            "Nested legacy injection recursion/order");
    {
      const obs::ScopedBypass bypass;
      check_legacy(1, &transition, 0, "Explicit bypass notification");
    }
    const auto unknown_before = evidence.legacy_callbacks;
    reinterpret_cast<ID3D12GraphicsCommandList*>(&unknown)->ResourceBarrier(1, &transition);
    require(
        evidence.legacy_callbacks == unknown_before && evidence.forwarded_list == reinterpret_cast<ID3D12GraphicsCommandList*>(&unknown),
        "Unknown object not forwarded exactly");

    D3D12_RENDER_PASS_RENDER_TARGET_DESC pass_target{};
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC pass_depth{};
    extended->BeginRenderPass(1, &pass_target, &pass_depth, D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES);
    require(evidence.begins == 1 && evidence.targets == &pass_target && evidence.depth == &pass_depth && evidence.begin_count == 1 &&
                evidence.pass_flags == D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES,
            "Begin pass forwarding");
    check_legacy(1, &transition, 0, "Active pass copy accepted");
    extended->EndRenderPass();
    require(evidence.ends == 1, "End pass forwarding");
    check_legacy(1, &transition, 1, "Completed pass permanently disables later explicit transition");
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
    extended->EndRenderPass();
    check_legacy(1, &transition, 0, "Suspended pass copy accepted");
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_RESUMING_PASS);
    check_legacy(1, &transition, 0, "Resumed active pass copy accepted");
    extended->EndRenderPass();
    check_legacy(1, &transition, 1, "Completed resumed pass cannot recover");
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_RESUMING_PASS);
    extended->EndRenderPass();
    check_legacy(1, &transition, 1, "Cross-list resumed pass cannot finish");
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    extended->EndRenderPass();
    check_legacy(1, &transition, 0, "Invalid nested pass accepted");
    obs::successful_reset(list, 9);
    check_legacy(1, &transition, 0, "Stale reset cleared invalid state");
    obs::successful_reset(list, 1);
    check_legacy(1, &transition, 0, "Reset alone authorized a possible future resuming first pass");
    {
      const obs::ScopedBypass bypass;
      list->DrawInstanced(3, 1, 0, 0);
    }
    check_legacy(1, &transition, 0, "Internal draw authorized application capture");
    list->DrawIndexedInstanced(31, 37, 41, -47, 53);
    require(evidence.indexed_draws == 1 && evidence.draw_arguments == std::array<UINT, 4>{31, 37, 41, 53} && evidence.vertex_offset == -47,
            "Native indexed draw exact signed argument forwarding");
    check_legacy(1, &transition, 1, "Successful reset did not clear pass state");
    extended->EndRenderPass();
    check_legacy(1, &transition, 0, "Unmatched end accepted");
    obs::successful_reset(list, 1);
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
    extended->EndRenderPass();
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    extended->EndRenderPass();
    check_legacy(1, &transition, 0, "Missing resume after suspension accepted");
    obs::successful_reset(list, 1);
    obs::reset_failed(list, 1);
    check_legacy(1, &transition, 0, "Failed reset allowed capture");
    obs::successful_reset(list, 1);
    check_legacy(1, &transition, 0, "Initial transition before cross-list resumed pass authorized");
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_RESUMING_PASS);
    list->DrawInstanced(3, 1, 0, 0);
    check_legacy(1, &transition, 0, "Draw inside resuming pass authorized capture");
    extended->EndRenderPass();
    check_legacy(1, &transition, 1, "Completed ordinary resumed pass did not authorize later transition");
    for (UINT access = 4; access <= 6; ++access) {
      obs::successful_reset(list, 1);
      list->DrawInstanced(3, 1, 0, 0);
      D3D12_RENDER_PASS_RENDER_TARGET_DESC local{};
      local.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
      local.EndingAccess.Type = static_cast<D3D12_RENDER_PASS_ENDING_ACCESS_TYPE>(access);
      extended->BeginRenderPass(1, &local, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
      extended->EndRenderPass();
      check_legacy(1, &transition, 0, "Local-preservation end access authorized an intervening copy");
      obs::successful_reset(list, 1);
      local.BeginningAccess.Type = static_cast<D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE>(access);
      local.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
      extended->BeginRenderPass(1, &local, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
      extended->EndRenderPass();
      check_legacy(1, &transition, 0, "Local-preservation beginning access did not invalidate recording");
    }
    obs::successful_reset(list, 1);
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC local_depth{};
    local_depth.DepthEndingAccess.Type = static_cast<D3D12_RENDER_PASS_ENDING_ACCESS_TYPE>(4);
    extended->BeginRenderPass(0, nullptr, &local_depth, D3D12_RENDER_PASS_FLAG_NONE);
    extended->EndRenderPass();
    list->DrawInstanced(3, 1, 0, 0);
    check_legacy(1, &transition, 0, "Local-preservation depth end access authorized unrelated work");
    obs::successful_reset(list, 1);
    list->DrawInstanced(3, 1, 0, 0);

    auto texture = texture_transition(resource);
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1;
    group.pTextureBarriers = &texture;
    auto check_enhanced = [&](UINT count, const D3D12_BARRIER_GROUP* groups, unsigned expected, const char* message) {
      const auto before = evidence.enhanced_callbacks, originals = evidence.enhanced;
      extended->Barrier(count, groups);
      require(evidence.enhanced_callbacks == before + expected && evidence.enhanced == originals + 1 &&
                  evidence.enhanced_pointer == groups && evidence.enhanced_count == count,
              message);
    };
    evidence.order_count = 0;
    check_enhanced(1, &group, 1, "Enhanced callback absent");
    require(evidence.order[0] == 5 && evidence.order[1] == 4 && evidence.callback_resource == resource, "Enhanced callback after original");
    check_enhanced(0, nullptr, 0, "Empty enhanced forwarding");
    check_enhanced(1, nullptr, 0, "Null enhanced forwarding");
    std::array<D3D12_BARRIER_GROUP, 65> metadata_groups{};
    for (auto& metadata_group : metadata_groups)
      metadata_group = group;
    check_enhanced(17, metadata_groups.data(), 0, "Enhanced group cap");
    std::vector<D3D12_TEXTURE_BARRIER> metadata_textures(4097, texture);
    group.NumBarriers = 257;
    group.pTextureBarriers = metadata_textures.data();
    check_enhanced(1, &group, 0, "Enhanced barrier cap");
    group.NumBarriers = 1;
    group.pTextureBarriers = nullptr;
    check_enhanced(1, &group, 0, "Null enhanced array");
    group.pTextureBarriers = &texture;
    texture.Flags = D3D12_TEXTURE_BARRIER_FLAG_DISCARD;
    check_enhanced(1, &group, 0, "Enhanced discard candidate accepted");
    texture = texture_transition(resource);
    texture.SyncBefore = D3D12_BARRIER_SYNC_SPLIT;
    check_enhanced(1, &group, 0, "Enhanced end split accepted");
    texture = texture_transition(resource);
    texture.SyncAfter = D3D12_BARRIER_SYNC_SPLIT;
    check_enhanced(1, &group, 0, "Enhanced begin split accepted");
    texture = texture_transition(resource);
    texture.AccessBefore = D3D12_BARRIER_ACCESS_COMMON;
    check_enhanced(1, &group, 0, "Enhanced unknown access accepted");
    texture = texture_transition(resource);
    texture.LayoutBefore = D3D12_BARRIER_LAYOUT_COMMON;
    check_enhanced(1, &group, 0, "Enhanced unknown layout accepted");
    texture = texture_transition(resource);
    texture.LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    check_enhanced(1, &group, 0, "Enhanced no layout exit accepted");
    texture = texture_transition(resource);
    texture.pResource = nullptr;
    check_enhanced(1, &group, 0, "Enhanced null resource accepted");
    texture = texture_transition(resource);
    texture.Subresources.IndexOrFirstMipLevel = 1;
    check_enhanced(1, &group, 0, "Enhanced other subresource accepted");
    texture.Subresources = {0, 0, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX};
    check_enhanced(1, &group, 1, "Enhanced index-form ignored members treated as a range");
    texture.Subresources = {0, 1, 0, 1, 0, 1};
    check_enhanced(1, &group, 1, "Enhanced exact sole range refused");
    texture.Subresources.NumMipLevels = 2;
    check_enhanced(1, &group, 0, "Enhanced multiple mips accepted");
    texture.Subresources = {0, 1, 1, 1, 0, 1};
    check_enhanced(1, &group, 0, "Enhanced nonzero array accepted");
    texture.Subresources = {0, 1, 0, 2, 0, 1};
    check_enhanced(1, &group, 0, "Enhanced multiple arrays accepted");
    texture.Subresources = {0, 1, 0, 1, 1, 1};
    check_enhanced(1, &group, 0, "Enhanced nonzero plane accepted");
    texture.Subresources = {0, 1, 0, 1, 0, 2};
    check_enhanced(1, &group, 0, "Enhanced multiple planes accepted");
    texture = texture_transition(resource);
    std::array<D3D12_TEXTURE_BARRIER, 2> textures{texture, texture};
    group.NumBarriers = 2;
    group.pTextureBarriers = textures.data();
    check_enhanced(1, &group, 1, "Earlier enhanced same-resource transition accepted");
    textures[0] = texture_transition(other_resource);
    check_enhanced(1, &group, 2, "Unrelated enhanced transition refused");
    textures[0].Flags = D3D12_TEXTURE_BARRIER_FLAG_DISCARD;
    check_enhanced(1, &group, 0, "Earlier enhanced alias/discard accepted");
    textures[0] = texture;
    textures[1] = texture_transition(other_resource);
    textures[1].Flags = D3D12_TEXTURE_BARRIER_FLAG_DISCARD;
    check_enhanced(1, &group, 1, "Later enhanced alias incorrectly refused");
    std::array<D3D12_BARRIER_GROUP, 2> groups{group, group};
    groups[0].NumBarriers = groups[1].NumBarriers = 1;
    groups[0].pTextureBarriers = groups[1].pTextureBarriers = &texture;
    check_enhanced(2, groups.data(), 1, "Earlier enhanced group change accepted");
    D3D12_GLOBAL_BARRIER global{};
    groups[0].Type = D3D12_BARRIER_TYPE_GLOBAL;
    groups[0].pGlobalBarriers = &global;
    check_enhanced(2, groups.data(), 1, "Global barrier incorrectly changes layout proof");
    groups[0].pGlobalBarriers = nullptr;
    check_enhanced(2, groups.data(), 0, "Null global array accepted");
    D3D12_BUFFER_BARRIER buffer{};
    groups[0].Type = D3D12_BARRIER_TYPE_BUFFER;
    groups[0].pBufferBarriers = &buffer;
    check_enhanced(2, groups.data(), 1, "Buffer barrier incorrectly changes texture layout");
    groups[0].pBufferBarriers = nullptr;
    check_enhanced(2, groups.data(), 0, "Null buffer array accepted");
    groups[0].Type = static_cast<D3D12_BARRIER_TYPE>(99);
    check_enhanced(2, groups.data(), 0, "Unknown group type accepted");
    group.NumBarriers = 1;
    group.pTextureBarriers = &texture;
    evidence.nested = true;
    evidence.order_count = 0;
    const auto enhanced_before = evidence.enhanced_callbacks, enhanced_original = evidence.enhanced;
    extended->Barrier(1, &group);
    evidence.nested = false;
    require(evidence.enhanced_callbacks == enhanced_before + 1 && evidence.enhanced == enhanced_original + 2 && evidence.order[0] == 5 &&
                evidence.order[1] == 4 && evidence.order[2] == 6 && evidence.order[3] == 4,
            "Enhanced nested injection order/reentry");
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
    check_enhanced(1, &group, 0, "Enhanced capture inside native pass");
    extended->EndRenderPass();
    check_enhanced(1, &group, 0, "Enhanced capture between suspended passes");
    obs::successful_reset(list, 1);

    // A changed slot disables capture globally. Calls made during that gap may
    // not have reached every hook: repairing registration must not revive a
    // prior no-pass proof, even when a Reset occurred while disabled.
    void* begin_wrapper = table[68];
    table[68] = reinterpret_cast<void*>(&original_begin);
    require(!obs::register_list(list, 1, callbacks).ready && !obs::operational(), "Changed slot did not disable capture");
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    obs::successful_reset(list, 1);
    table[68] = begin_wrapper;
    require(obs::register_list(list, 1, callbacks).ready, "Changed slot repair failed");
    check_legacy(1, &transition, 0, "Repair resurrected an unproven recording");
    obs::successful_reset(list, 1);
    list->DrawInstanced(3, 1, 0, 0);
    check_legacy(1, &transition, 1, "Fresh actual reset after repair did not restore proof");

    // Real D3D12 swaps the same object's vtable during BeginRenderPass. Each
    // observed active table needs its own original End and bounded trampoline.
    object.after_end = table.data();
    for (std::size_t n = 0; n < active_tables.size(); ++n) {
      active_tables[n] = table;
      active_tables[n][69] = reinterpret_cast<void*>(&original_active_end);
    }
    object.after_begin = active_tables[0].data();
    obs::successful_reset(list, 1);
    fail_protect_call = protect_calls + 2;
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    require(!obs::operational(), "Failed active End protection restore did not disable capture");
    const auto active_before = evidence.active_ends;
    extended->EndRenderPass();
    require(evidence.active_ends == active_before + 1 && object.table == table.data(), "Partial active End hook lost exact original");
    fail_protect_call = 0;
    require(obs::repair_protection().protection_restored, "Active End protection repair");
    require(obs::register_list(list, 1, callbacks).ready, "Active End repaired registration");
    check_legacy(1, &transition, 0, "Active End repair revived old recording");
    for (std::size_t n = 0; n < 8; ++n) {
      obs::successful_reset(list, 1);
      object.after_begin = active_tables[n].data();
      const auto before = evidence.active_ends;
      const auto queries_before = query_calls.load(), reads_before = rpm_calls.load(), protects_before = protect_calls.load();
      extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
      if (n == 0)
        require(query_calls == queries_before && rpm_calls == reads_before + 2 && protect_calls == protects_before && last_rpm_bytes == 8,
                "Known active table did not use exactly two reads without query/mutation");
      else
        require(query_calls > queries_before && protect_calls > protects_before, "New active table bypassed full registration protections");
      require(object.table == active_tables[n].data() && obs::operational(), "Observed active table was not retained");
      check_legacy(1, &transition, 0, "Swapped active table admitted an in-pass copy");
      extended->EndRenderPass();
      require(evidence.active_ends == before + 1 && object.table == table.data(),
              "Active table did not forward its distinct End exactly once");
      check_legacy(1, &transition, 1, "Actual swapped-table End did not complete pass proof");
    }
    object.after_begin = active_tables[0].data();
    const auto repeated_queries = query_calls.load(), repeated_reads = rpm_calls.load(), repeated_protects = protect_calls.load();
    for (unsigned n = 0; n < 100; ++n) {
      extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
      extended->EndRenderPass();
    }
    require(query_calls == repeated_queries && rpm_calls == repeated_reads + 200 && protect_calls == repeated_protects,
            "Repeated known-table passes reintroduced queries or writes");
    for (unsigned failed_read = 1; failed_read <= 2; ++failed_read) {
      obs::successful_reset(list, 1);
      fail_rpm_call = rpm_calls + failed_read;
      extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
      require(!obs::operational(), "Failed exact live-object/known-slot read did not disable capture");
      extended->EndRenderPass();
      fail_rpm_call = 0;
      require(obs::register_list(list, 1, callbacks).ready, "Failed-read repair could not register");
      check_legacy(1, &transition, 0, "Failed-read repair resurrected prior recording");
    }
    obs::successful_reset(list, 1);
    short_rpm_call = rpm_calls + 2;
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    require(!obs::operational(), "Partial known-slot read accepted");
    extended->EndRenderPass();
    short_rpm_call = 0;
    require(obs::register_list(list, 1, callbacks).ready, "Partial-read repair could not register");
    obs::successful_reset(list, 1);
    auto* saved_end = active_tables[0][69];
    active_tables[0][69] = reinterpret_cast<void*>(&original_active_end);
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    require(!obs::operational(), "Known-slot mismatch did not refuse capture");
    extended->EndRenderPass();
    active_tables[0][69] = saved_end;
    require(obs::register_list(list, 1, callbacks).ready, "Exact restored known slot could not register");
    obs::successful_reset(list, 1);
    object.after_begin = nullptr;
    const auto base_queries = query_calls.load(), base_reads = rpm_calls.load(), base_protects = protect_calls.load();
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    extended->EndRenderPass();
    require(query_calls == base_queries && rpm_calls == base_reads + 2 && protect_calls == base_protects,
            "Known base table bypassed fast-path contract");
    obs::successful_reset(list, 1);
    object.after_begin = active_tables[8].data();
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    require(!obs::operational() && active_tables[8][69] == reinterpret_cast<void*>(&original_active_end),
            "Active table capacity not enforced");
    extended->EndRenderPass();
    object.after_begin = nullptr;
    require(obs::register_list(list, 1, callbacks).ready, "Registration after bounded active-table refusal");
    check_legacy(1, &transition, 0, "Capacity failure revived old pass proof");
    obs::successful_reset(list, 1);
    list->DrawInstanced(3, 1, 0, 0);

    require(!obs::register_list(list, 2, callbacks).ready, "Unretired generation replaced");
    obs::unregister_list(list, 9);
    check_legacy(1, &transition, 1, "Stale destroy removed current generation");
    batch = {transition, legacy_transition(other_resource)};
    evidence.retire = true;
    check_legacy(2, batch.data(), 1, "Second callback crossed retired generation");
    evidence.retire = false;
    require(obs::register_list(list, 2, callbacks).ready, "Reused object cannot register");
    check_enhanced(1, &group, 0, "Reused generation inherited previous native work");
    list->DrawInstanced(3, 1, 0, 0);
    check_enhanced(1, &group, 1, "New generation missing callback");
    require(evidence.callback_generation == 2, "Stale generation delivered");
    obs::successful_reset(list, 2);
    auto non_rt = transition;
    non_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    auto raw_before = evidence.raw_legacy;
    check_legacy(1, &non_rt, 0, "COPY_DEST transition admitted as RT");
    require(evidence.raw_legacy == raw_before + 1 && evidence.raw_before == D3D12_RESOURCE_STATE_COPY_DEST &&
                evidence.raw_scope == obs::ScopeEnabled,
            "Raw transition lost before RT/prior-work filters");
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    raw_before = evidence.raw_legacy;
    check_legacy(1, &transition, 0, "Raw observer changed active-pass admission");
    require(evidence.raw_legacy == raw_before + 1 && (evidence.raw_scope & obs::ScopeActivePass) != 0,
            "Raw transition lost inside active pass");
    extended->EndRenderPass();
    obs::successful_reset(list, 2);
    evidence.order_count = 0;
    list->CopyResource(other_resource, resource);
    require(evidence.copy_resources == 1 && evidence.after_resources == 1 && evidence.copy_allowed &&
                evidence.copy_destination == other_resource && evidence.copy_source == resource && evidence.order[0] == 7 &&
                evidence.order[1] == 8,
            "CopyResource exact post-original notification");
    check_legacy(1, &transition, 1, "Original copy did not establish prior GPU work");
    evidence.nested = true;
    evidence.order_count = 0;
    list->CopyResource(other_resource, resource);
    evidence.nested = false;
    require(evidence.copy_resources == 3 && evidence.after_resources == 2 && evidence.order[0] == 7 && evidence.order[1] == 8 &&
                evidence.order[2] == 7,
            "Injected native copy recursed or wrong order");
    {
      const obs::ScopedBypass bypass;
      list->CopyResource(other_resource, resource);
    }
    require(evidence.copy_resources == 4 && evidence.after_resources == 2, "Explicit copy bypass failed");
    reinterpret_cast<ID3D12GraphicsCommandList*>(&unknown)->CopyResource(other_resource, resource);
    require(evidence.copy_resources == 5 && evidence.after_resources == 2, "Unknown copy identity notified");
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = other_resource;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0x123456789abcde00ull;
    D3D12_BOX box{2, 3, 4, 5, 6, 7};
    obs::successful_reset(list, 2);
    evidence.order_count = 0;
    list->CopyTextureRegion(&dst, 13, 17, 19, &src, &box);
    require(evidence.copy_textures == 1 && evidence.after_textures == 1 && evidence.copy_allowed && evidence.destination_location == &dst &&
                evidence.source_location == &src && evidence.copy_box == &box && evidence.copy_offsets == std::array<UINT, 3>{13, 17, 19} &&
                evidence.order[0] == 9 && evidence.order[1] == 10,
            "CopyTextureRegion altered location/box/offset or timing");
    evidence.nested = true;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    evidence.nested = false;
    require(evidence.copy_textures == 3 && evidence.after_textures == 2 && evidence.copy_box == nullptr,
            "Texture snapshot recursion/null-box forwarding");
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
    extended->EndRenderPass();
    list->CopyResource(other_resource, resource);
    require(evidence.after_resources == 3 && !evidence.copy_allowed, "Suspended copy callback permitted added GPU work");
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    require(evidence.after_textures == 3 && !evidence.copy_allowed, "Suspended texture callback permitted added GPU work");
    obs::successful_reset(list, 2);
    obs::reset_failed(list, 2);
    list->CopyResource(other_resource, resource);
    require(evidence.after_resources == 4 && !evidence.copy_allowed, "Failed-reset copy allowed capture");
    obs::successful_reset(list, 2);
    raw_before = evidence.raw_legacy;
    check_legacy(4097, metadata_barriers.data(), 0, "Metadata cap relaxed native capture cap");
    require(evidence.raw_legacy == raw_before + 4097, "Complete large legacy metadata was truncated");
    const auto large_invalidations = evidence.invalidations;
    metadata_barriers.resize(65537, transition);
    metadata_barriers.back() = legacy_transition(other_resource);
    raw_before = evidence.raw_legacy;
    check_legacy(static_cast<UINT>(metadata_barriers.size()), metadata_barriers.data(), 0,
                 "Large metadata batch relaxed native capture cap");
    require(evidence.raw_legacy == raw_before + metadata_barriers.size() && evidence.invalidations == large_invalidations &&
                (evidence.raw_scope & obs::ScopeInvalidRecording) == 0,
            "Valid large batch invalidated otherwise persistent source state");
    require(obs::statistics().maximum_legacy_batch == metadata_barriers.size(), "Largest legacy batch was not recorded");
    // The original API is still called exactly once, but an oversized span is
    // refused before inspecting any prefix (the supplied small array is safe).
    raw_before = evidence.raw_legacy;
    check_legacy(obs::maximum_legacy_metadata_barriers + 1, metadata_barriers.data(), 0, "Oversize batch forwarding changed");
    require(evidence.raw_legacy == raw_before && evidence.invalidations == large_invalidations + 1 &&
                (evidence.invalid_reasons & obs::InvalidationBarrierBatch),
            "Oversize metadata guard lost");
    auto enhanced_raw_before = evidence.raw_enhanced;
    D3D12_BARRIER_GROUP metadata_group{};
    metadata_group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    metadata_group.NumBarriers = 4096;
    metadata_group.pTextureBarriers = metadata_textures.data();
    check_enhanced(1, &metadata_group, 0, "Enhanced metadata relaxed capture cap");
    require(evidence.raw_enhanced == enhanced_raw_before + 4096, "Enhanced metadata extent incorrect");
    metadata_group.NumBarriers = 4097;
    enhanced_raw_before = evidence.raw_enhanced;
    check_enhanced(1, &metadata_group, 0, "Oversized enhanced metadata forwarded as capture");
    check_enhanced(65, metadata_groups.data(), 0, "Enhanced metadata group cap");
    require(evidence.raw_enhanced == enhanced_raw_before, "Enhanced metadata read beyond bounded total");
    obs::successful_reset(list, 2);
    auto after_draw_before = evidence.after_draws;
    auto original_draw_before = evidence.draws + evidence.indexed_draws;
    list->DrawInstanced(5, 7, 11, 13);
    require(evidence.after_draws == after_draw_before + 1 && evidence.draw_allowed &&
                evidence.draw_originals_seen == original_draw_before + 1 && evidence.callback_generation == 2,
            "Nonzero draw callback was not after the exact original");
    after_draw_before = evidence.after_draws;
    list->DrawInstanced(0, 7, 0, 0);
    list->DrawIndexedInstanced(7, 0, 0, 0, 0);
    require(evidence.after_draws == after_draw_before, "Empty draw produced a source notification");
    list->DrawIndexedInstanced(17, 19, 23, -29, 31);
    require(evidence.after_draws == after_draw_before + 1 && evidence.draw_allowed &&
                evidence.draw_arguments == std::array<UINT, 4>{17, 19, 23, 31} && evidence.vertex_offset == -29,
            "Indexed source notification changed arguments");
    after_draw_before = evidence.after_draws;
    original_draw_before = evidence.draws;
    evidence.nested_draw = true;
    list->DrawInstanced(3, 1, 0, 0);
    evidence.nested_draw = false;
    require(evidence.after_draws == after_draw_before + 1 && evidence.draws == original_draw_before + 2,
            "After-draw injected work recursed");
    after_draw_before = evidence.after_draws;
    {
      const obs::ScopedBypass bypass;
      list->DrawInstanced(3, 1, 0, 0);
    }
    reinterpret_cast<ID3D12GraphicsCommandList*>(&unknown)->DrawInstanced(3, 1, 0, 0);
    require(evidence.after_draws == after_draw_before, "Unknown/bypassed draw notified staged sources");
    auto invalid_before = evidence.invalidations;
    auto begins_before = evidence.begins;
    extended->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    require(evidence.invalidations == invalid_before + 1 && evidence.invalid_reasons == obs::InvalidationPassBegin &&
                evidence.invalid_begins_seen == begins_before,
            "Ordinary pass did not report its distinct reason before original Begin");
    list->DrawInstanced(3, 1, 0, 0);
    require(!evidence.draw_allowed && evidence.invalidations == invalid_before + 1,
            "An unrelated ordinary active-pass draw globally invalidated source models");
    extended->EndRenderPass();
    obs::successful_reset(list, 2);
    extended->BeginRenderPass(9, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    require((evidence.invalid_reasons & obs::InvalidationPassState) != 0, "Malformed pass omitted global uncertainty");
    extended->EndRenderPass();
    obs::successful_reset(list, 2);
    auto uncertain = transition;
    uncertain.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
    auto barriers_before = evidence.legacy + evidence.enhanced;
    list->ResourceBarrier(1, &uncertain);
    require((evidence.invalid_reasons & obs::InvalidationSplitBarrier) != 0 && evidence.invalid_barriers_seen == barriers_before &&
                evidence.invalid_raw_seen == evidence.raw_legacy + evidence.raw_enhanced &&
                (evidence.raw_scope & obs::ScopeInvalidRecording) == 0,
            "Split uncertainty did not invalidate after metadata and before original");
    uncertain.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    uncertain.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    uncertain.Aliasing = {nullptr, resource};
    list->ResourceBarrier(1, &uncertain);
    require((evidence.invalid_reasons & obs::InvalidationAliasOrDiscard) != 0 &&
                evidence.raw_type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING && evidence.raw_alias_before == nullptr &&
                evidence.raw_alias_after == resource,
            "Aliasing did not preserve exact wildcard metadata/invalidate source proof");
    uncertain = transition;
    uncertain.Flags = static_cast<D3D12_RESOURCE_BARRIER_FLAGS>(8);
    list->ResourceBarrier(1, &uncertain);
    require((evidence.invalid_reasons & obs::InvalidationBarrierBatch) != 0 && (evidence.raw_scope & obs::ScopeInvalidRecording) != 0,
            "Unknown transition flags were treated as source-specific split metadata");
    list->ResourceBarrier(obs::maximum_legacy_metadata_barriers + 1, metadata_barriers.data());
    require((evidence.invalid_reasons & obs::InvalidationBarrierBatch) != 0, "Truncated legacy metadata remained usable");
    auto uncertain_texture = texture;
    uncertain_texture.Flags = D3D12_TEXTURE_BARRIER_FLAG_DISCARD;
    auto uncertain_group = group;
    uncertain_group.pTextureBarriers = &uncertain_texture;
    extended->Barrier(1, &uncertain_group);
    require((evidence.invalid_reasons & obs::InvalidationAliasOrDiscard) != 0, "Enhanced discard did not invalidate source proof");
    uncertain_texture.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
    uncertain_texture.SyncAfter = D3D12_BARRIER_SYNC_SPLIT;
    extended->Barrier(1, &uncertain_group);
    require((evidence.invalid_reasons & obs::InvalidationSplitBarrier) != 0, "Enhanced split did not invalidate source proof");
    uncertain_texture.SyncAfter = D3D12_BARRIER_SYNC_ALL;
    uncertain_texture.Flags = static_cast<D3D12_TEXTURE_BARRIER_FLAGS>(2);
    extended->Barrier(1, &uncertain_group);
    require((evidence.invalid_reasons & obs::InvalidationBarrierBatch) != 0 && (evidence.raw_scope & obs::ScopeInvalidRecording) != 0,
            "Unknown texture flags were treated as a source-specific discard");
    extended->Barrier(65, metadata_groups.data());
    require((evidence.invalid_reasons & obs::InvalidationBarrierBatch) != 0, "Truncated enhanced metadata remained usable");
    obs::successful_reset(list, 2);
    invalid_before = evidence.invalidations;
    obs::invalidate_recording(list, 99);
    require(evidence.invalidations == invalid_before, "Stale explicit invalidation notified a new generation");
    obs::invalidate_recording(list, 2);
    require(evidence.invalidations == invalid_before + 1 && evidence.invalid_reasons == obs::InvalidationUnobservedWork,
            "Bundle/unobserved work was not explicitly invalidated");
    list->DrawInstanced(3, 1, 0, 0);
    require(!evidence.draw_allowed, "Invalidated recording captured after a later draw");
    evidence.reenter_invalidation = true;
    obs::reset_failed(list, 2);
    evidence.reenter_invalidation = false;
    require(evidence.invalidation_reentered && evidence.invalid_reasons == obs::InvalidationResetFailed,
            "Invalidation callback ran under observer locks");
    obs::successful_reset(list, 2);
    after_draw_before = evidence.after_draws;
    evidence.retire_draw = true;
    list->DrawInstanced(3, 1, 0, 0);
    evidence.retire_draw = false;
    require(evidence.after_draws == after_draw_before, "After-draw callback crossed a destroyed generation");
    require(obs::register_list(list, 2, callbacks).ready, "Draw-retired object could not re-register");
    const auto old_after = evidence.after_resources;
    evidence.retire_copy_original = true;
    list->CopyResource(other_resource, resource);
    evidence.retire_copy_original = false;
    require(evidence.after_resources == old_after, "Post-copy callback crossed destroyed generation");
    require(obs::register_list(list, 3, callbacks).ready, "Copy-retired address cannot register new generation");
    const auto scope_begin_before = metadata_begins, scope_end_before = metadata_ends, raw_scope_before = evidence.raw_legacy;
    std::array<D3D12_RESOURCE_BARRIER, 3> retire_batch{transition, transition, transition};
    retire_metadata = true;
    list->ResourceBarrier(static_cast<UINT>(retire_batch.size()), retire_batch.data());
    retire_metadata = false;
    require(metadata_begins == scope_begin_before + 1 && metadata_ends == scope_end_before + 1 && !metadata_depth &&
                evidence.raw_legacy == raw_scope_before + 1,
            "Metadata scope failed to unwind after per-item identity retirement");
    require(obs::register_list(list, 3, callbacks).ready, "Metadata-retired generation cannot re-register");
    const auto begin_retire_raw = evidence.raw_legacy, begin_retire_scopes = metadata_begins;
    retire_metadata_begin = true;
    list->ResourceBarrier(static_cast<UINT>(retire_batch.size()), retire_batch.data());
    retire_metadata_begin = false;
    require(evidence.raw_legacy == begin_retire_raw && metadata_begins == begin_retire_scopes + 1 && metadata_begins == metadata_ends,
            "Begin-side identity retirement escaped RAII or delivered stale metadata");
    require(obs::register_list(list, 3, callbacks).ready, "Begin-retired generation cannot re-register");
    require(metadata_begins == metadata_ends && !metadata_depth && !metadata_errors,
            "Legacy/enhanced metadata escaped balanced unlocked batch scopes");
    std::vector<Fake> more(8192, Fake{table.data()});
    for (std::size_t n = 0; n < 8191; ++n)
      require(obs::register_list(reinterpret_cast<ID3D12GraphicsCommandList*>(&more[n]), 3, callbacks).ready, "Registry early refusal");
    require(!obs::register_list(reinterpret_cast<ID3D12GraphicsCommandList*>(&more.back()), 3, callbacks).ready,
            "Registry cap not enforced");
    for (std::size_t n = 0; n < 8191; ++n)
      obs::unregister_list(reinterpret_cast<ID3D12GraphicsCommandList*>(&more[n]), 3);
    const auto counters = obs::statistics();
    require(counters.legacy_candidates == evidence.legacy_callbacks && counters.enhanced_candidates == evidence.enhanced_callbacks &&
                counters.batch_refusals > 0 && counters.pass_refusals > 0 && counters.metadata_truncated_calls >= 3 &&
                counters.copy_resource_calls == evidence.after_resources && counters.copy_texture_calls == evidence.after_textures,
            "Diagnostic counters inconsistent");
    fail_protect_call = protect_calls + 2;
    result = obs::remove();
    require(!result.protection_restored && !obs::operational(), "Removal restore failure lost");
    check_enhanced(1, &group, 0, "Partially removed observer still invokes callbacks");
    fail_protect_call = 0;
    require(obs::repair_protection().protection_restored, "Removal protection repair");
    result = obs::remove();
    require(std::string(result.status) == "removed" && result.protection_restored && !obs::operational(), "Removal completion");
    require(table[26] == reinterpret_cast<void*>(&original_legacy) && table[68] == reinterpret_cast<void*>(&original_begin) &&
                table[69] == reinterpret_cast<void*>(&original_end) && table[80] == reinterpret_cast<void*>(&original_enhanced) &&
                table[12] == reinterpret_cast<void*>(&original_draw) && table[13] == reinterpret_cast<void*>(&original_draw_indexed) &&
                table[16] == reinterpret_cast<void*>(&original_copy_texture) &&
                table[17] == reinterpret_cast<void*>(&original_copy_resource),
            "Native original slots not restored");
    for (std::size_t n = 0; n < 8; ++n)
      require(active_tables[n][69] == reinterpret_cast<void*>(&original_active_end), "Observed active End original not restored");
    require(!obs::register_list(list, 3, callbacks).ready, "Removed observer reenabled");
    std::printf("{\"passed\":true,\"checks\":%llu,\"retainedCommandLists\":0,\"nativeSlots\":[12,13,16,17,26,68,69,80]}\n",
                static_cast<unsigned long long>(checks));
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
