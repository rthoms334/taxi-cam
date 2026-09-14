#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>
#include "../src/pfd_stamp_state.hpp"
using namespace taxi_camera;
namespace {
struct Call {
  unsigned slot, index, offset;
  UINT64 value;
};
std::vector<Call> calls;
std::vector<unsigned> dynamic_calls;
std::array<float, 3> replayed_bias{};
D3D12_INDEX_BUFFER_STRIP_CUT_VALUE replayed_cut{};
void STDMETHODCALLTYPE pipeline(void*, ID3D12PipelineState*) {
  dynamic_calls.push_back(25);
}
void STDMETHODCALLTYPE bias(void*, float a, float b, float c) {
  dynamic_calls.push_back(82);
  replayed_bias = {a, b, c};
}
void STDMETHODCALLTYPE cut(void*, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE v) {
  dynamic_calls.push_back(83);
  replayed_cut = v;
}
void STDMETHODCALLTYPE root(void*, ID3D12RootSignature*) {}
void STDMETHODCALLTYPE constant(void*, UINT index, UINT value, UINT offset) {
  calls.push_back({34, index, offset, value});
}
void STDMETHODCALLTYPE table(void*, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE value) {
  calls.push_back({32, index, 0, value.ptr});
}
void STDMETHODCALLTYPE cbv(void*, UINT index, UINT64 value) {
  calls.push_back({38, index, 0, value});
}
void STDMETHODCALLTYPE srv(void*, UINT index, UINT64 value) {
  calls.push_back({40, index, 0, value});
}
void STDMETHODCALLTYPE uav(void*, UINT index, UINT64 value) {
  calls.push_back({42, index, 0, value});
}
void STDMETHODCALLTYPE topology(void*, D3D12_PRIMITIVE_TOPOLOGY) {}
void STDMETHODCALLTYPE viewports(void*, UINT, const D3D12_VIEWPORT*) {}
void STDMETHODCALLTYPE scissors(void*, UINT, const D3D12_RECT*) {}
void check(bool v, const char* s) {
  if (!v)
    throw std::runtime_error(s);
}
PfdGraphicsState state(bool observed = true) {
  PfdGraphicsState s;
  s.reset(1, observed);
  s.bind_pipeline(reinterpret_cast<ID3D12PipelineState*>(1));
  s.bind_observed_root(reinterpret_cast<ID3D12RootSignature*>(2), 2);
  s.topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VIEWPORT v{0, 0, 768, 1024, 0, 1};
  s.viewports(0, 1, &v);
  D3D12_RECT r{0, 0, 768, 1024};
  s.scissors(0, 1, &r);
  return s;
}
}  // namespace
int main() {
  try {
    std::array<void*, 84> vtable{};
    vtable[82] = reinterpret_cast<void*>(&bias);
    vtable[83] = reinterpret_cast<void*>(&cut);
    vtable[25] = reinterpret_cast<void*>(&pipeline);
    vtable[30] = reinterpret_cast<void*>(&root);
    vtable[34] = reinterpret_cast<void*>(&constant);
    vtable[32] = reinterpret_cast<void*>(&table);
    vtable[38] = reinterpret_cast<void*>(&cbv);
    vtable[40] = reinterpret_cast<void*>(&srv);
    vtable[42] = reinterpret_cast<void*>(&uav);
    vtable[20] = reinterpret_cast<void*>(&topology);
    vtable[21] = reinterpret_cast<void*>(&viewports);
    vtable[22] = reinterpret_cast<void*>(&scissors);
    auto* pointer = vtable.data();
    auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(&pointer);
    auto s = state();
    UINT words[]{7, 8};
    s.constants(5, 2, 2, words);
    s.bind_observed_root(s.root(), s.layout_generation());  // Redundant binding retains partial words.
    const UINT update = 9;
    s.constants(5, 3, 1, &update);
    s.table(1, 0x1234);
    s.descriptor(2, PfdRootKind::cbv, 0x2000);
    s.descriptor(3, PfdRootKind::srv, 0x3000);
    s.descriptor(4, PfdRootKind::uav, 0x4000);
    check(s.complete(), "Observed partial arguments must replay without fabricated layout");
    s.restore(list);
    check(calls.size() == 6, "Exactly the observed arguments must replay");
    check(calls[0].index == 5 && calls[0].offset == 2 && calls[0].value == 7, "Partial constant first offset");
    check(calls[1].offset == 3 && calls[1].value == 9, "Partial constant update");
    check(calls[2].slot == 32 && calls[2].value == 0x1234, "Descriptor table");
    check(calls[3].slot == 38 && calls[4].slot == 40 && calls[5].slot == 42, "Root descriptor kinds");
    ID3D12DescriptorHeap* heaps[]{reinterpret_cast<ID3D12DescriptorHeap*>(3)};
    s.descriptor_heaps(1, heaps);
    calls.clear();
    s.restore(list);
    check(calls.size() == 5, "Heap change clears only table argument");
    s.table(1, 0x5678);
    s.descriptor_heaps(1, heaps);
    calls.clear();
    s.restore(list);
    check(calls.size() == 6 && calls[2].value == 0x5678, "Same heap preserves table");
    s.bind_observed_root(reinterpret_cast<ID3D12RootSignature*>(4), 3);
    calls.clear();
    s.restore(list);
    check(calls.empty(), "Different signature clears all observed arguments");
    check(!state(false).complete(), "Mid-recording discovery cannot establish initial state");
    auto invalid = state();
    invalid.table(64, 1);
    check(!invalid.complete(), "Root index bounds");
    invalid = state();
    invalid.constants(0, 63, 2, words);
    check(!invalid.complete(), "Root word bounds");
    invalid = state();
    invalid.table(0, 1);
    invalid.descriptor(0, PfdRootKind::srv, 1);
    check(!invalid.complete(), "Conflicting root kind");
    invalid = state();
    invalid.invalidate();
    invalid.bind_observed_root(reinterpret_cast<ID3D12RootSignature*>(4), 3);
    check(!invalid.complete(), "Signature change must not clear unsupported-work invalidation");
    auto dynamic_state = state();
    auto* native9 = reinterpret_cast<d3d12_extended::CommandList9*>(list);
    dynamic_state.depth_bias(native9, -4.f, .5f, 2.f);
    dynamic_state.strip_cut(native9, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
    check(dynamic_state.has_depth_bias() && dynamic_state.has_strip_cut() && dynamic_state.can_restore(list), "Dynamic snapshot ready");
    dynamic_calls.clear();
    dynamic_state.restore(list);
    check((dynamic_calls == std::vector<unsigned>{25, 82, 83}), "Dynamic overrides replay after PSO reset");
    check((replayed_bias == std::array<float, 3>{-4.f, .5f, 2.f}) && replayed_cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF,
          "Exact dynamic arguments");
    check(!dynamic_state.can_restore(reinterpret_cast<ID3D12GraphicsCommandList*>(UINT_PTR{8})), "Wrong native list refused before stamp");
    dynamic_state.bind_pipeline(reinterpret_cast<ID3D12PipelineState*>(1));
    check(!dynamic_state.has_depth_bias() && !dynamic_state.has_strip_cut(), "Same app PSO assignment clears dynamic overrides");
    dynamic_calls.clear();
    dynamic_state.restore(list);
    check((dynamic_calls == std::vector<unsigned>{25}), "No fabricated default override");
    dynamic_state.depth_bias(native9, std::numeric_limits<float>::quiet_NaN(), 0, 0);
    check(!dynamic_state.complete(), "Nonfinite depth bias refuses restoration");
    dynamic_state = state();
    dynamic_state.strip_cut(native9, static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(3));
    check(!dynamic_state.complete(), "Invalid strip cut refuses restoration");
    dynamic_state = state();
    dynamic_state.depth_bias(native9, -4, 0, 0);
    dynamic_state.strip_cut(reinterpret_cast<d3d12_extended::CommandList9*>(UINT_PTR{8}), D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED);
    check(!dynamic_state.complete(), "Mixed native identity refuses restoration");
    dynamic_state.reset(2, true);
    check(!dynamic_state.has_depth_bias() && !dynamic_state.has_strip_cut(), "Actual Reset clears dynamic overrides");
    std::puts("PASS dynamic PSO replay: exact bias/cut after PSO, same-PSO reset, native identity and invalid input guards.");
    std::puts(
        "PASS native root replay: pre-existing layouts, sparse partial constants, all descriptor kinds, redundant bindings, heap changes, "
        "Reset requirement and bounds.");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL root replay: %s\n", e.what());
    return 1;
  }
}
