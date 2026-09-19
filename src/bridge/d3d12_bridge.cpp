#include "d3d12_bridge.hpp"
#include <dxgi1_6.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include "../graphics/calibration_d3d12.hpp"
#include "../graphics/d3d12_command_list9.hpp"
#include "../graphics/metadata_batch_cache.hpp"
#include "../graphics/native_device_identity.hpp"
#include "../graphics/pfd_copy_proof.hpp"
#include "../graphics/pfd_submission_proof.hpp"
#include "../graphics/query_scope.hpp"
#include "../graphics/taxi_button_routes.hpp"
#include "../graphics/write_budget.hpp"
#include "../hooks/render_boundary_observer.hpp"
#include "native_hooks.hpp"
#include "root_layout.hpp"

namespace taxi_camera::standalone {
namespace {
namespace boundary = engine_hook::render_boundary;
namespace queue_hook = engine_hook::queue_submit;
namespace runtime = scene_runtime;
constexpr GUID LifetimeId{0x986208c6, 0x68b9, 0x450d, {0x86, 0x0c, 0xac, 0x85, 0x60, 0x07, 0x88, 0x01}};
struct Metadata {
  std::atomic<bool> alive{true};
  std::uint64_t id{};
  virtual void retire() noexcept { alive.store(false); }
  virtual ~Metadata() = default;
};
struct Resource : Metadata {
  ID3D12Resource* native{};
  std::uint64_t key{};
  D3D12_RESOURCE_DESC desc{};
  std::atomic<std::uint64_t> draws{};
  std::atomic<std::uint64_t> submission_activity{};
  std::array<UINT, 4> typed_rtv_refs{};
  void retire() noexcept override {
    alive.store(false, std::memory_order_release);
    runtime::manager().unregister_source_candidate(key, native, id);
    scene_handoff().unregister_resource(key, reinterpret_cast<std::uint64_t>(native));
  }
};
struct Root : Metadata {
  PfdRootLayout layout;
};
struct View {
  std::shared_ptr<Resource> resource;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  UINT mip{};
  SIZE_T rtv{};
  // A late bind identifies the resource, not the opaque application's format.
  // Output to such a view uses an explicit bridge-owned typed descriptor.
  bool recovered{};
};
void clear_live_bind(ID3D12GraphicsCommandList* native) noexcept;
struct List : Metadata {
  ID3D12GraphicsCommandList* native{};
  std::atomic<std::uint64_t> recording{1};
  std::atomic<std::uint64_t> observation_epoch{};
  PfdGraphicsState graphics;
  std::array<View, 8> targets{};
  std::array<View, 2> pending_pfds{};
  std::array<bool, 2> pending_rt{};
  QueryScope queries;
  PfdCopyProof copy_proof;
  PfdSubmissionProof submission_proof;
  std::atomic<std::uint64_t> closed_recording{};
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> raw_rtvs{};
  D3D12_CPU_DESCRIPTOR_HANDLE raw_dsv{};
  UINT raw_rtv_count{};
  bool raw_om_known = false, raw_has_dsv = false;
  ID3D12DescriptorHeap* snapshot_rtvs{};
  ID3D12DescriptorHeap* snapshot_dsvs{};
  ~List() override {
    if (snapshot_rtvs)
      snapshot_rtvs->Release();
    if (snapshot_dsvs)
      snapshot_dsvs->Release();
  }
  UINT count{};
  DXGI_FORMAT depth = DXGI_FORMAT_UNKNOWN;
  bool depth_known = true;
  bool ready = false;
  bool closing = false;
  bool pfd_dirty = false, pfd_transition = false;
  void retire() noexcept override {
    alive.store(false, std::memory_order_release);
    clear_live_bind(native);
    boundary::unregister_list(native, id);
    runtime::manager().destroy_command_list(native, id);
  }
};
class Lifetime final : public IUnknown {
 public:
  explicit Lifetime(std::shared_ptr<Metadata> p) : value_(std::move(p)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown))
      return E_NOINTERFACE;
    *out = static_cast<IUnknown*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto n = --references_;
    if (!n) {
      value_->retire();
      delete this;
    }
    return n;
  }

 private:
  std::atomic<ULONG> references_{1};
  std::shared_ptr<Metadata> value_;
};
struct Registry {
  std::recursive_mutex mutex;
  // Only demand transitions and publication of a real Reset/new-list proof
  // take this lock. Ordinary setters and metadata callbacks never take it.
  std::mutex observation_mutex;
  std::atomic<std::uint64_t> observation_epoch{1};  // Odd: observing; even: idle.
  std::atomic<std::uint64_t> session_recording_floor{};
  std::atomic<bool> diagnostics_enabled{};
  std::atomic<std::uint64_t> list_lookup_calls{}, list_cache_hits{}, list_registry_lookups{};
  std::atomic<std::uint64_t> idle_state_bypasses{}, idle_callback_bypasses{}, observation_invalidations{};
  ID3D12Device* device{};
  std::uint64_t key{}, next_id = 0;
  UINT rtv_stride{}, dsv_stride{};
  std::atomic<bool> ready{};
  std::atomic<std::uint64_t> failures{}, draws{}, clear_states{};
  const char* error = "not_started";
  std::unordered_map<ID3D12Resource*, std::shared_ptr<Resource>> resources;
  std::unordered_map<ID3D12RootSignature*, std::shared_ptr<Root>> roots;
  std::unordered_map<ID3D12GraphicsCommandList*, std::shared_ptr<List>> lists;
  std::unordered_map<SIZE_T, View> rtvs;
  std::unordered_map<SIZE_T, DXGI_FORMAT> dsvs;
  TaxiButtonRoutes routes;
  PfdTargetDetector detector;
  // A dropped resource can make a truncated allocation group look complete.
  // Keep this false for the device lifetime once observation loses coverage.
  std::atomic<bool> pfd_inventory_complete{true};
  const profiles::AircraftProfile* profile = &profiles::A380;
  unsigned active_mask{}, calibration_mask{};
  std::array<std::shared_ptr<Resource>, 2> selected_resources{};
  std::array<std::atomic<ID3D12Resource*>, 2> selected_native{};
  std::array<std::atomic<std::uint64_t>, 2> selected_ids{};
  std::atomic<unsigned> selected_mask{};
  std::atomic<std::uint64_t> queue_patch_generation{1};
  std::array<std::uint64_t, 2> queue_patch_targets{};
  unsigned queue_patch_camera{}, queue_patch_calibration{};
  std::uint32_t queue_patch_profile{};
  std::atomic<std::uint64_t> queue_patch_plans{};
  std::array<std::atomic<std::uint64_t>, static_cast<unsigned>(DisplaySubmissionOutcome::count)> queue_outcomes{};
  std::array<std::atomic<std::uint64_t>, static_cast<unsigned>(PfdSubmissionProof::Refusal::count)> queue_proof_refusals{};
  std::atomic<unsigned> queue_last_proof_flags{};
  std::array<std::atomic<std::uint64_t>, 81> queue_prefix_blockers{};
  std::atomic<std::uint64_t> selected_draws{}, selected_rt_metadata{}, selected_rt_callbacks{}, selected_pending_matches{};
  std::atomic<std::uint64_t> selected_view_resolved{}, selected_view_rejected{}, copy_attempts{}, copy_rejected{};
  std::atomic<const char*> copy_error{"not_attempted"};
  std::array<std::atomic<std::uint64_t>, 32> selected_exit_scopes{};
  std::atomic<std::uint64_t> calibration_clears{};
  std::atomic<std::uint64_t> selected_exit_base{}, selected_exit_nonbase{}, selected_exit_split{};
  std::atomic<std::uint64_t> fallback_attempts{}, fallback_stamps{}, fallback_query_refused{}, fallback_state_refused{};
  std::atomic<std::uint64_t> recording_end_draws{}, shader_deferred{}, close_forward_refused{};
  bool close_forward_verified = false;
  std::atomic<std::uint64_t> preferred_copy_attempts{}, preferred_copy_stamps{}, preferred_copy_no_proof{};
  std::atomic<const char*> preferred_copy_reason{"not_attempted"};
  std::atomic<std::uint64_t> dynamic_depth_bias_calls{}, dynamic_strip_cut_calls{};
  std::atomic<std::uint64_t> sample_position_calls{};
  WriteBudget calibration_budget;
  // Late-attach learn-on-use. After this is false, barrier/copy/OM extra work is
  // one relaxed load. Never allocate or lock unless this flag is still set.
  std::atomic<bool> live_backfill{};
  std::uint64_t backfill_started_ms{};
  std::uint64_t backfill_inventory_ms{};
  bool backfill_full_window{};
  struct SeenResources {
    static constexpr unsigned Capacity = 1024;
    static constexpr unsigned Probes = 8;
    std::array<std::atomic<ID3D12Resource*>, Capacity> slots{};
    static unsigned hash(ID3D12Resource* p) noexcept {
      auto x = reinterpret_cast<std::uintptr_t>(p);
      x ^= x >> 30;
      x *= static_cast<std::uintptr_t>(0xbf58476d1ce4e5b9ULL);
      return static_cast<unsigned>(x) & (Capacity - 1);
    }
    bool contains(ID3D12Resource* p) const noexcept {
      auto i = hash(p);
      for (unsigned n = 0; n < Probes; ++n) {
        const auto value = slots[i].load(std::memory_order_relaxed);
        if (value == p)
          return true;
        if (!value)
          return false;
        i = (i + 1) & (Capacity - 1);
      }
      return false;
    }
    void remember(ID3D12Resource* p) noexcept {
      auto i = hash(p);
      for (unsigned n = 0; n < Probes; ++n) {
        ID3D12Resource* expected = nullptr;
        if (slots[i].compare_exchange_strong(expected, p, std::memory_order_relaxed) || expected == p)
          return;
        i = (i + 1) & (Capacity - 1);
      }
    }
  } seen_resources;
  struct LiveBindHint {
    // One slot per in-flight list. A global hint was wiped by Reset/ClearState
    // on any other list, so MSFS association never survived cockpit recording.
    static constexpr unsigned Slots = 64;
    std::mutex mutex;
    std::array<std::atomic<ID3D12GraphicsCommandList*>, Slots> lists{};
    std::array<std::atomic<ID3D12Resource*>, Slots> resources{};
    std::array<std::atomic<unsigned>, Slots> entries{};
    std::array<std::uint64_t, Slots> generations{};
    void note(ID3D12GraphicsCommandList* native, ID3D12Resource* p, std::uint64_t generation = 0) noexcept {
      if (!native)
        return;
      const std::lock_guard lock(mutex);
      int empty = -1;
      for (unsigned i = 0; i < Slots; ++i) {
        const auto owner = lists[i].load(std::memory_order_relaxed);
        if (owner == native) {
          if (entries[i].load(std::memory_order_relaxed) && resources[i].load(std::memory_order_relaxed) == p &&
              generations[i] == generation)
            return;
          resources[i].store(p, std::memory_order_relaxed);
          generations[i] = generation;
          entries[i].store(p && !entries[i].load(std::memory_order_relaxed) ? 1u : 2u, std::memory_order_relaxed);
          return;
        }
        if (!owner && empty < 0)
          empty = static_cast<int>(i);
      }
      if (empty < 0)
        return;
      const auto i = static_cast<unsigned>(empty);
      ID3D12GraphicsCommandList* expected = nullptr;
      if (!lists[i].compare_exchange_strong(expected, native, std::memory_order_relaxed) && expected != native)
        return;
      resources[i].store(p, std::memory_order_relaxed);
      generations[i] = generation;
      entries[i].store(p ? 1u : 2u, std::memory_order_relaxed);
    }
    ID3D12Resource* take(ID3D12GraphicsCommandList* native, std::uint64_t* generation = nullptr) noexcept {
      if (!native)
        return nullptr;
      const std::lock_guard lock(mutex);
      for (unsigned i = 0; i < Slots; ++i) {
        if (lists[i].load(std::memory_order_relaxed) != native)
          continue;
        auto* result = entries[i].load(std::memory_order_relaxed) == 1 ? resources[i].load(std::memory_order_relaxed) : nullptr;
        if (generation)
          *generation = result ? generations[i] : 0;
        entries[i].store(0, std::memory_order_relaxed);
        resources[i].store(nullptr, std::memory_order_relaxed);
        lists[i].store(nullptr, std::memory_order_relaxed);
        return result;
      }
      return nullptr;
    }
    void clear(ID3D12GraphicsCommandList* native) noexcept {
      if (!native)
        return;
      const std::lock_guard lock(mutex);
      for (unsigned i = 0; i < Slots; ++i)
        if (lists[i].load(std::memory_order_relaxed) == native) {
          entries[i].store(0, std::memory_order_relaxed);
          resources[i].store(nullptr, std::memory_order_relaxed);
          lists[i].store(nullptr, std::memory_order_relaxed);
        }
    }
    void clear_all() noexcept {
      const std::lock_guard lock(mutex);
      for (unsigned i = 0; i < Slots; ++i) {
        entries[i].store(0, std::memory_order_relaxed);
        resources[i].store(nullptr, std::memory_order_relaxed);
        lists[i].store(nullptr, std::memory_order_relaxed);
      }
    }
  } live_bind;
  std::array<std::atomic<ID3D12Resource*>, 64> backfill_displays{};
  // Monotonic negative filter: retirement never clears shared bits. False
  // positives take the exact registry lookup; a tracked display cannot vanish.
  std::array<std::atomic<std::uint64_t>, 16> display_filter{};
};
Registry& registry() {
  static auto* r = new Registry;
  return *r;
}
void clear_live_bind(ID3D12GraphicsCommandList* native) noexcept {
  registry().live_bind.clear(native);
}
bool observation_enabled() noexcept {
  return (registry().observation_epoch.load(std::memory_order_acquire) & 1u) != 0;
}
bool recording_observed(const List& list) noexcept {
  const auto epoch = registry().observation_epoch.load(std::memory_order_acquire);
  return (epoch & 1u) && list.observation_epoch.load(std::memory_order_acquire) == epoch;
}
bool idle_callback() noexcept {
  auto& r = registry();
  if (observation_enabled())
    return false;
  if (r.diagnostics_enabled.load(std::memory_order_relaxed))
    r.idle_callback_bypasses.fetch_add(1, std::memory_order_relaxed);
  return true;
}
constexpr std::array<DXGI_FORMAT, 4> TypedFormats{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                  DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
DXGI_FORMAT output_format(const View& view) noexcept {
  if (!view.resource || !view.resource->alive || view.mip)
    return DXGI_FORMAT_UNKNOWN;
  if (!view.recovered)
    return std::find(TypedFormats.begin(), TypedFormats.end(), view.format) != TypedFormats.end() ? view.format : DXGI_FORMAT_UNKNOWN;
  // This is the bridge's explicit output encoding. It is never entered in the
  // application's typed-RTV evidence, and never used with its opaque descriptor.
  const auto format = view.resource->desc.Format;
  if (format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  if (format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  return std::find(TypedFormats.begin(), TypedFormats.end(), format) != TypedFormats.end() ? format : DXGI_FORMAT_UNKNOWN;
}
void account_view(const View& view, bool add) noexcept {
  if (!view.resource || view.mip || view.recovered)
    return;
  for (unsigned i = 0; i < TypedFormats.size(); ++i)
    if (view.format == TypedFormats[i]) {
      auto& count = view.resource->typed_rtv_refs[i];
      if (add)
        ++count;
      else if (count)
        --count;
    }
}
// Every observed RTV replacement/erase updates the same lifetime-qualified
// numeric proof. No descriptor or native resource memory is read here.
void replace_view(Registry& r, SIZE_T handle, const View& supplied) {
  const View view = supplied;
  const auto old = r.rtvs.find(handle);
  if (old != r.rtvs.end())
    account_view(old->second, false);
  if (view.resource && (old != r.rtvs.end() || r.rtvs.size() < 65536)) {
    r.rtvs[handle] = view;
    account_view(view, true);
  } else if (old != r.rtvs.end())
    r.rtvs.erase(old);
}
void clear_views(Registry& r) noexcept {
  for (const auto& [handle, view] : r.rtvs) {
    (void)handle;
    account_view(view, false);
  }
  r.rtvs.clear();
}
void refresh_selected(Registry& r) {
  for (unsigned side = 0; side < 2; ++side) {
    auto& selected = r.selected_resources[side];
    const auto id = r.routes.targets[side];
    if (!selected || !selected->alive || selected->id != id) {
      selected.reset();
      if (id)
        for (const auto& [native, item] : r.resources) {
          (void)native;
          if (item->alive && item->id == id) {
            selected = item;
            break;
          }
        }
    }
    r.selected_native[side].store(selected ? selected->native : nullptr, std::memory_order_relaxed);
    r.selected_ids[side].store(selected ? selected->id : 0, std::memory_order_relaxed);
  }
  r.selected_mask.store(r.active_mask | r.calibration_mask, std::memory_order_relaxed);
  const std::array<std::uint64_t, 2> selected{r.selected_ids[0].load(), r.selected_ids[1].load()};
  if (selected != r.queue_patch_targets || r.queue_patch_camera != r.active_mask || r.queue_patch_calibration != r.calibration_mask ||
      r.queue_patch_profile != r.profile->id) {
    r.queue_patch_targets = selected;
    r.queue_patch_camera = r.active_mask;
    r.queue_patch_calibration = r.calibration_mask;
    r.queue_patch_profile = r.profile->id;
    r.queue_patch_generation.fetch_add(1, std::memory_order_release);
  }
}
bool maybe_selected(ID3D12Resource* native) noexcept {
  auto& r = registry();
  const auto mask = r.selected_mask.load(std::memory_order_relaxed);
  return native && (((mask & 1) && r.selected_native[0].load(std::memory_order_relaxed) == native) ||
                    ((mask & 2) && r.selected_native[1].load(std::memory_order_relaxed) == native));
}
void selected_metadata(ID3D12Resource* native, std::uint32_t scope, bool base, bool split) noexcept {
  if (!maybe_selected(native))
    return;
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  refresh_selected(r);
  for (unsigned side = 0; side < 2; ++side)
    if (((r.active_mask | r.calibration_mask) & (1u << side)) && r.selected_resources[side] && r.selected_resources[side]->alive &&
        r.selected_resources[side]->native == native) {
      ++r.selected_rt_metadata;
      ++r.selected_exit_scopes[scope & 31u];
      ++(base ? r.selected_exit_base : r.selected_exit_nonbase);
      if (split)
        ++r.selected_exit_split;
      return;
    }
}
UINT selected_legacy_targets(void*, ID3D12GraphicsCommandList* native, std::uint64_t id, ID3D12Resource** targets, UINT capacity) noexcept;
bool attach(ID3D12Object* object, const std::shared_ptr<Metadata>& metadata) {
  auto* lifetime = new Lifetime(metadata);
  const auto hr = object->SetPrivateDataInterface(LifetimeId, lifetime);
  lifetime->Release();
  return SUCCEEDED(hr);
}
void error(const char* value) noexcept {
  auto& r = registry();
  ++r.failures;
  const std::lock_guard lock(r.mutex);
  r.error = value;
}
template <class F>
void observe_safely(F&& action) noexcept {
  try {
    action();
  } catch (...) {
    auto& r = registry();
    r.ready = false;
    const std::lock_guard lock(r.mutex);
    r.active_mask = r.calibration_mask = 0;
    error("native_observation_allocation_failed");
  }
}
bool relevant(const D3D12_RESOURCE_DESC& d) noexcept {
  bool dimensions = profiles::camera_candidate(static_cast<UINT>(d.Width), d.Height);
  for (const auto* profile : profiles::Catalog)
    dimensions |= d.Width == profile->width && d.Height == profile->height;
  return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && dimensions && d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1 &&
         (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
}
bool display_candidate_desc(const D3D12_RESOURCE_DESC& d) noexcept {
  if (!relevant(d))
    return false;
  for (const auto* profile : profiles::Catalog)
    if (profiles::matches_display(*profile, static_cast<UINT>(d.Width), d.Height, d.MipLevels, static_cast<UINT>(d.Format)))
      return true;
  return false;
}
bool same_device(ID3D12Device* device) noexcept {
  IUnknown *a{}, *b{};
  auto* expected = registry().device;
  if (!device || !expected)
    return false;
  // The registry retains this exact interface for its entire lifetime. Equal
  // pointers already prove identity; alternate interfaces still use IUnknown.
  if (device == expected)
    return true;
  const bool equal =
      SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&a))) && SUCCEEDED(expected->QueryInterface(IID_PPV_ARGS(&b))) && a && a == b;
  if (a)
    a->Release();
  if (b)
    b->Release();
  return equal;
}
constexpr std::uint64_t LiveBackfillAdmitMs = 3000;
constexpr std::uint64_t LiveBackfillAssociateMs = 10000;
static_assert(LiveBackfillAdmitMs < LiveBackfillAssociateMs);
std::array<std::uint64_t, 2> dominant_activity_pair(const profiles::AircraftProfile& profile,
                                                    const std::vector<PfdTargetObservation>& inventory) noexcept {
  if (inventory.size() != 2 || !inventory[0].id || !inventory[1].id || inventory[0].id == inventory[1].id)
    return {};
  // With opaque pre-existing views, A350's first two surfaces can be unrelated
  // five-mip outputs or an incomplete powered EFIS group. Let the detector
  // establish its full submission-only group and activity windows first.
  if ((profile.id == profiles::A359.id || profile.id == profiles::A35K.id) && !inventory[0].draws && !inventory[1].draws)
    return {};
  const auto high = std::max(inventory[0].id, inventory[1].id);
  const auto low = std::min(inventory[0].id, inventory[1].id);
  return profile.higher_id_left ? std::array<std::uint64_t, 2>{high, low} : std::array<std::uint64_t, 2>{low, high};
}
// Control-thread only. Same last / third-last mapping as the allocation-group
// detector. Preserve the complete-group automatic selection used by the
// aircraft profile, including late discovery. A partial list is incomplete.
std::array<std::uint64_t, 2> allocation_group_pair(const profiles::AircraftProfile& profile,
                                                   const std::vector<PfdTargetObservation>& inventory) noexcept {
  if (profile.pfd_detection != profiles::PfdDetectionPolicy::ini_a380_allocation_group)
    return {};
  std::array<std::uint64_t, 8> ids{};
  std::size_t n = 0;
  for (const auto& item : inventory) {
    if (!item.id || item.format != 27)
      continue;
    if (n == ids.size())
      return {};
    ids[n++] = item.id;
  }
  if (n != ids.size())
    return {};
  std::sort(ids.begin(), ids.end());
  for (std::size_t i = 1; i < ids.size(); ++i)
    if (ids[i] == ids[i - 1])
      return {};
  return {ids[7], ids[5]};
}
bool display_item(const Registry& r, const Resource& item) noexcept {
  return item.alive && profiles::matches_display(*r.profile, static_cast<UINT>(item.desc.Width), item.desc.Height, item.desc.MipLevels,
                                                 static_cast<UINT>(item.desc.Format));
}
unsigned live_display_resources(const Registry& r) noexcept {
  unsigned n = 0;
  for (const auto& [_, item] : r.resources) {
    (void)_;
    if (item && display_item(r, *item))
      ++n;
  }
  return n;
}
bool display_set_ready(const Registry& r, unsigned need, DXGI_FORMAT format, UINT mips) noexcept {
  unsigned n = 0;
  for (const auto& [native, item] : r.resources) {
    (void)native;
    if (!item || !display_item(r, *item))
      continue;
    if (item->desc.MipLevels == mips && item->desc.Format == format && ++n == need)
      return true;
  }
  return false;
}
// The detector can assign only this set. A generic pair, or another mip/format
// that still matches the profile size, must not end discovery.
bool backfill_resources_ready(const Registry& r) noexcept {
  if (!r.profile)
    return false;
  if (r.profile->id == profiles::A359.id || r.profile->id == profiles::A35K.id)
    return display_set_ready(r, 3, DXGI_FORMAT_R8G8B8A8_TYPELESS, 1);
  if (r.profile->pfd_detection == profiles::PfdDetectionPolicy::ini_a380_allocation_group)
    return display_set_ready(r, 8, DXGI_FORMAT_R8G8B8A8_TYPELESS, 1);
  if (r.profile->id == profiles::A380.id)
    return display_set_ready(r, 2, static_cast<DXGI_FORMAT>(r.profile->formats[0]), r.profile->mips);
  return false;
}
unsigned live_display_rtvs(const Registry& r) noexcept {
  unsigned n = 0;
  for (const auto& [native, item] : r.resources) {
    (void)native;
    if (!item || !display_item(r, *item))
      continue;
    for (const auto& [handle, view] : r.rtvs) {
      (void)handle;
      if (view.resource == item && !view.mip) {
        ++n;
        break;
      }
    }
  }
  return n;
}
void maybe_stop_live_backfill(Registry& r) noexcept {
  if (!r.live_backfill.load(std::memory_order_relaxed) || r.backfill_full_window)
    return;
  // Same set as the timed scan. A generic pair of A350 auxiliaries is not ready.
  if (backfill_resources_ready(r) && live_display_rtvs(r) == live_display_resources(r))
    r.live_backfill.store(false, std::memory_order_relaxed);
}
void remember_backfill_display(Registry& r, ID3D12Resource* native) noexcept {
  for (auto& slot : r.backfill_displays) {
    ID3D12Resource* expected = nullptr;
    if (slot.compare_exchange_strong(expected, native, std::memory_order_relaxed) || expected == native)
      return;
  }
}
bool is_backfill_display(const Registry& r, ID3D12Resource* native) noexcept {
  for (const auto& slot : r.backfill_displays)
    if (slot.load(std::memory_order_relaxed) == native)
      return true;
  return false;
}
void rearm_live_backfill(Registry& r) noexcept {
  r.live_backfill.store(false, std::memory_order_relaxed);
  r.live_bind.clear_all();
  for (auto& slot : r.backfill_displays)
    slot.store(nullptr, std::memory_order_relaxed);
  for (auto& slot : r.seen_resources.slots)
    slot.store(nullptr, std::memory_order_relaxed);
  for (const auto& [native, item] : r.resources)
    if (item && item->alive) {
      r.seen_resources.remember(native);
      if (display_item(r, *item))
        remember_backfill_display(r, native);
    }
  r.backfill_started_ms = r.backfill_inventory_ms = 0;
  // Existing typed aliases do not establish that the application's older
  // opaque handles have returned. Give every reconnect its bounded full scan.
  r.backfill_full_window = true;
  r.live_backfill.store(true, std::memory_order_relaxed);
  maybe_stop_live_backfill(r);
}
bool committed_readable(const void* p) noexcept {
  MEMORY_BASIC_INFORMATION memory{};
  if (!p || VirtualQuery(p, &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT ||
      (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
    return false;
  const DWORD access = memory.Protect & 255;
  return access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READ ||
         access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
}
bool plausible_resource(ID3D12Resource* native) noexcept {
  if (!committed_readable(native))
    return false;
  void** table{};
  std::memcpy(&table, static_cast<const void*>(native), sizeof(table));
  if (!committed_readable(table))
    return false;
  void* query{};
  std::memcpy(&query, static_cast<const void*>(table), sizeof(query));
  return image_region(query, true);
}
bool observe_resource(ID3D12Device* device, IUnknown* object, source_state::Model initial);
bool admit_live_resource(ID3D12Resource* native, source_state::Model initial) {
  const OwnedWork guard;
  auto& r = registry();
  if (!same_native_device(native, r.device))
    return false;
  return observe_resource(r.device, native, initial);
}
bool can_classify_live_resource(ID3D12Resource* native) noexcept {
  auto& r = registry();
  // Remembering a pointer before GetDesc succeeds hides it from every profile.
  // Cockpit textures are often named in a barrier before that description is valid.
  return native && r.ready && plausible_resource(native) && same_native_device(native, r.device);
}
void consider_live_resource(ID3D12GraphicsCommandList* list, ID3D12Resource* native, source_state::Model initial) {
  auto& r = registry();
  if (!native)
    return;
  if (!r.seen_resources.contains(native)) {
    if (!can_classify_live_resource(native)) {
      if (initial == source_state::Model::unknown)
        r.live_bind.clear(list);
      return;
    }
    bool classified = false;
    observe_safely([&] { classified = admit_live_resource(native, initial); });
    if (!classified)
      return;
    r.seen_resources.remember(native);
  }
  if (initial == source_state::Model::unknown) {
    r.live_bind.clear(list);
    return;
  }
  const std::lock_guard lock(r.mutex);
  const auto found = r.resources.find(native);
  if (is_backfill_display(r, native) && found != r.resources.end() && display_item(r, *found->second))
    r.live_bind.note(list, native, found->second->id);
  else
    r.live_bind.note(list, nullptr);  // An unrecognized RT entry makes this bind ambiguous.
}
void consider_live_copy(ID3D12Resource* native) {
  auto& r = registry();
  if (!native || r.seen_resources.contains(native) || !can_classify_live_resource(native))
    return;
  bool classified = false;
  observe_safely([&] { classified = admit_live_resource(native, source_state::Model::unknown); });
  if (classified)
    r.seen_resources.remember(native);
}
bool bind_live_rtv(Registry& r, List& l, SIZE_T handle) {
  std::uint64_t generation{};
  auto* native = r.live_bind.take(l.native, &generation);
  if (!native)
    return false;
  const auto found = r.resources.find(native);
  if (found == r.resources.end() || found->second->id != generation || !display_item(r, *found->second))
    return false;
  View view{found->second, DXGI_FORMAT_UNKNOWN, 0, handle, true};
  replace_view(r, handle, view);
  l.targets[0] = view;
  l.targets[0].rtv = handle;
  maybe_stop_live_backfill(r);
  return r.routes.matches(found->second->id, r.active_mask | r.calibration_mask);
}
bool observe_resource(ID3D12Device* device, IUnknown* object, source_state::Model initial) {
  auto& r = registry();
  if (!r.ready || !object || !same_device(device))
    return false;
  ID3D12Resource* native{};
  if (FAILED(object->QueryInterface(IID_PPV_ARGS(&native))))
    return false;
  const auto desc = native->GetDesc();
  if (relevant(desc)) {
    std::shared_ptr<Resource> item;
    {
      const std::lock_guard lock(r.mutex);
      auto found = r.resources.find(native);
      if (found != r.resources.end() && found->second->alive) {
        native->Release();
        return true;
      }
      if (r.resources.size() < 16384 && r.next_id != UINT64_MAX) {
        item = std::make_shared<Resource>();
        item->native = native;
        item->key = r.key;
        item->desc = desc;
        item->id = ++r.next_id;
        r.resources[native] = item;
        for (const auto* profile : profiles::Catalog)
          if (profiles::matches_display(*profile, static_cast<UINT>(desc.Width), desc.Height, desc.MipLevels,
                                        static_cast<UINT>(desc.Format))) {
            remember_backfill_display(r, native);
            const auto hash = Registry::SeenResources::hash(native);
            r.display_filter[hash / 64].fetch_or(1ull << (hash % 64), std::memory_order_release);
            break;
          }
        maybe_stop_live_backfill(r);
      } else
        r.pfd_inventory_complete = false;
    }
    if (item && scene_handoff().register_resource(r.key, reinterpret_cast<std::uint64_t>(native), item->id)) {
      runtime::manager().register_source_candidate(r.key, native, item->id, desc, initial);
      if (!attach(native, item)) {
        r.pfd_inventory_complete = false;
        error("resource_lifetime_notification_failed");
      }
    } else if (item) {
      r.pfd_inventory_complete = false;
      item->retire();
      error("resource_registry_full");
    }
  }
  native->Release();
  return true;
}
std::shared_ptr<Resource> resource(ID3D12Resource* p) {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  const auto it = r.resources.find(p);
  return it != r.resources.end() && it->second->alive ? it->second : nullptr;
}
#ifdef TAXI_METADATA_BATCH_VALIDATION
thread_local std::uint64_t metadata_lookup_calls{};
thread_local std::uint64_t registry_lookup_calls{};
thread_local bool bypass_known_list_cache{};
#endif
// A bounded weak cache avoids keeping metadata, descriptor heaps or resources
// alive after registry cleanup. The retained object identity and recording
// must still match; retirement and address reuse never borrow another list's
// proof. Recording calls on one D3D12 list remain caller-serialized as before.
struct KnownListCache {
  struct Entry {
    ID3D12GraphicsCommandList* native{};
    std::uint64_t id{}, recording{};
    std::weak_ptr<List> item;
  };
  std::array<Entry, 8> entries{};
  unsigned next{};
  std::shared_ptr<List> find(ID3D12GraphicsCommandList* native) noexcept {
    for (auto& entry : entries)
      if (entry.native == native) {
        auto item = entry.item.lock();
        if (item && item->alive.load(std::memory_order_acquire) && item->native == native && item->id == entry.id &&
            item->recording.load(std::memory_order_acquire) == entry.recording)
          return item;
        entry = {};
      }
    return {};
  }
  void remember(const std::shared_ptr<List>& item) noexcept {
    if (item)
      entries[next++ % entries.size()] = {item->native, item->id, item->recording.load(std::memory_order_acquire), item};
  }
};
thread_local KnownListCache known_lists;
std::shared_ptr<List> find_list(ID3D12GraphicsCommandList* p) {
#ifdef TAXI_METADATA_BATCH_VALIDATION
  ++metadata_lookup_calls;
#endif
  auto& r = registry();
  const bool diagnostics = r.diagnostics_enabled.load(std::memory_order_relaxed);
  if (diagnostics)
    r.list_lookup_calls.fetch_add(1, std::memory_order_relaxed);
#ifdef TAXI_METADATA_BATCH_VALIDATION
  if (!bypass_known_list_cache)
#endif
    if (auto item = known_lists.find(p)) {
      if (diagnostics)
        r.list_cache_hits.fetch_add(1, std::memory_order_relaxed);
      return item;
    }
#ifdef TAXI_METADATA_BATCH_VALIDATION
  ++registry_lookup_calls;
#endif
  if (diagnostics)
    r.list_registry_lookups.fetch_add(1, std::memory_order_relaxed);
  const std::lock_guard lock(r.mutex);
  const auto it = r.lists.find(p);
  auto item = it != r.lists.end() && it->second->alive ? it->second : nullptr;
#ifdef TAXI_METADATA_BATCH_VALIDATION
  if (!bypass_known_list_cache)
#endif
    known_lists.remember(item);
  return item;
}
thread_local MetadataBatchCache<List, ID3D12GraphicsCommandList*> metadata_batches;
void metadata_begin(void*, ID3D12GraphicsCommandList* native, std::uint64_t id) noexcept {
  const OwnedWork guard;
  std::shared_ptr<List> item;
  if (observation_enabled())
    observe_safely([&] { item = find_list(native); });
  metadata_batches.begin(native, id, std::move(item));
}
void metadata_end(void*, ID3D12GraphicsCommandList*, std::uint64_t) noexcept {
  const OwnedWork guard;
  metadata_batches.end();
}
List* metadata_list(ID3D12GraphicsCommandList* native, std::uint64_t id, std::shared_ptr<List>& fallback) {
  if (auto* item = metadata_batches.current(native, id))
    return item;
  // No scope, overflow, retirement, or Reset uses the original fresh lookup.
  fallback = find_list(native);
  return fallback && fallback->id == id ? fallback.get() : nullptr;
}
void flush_pfd(ID3D12GraphicsCommandList*, std::uint64_t, bool = true) noexcept;
void copy_pending_pfd(ID3D12GraphicsCommandList*, std::uint64_t, ID3D12Resource*, ID3D12GraphicsCommandList7* = nullptr) noexcept;
void before_legacy(void*, ID3D12GraphicsCommandList* list, std::uint64_t id, const D3D12_RESOURCE_TRANSITION_BARRIER& b) noexcept {
  if (idle_callback())
    return;
  const OwnedWork guard;
  copy_pending_pfd(list, id, b.pResource);
  runtime::manager().record_render_target_before_transition(list, b.pResource, true, id);
}
void before_enhanced(void*, ID3D12GraphicsCommandList7* list, std::uint64_t id, const D3D12_TEXTURE_BARRIER& b) noexcept {
  if (idle_callback())
    return;
  const OwnedWork guard;
  copy_pending_pfd(list, id, b.pResource, list);
  runtime::manager().record_render_target_before_enhanced_transition(list, b.pResource, true, id);
}
// The observations below are before native barrier forwarding. They only stage
// evidence; after_draw promotes it after that barrier and the application draw
// have both returned. No GPU command is admitted from a pending observation.
void stage_copy_model(List* list, ID3D12Resource* target, PfdCopyProof::Mode model, const char* reason, std::uint32_t scope) noexcept {
  if (!list)
    return;
  // One selection sample decides both paths; a concurrent routing update must
  // not leave pre-existing proof intact while suppressing the new transition.
  if (!maybe_selected(target)) {
    list->copy_proof.forget_resource(reinterpret_cast<std::uint64_t>(target));
    return;
  }
  if ((scope & (boundary::ScopeActivePass | boundary::ScopeSuspendedPass | boundary::ScopeInvalidRecording)) ||
      !(scope & boundary::ScopeEnabled)) {
    list->copy_proof.invalidate();
    return;
  }
  if (const auto item = resource(target); item && item->alive)
    list->copy_proof.observe_transition({reinterpret_cast<std::uint64_t>(target), item->id}, model, reason);
}
void observe_legacy(void*,
                    ID3D12GraphicsCommandList* list,
                    std::uint64_t id,
                    const D3D12_RESOURCE_BARRIER& b,
                    std::uint32_t scope) noexcept {
  if (registry().live_backfill.load(std::memory_order_relaxed) && b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
      (b.Transition.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET || b.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET))
    consider_live_resource(list, b.Transition.pResource,
                           b.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET && b.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
                                   (b.Transition.Subresource == 0 || b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) &&
                                   !(scope & (boundary::ScopeActivePass | boundary::ScopeSuspendedPass | boundary::ScopeInvalidRecording))
                               ? source_state::Model::legacy_rt
                               : source_state::Model::unknown);
  const bool invalid_submission = b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING || b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
                                  (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Type != D3D12_RESOURCE_BARRIER_TYPE_UAV);
  bool display_transition = false;
  if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
    const auto hash = Registry::SeenResources::hash(b.Transition.pResource);
    display_transition = (registry().display_filter[hash / 64].load(std::memory_order_acquire) & (1ull << (hash % 64))) != 0;
  }
  if (invalid_submission || display_transition) {
    std::shared_ptr<List> fallback;
    if (auto* item = metadata_list(list, id, fallback)) {
      if (invalid_submission)
        item->submission_proof.invalidate(PfdSubmissionProof::Refusal::barrier_uncertainty);
      else if (const auto target = resource(b.Transition.pResource); target && target->alive && display_candidate_desc(target->desc)) {
        const auto subresource = b.Transition.Subresource;
        // Exact admitted 8-bit color displays have one layer/plane. A complete
        // transition of another valid mip cannot change the copied base mip.
        // Split/alias uncertainty above still invalidates the whole recording.
        if (subresource == 0 || subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
          item->submission_proof.observe_legacy({reinterpret_cast<std::uint64_t>(target->native), target->id}, b.Transition.StateBefore,
                                                b.Transition.StateAfter, subresource, b.Flags);
        else if (subresource >= target->desc.MipLevels)
          item->submission_proof.invalidate(PfdSubmissionProof::Refusal::invalid_transition);
      }
    }
  }
  if (idle_callback()) {
    // The source layout model must remain continuous even when no capture is
    // requested: persistent born-RT sources may never emit another RT entry.
    runtime::manager().observe_source_legacy(list, id, b);
    return;
  }
  const OwnedWork guard;
  std::shared_ptr<List> fallback;
  auto* item = b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ? metadata_list(list, id, fallback) : nullptr;
  if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
    if (item) {
      for (UINT i = 0; i < item->count; ++i)
        if (item->targets[i].resource && item->targets[i].resource->native == b.Transition.pResource)
          item->pfd_transition = true;
    }
  if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET &&
      b.Transition.StateAfter != D3D12_RESOURCE_STATE_RENDER_TARGET)
    selected_metadata(b.Transition.pResource, scope,
                      b.Transition.Subresource == 0 || b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                      (b.Flags & (D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY | D3D12_RESOURCE_BARRIER_FLAG_END_ONLY)) != 0);
  if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
    if ((item = metadata_list(list, id, fallback)))
      for (unsigned side = 0; side < 2; ++side)
        if (item->pending_pfds[side].resource && item->pending_pfds[side].resource->native == b.Transition.pResource)
          item->pending_rt[side] = false;
  if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
    if ((item = metadata_list(list, id, fallback)))
      item->copy_proof.invalidate();
  } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
    const bool complete = b.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
                          (b.Transition.Subresource == 0 || b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    stage_copy_model(metadata_list(list, id, fallback), b.Transition.pResource,
                     complete && b.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET ? PfdCopyProof::Mode::legacy_rt
                                                                                               : PfdCopyProof::Mode::unknown,
                     complete ? "not_rt_entry" : "split_or_partial_transition", scope);
  }
  runtime::manager().observe_source_legacy(list, id, b);
}
void observe_enhanced(void*,
                      ID3D12GraphicsCommandList7* list,
                      std::uint64_t id,
                      const D3D12_TEXTURE_BARRIER& b,
                      std::uint32_t scope) noexcept {
  if (registry().live_backfill.load(std::memory_order_relaxed) &&
      (b.LayoutBefore == D3D12_BARRIER_LAYOUT_RENDER_TARGET || b.LayoutAfter == D3D12_BARRIER_LAYOUT_RENDER_TARGET))
    consider_live_resource(
        list, b.pResource,
        b.LayoutAfter == D3D12_BARRIER_LAYOUT_RENDER_TARGET && b.AccessAfter == D3D12_BARRIER_ACCESS_RENDER_TARGET &&
                b.Flags == D3D12_TEXTURE_BARRIER_FLAG_NONE && !((b.SyncBefore | b.SyncAfter) & D3D12_BARRIER_SYNC_SPLIT) &&
                (!b.Subresources.NumMipLevels ? b.Subresources.IndexOrFirstMipLevel == 0 || b.Subresources.IndexOrFirstMipLevel == UINT_MAX
                                              : b.Subresources.IndexOrFirstMipLevel == 0 && b.Subresources.NumMipLevels == 1 &&
                                                    b.Subresources.FirstArraySlice == 0 && b.Subresources.NumArraySlices == 1 &&
                                                    b.Subresources.FirstPlane == 0 && b.Subresources.NumPlanes == 1) &&
                !(scope & (boundary::ScopeActivePass | boundary::ScopeSuspendedPass | boundary::ScopeInvalidRecording))
            ? source_state::Model::enhanced_rt
            : source_state::Model::unknown);
  if (idle_callback()) {
    runtime::manager().observe_source_enhanced(list, id, b);
    return;
  }
  const OwnedWork guard;
  std::shared_ptr<List> fallback;
  auto* item = metadata_list(list, id, fallback);
  if (item) {
    for (UINT i = 0; i < item->count; ++i)
      if (item->targets[i].resource && item->targets[i].resource->native == b.pResource)
        item->pfd_transition = true;
  }
  if (b.LayoutBefore == D3D12_BARRIER_LAYOUT_RENDER_TARGET && b.LayoutAfter != D3D12_BARRIER_LAYOUT_RENDER_TARGET)
    selected_metadata(b.pResource, scope,
                      !b.Subresources.NumMipLevels
                          ? b.Subresources.IndexOrFirstMipLevel == 0 || b.Subresources.IndexOrFirstMipLevel == UINT_MAX
                          : b.Subresources.IndexOrFirstMipLevel == 0 && b.Subresources.NumMipLevels == 1 &&
                                b.Subresources.FirstArraySlice == 0 && b.Subresources.NumArraySlices == 1 &&
                                b.Subresources.FirstPlane == 0 && b.Subresources.NumPlanes == 1,
                      ((b.SyncBefore | b.SyncAfter) & D3D12_BARRIER_SYNC_SPLIT) != 0);
  if ((item = metadata_list(list, id, fallback)))
    for (unsigned side = 0; side < 2; ++side)
      if (item->pending_pfds[side].resource && item->pending_pfds[side].resource->native == b.pResource)
        item->pending_rt[side] = false;
  const auto& range = b.Subresources;
  const bool whole = !range.NumMipLevels ? range.IndexOrFirstMipLevel == 0 || range.IndexOrFirstMipLevel == UINT_MAX
                                         : range.IndexOrFirstMipLevel == 0 && range.NumMipLevels == 1 && range.FirstArraySlice == 0 &&
                                               range.NumArraySlices == 1 && range.FirstPlane == 0 && range.NumPlanes == 1;
  const bool complete = whole && b.Flags == D3D12_TEXTURE_BARRIER_FLAG_NONE && !((b.SyncBefore | b.SyncAfter) & D3D12_BARRIER_SYNC_SPLIT);
  stage_copy_model(metadata_list(list, id, fallback), b.pResource,
                   complete && b.LayoutAfter == D3D12_BARRIER_LAYOUT_RENDER_TARGET && b.AccessAfter == D3D12_BARRIER_ACCESS_RENDER_TARGET
                       ? PfdCopyProof::Mode::enhanced_rt
                       : PfdCopyProof::Mode::unknown,
                   complete ? "not_rt_entry" : "split_or_partial_transition", scope);
  runtime::manager().observe_source_enhanced(list, id, b);
}
void copy_resource(void*,
                   ID3D12GraphicsCommandList* list,
                   std::uint64_t id,
                   ID3D12Resource* dest,
                   ID3D12Resource* src,
                   bool allowed) noexcept {
  if (registry().live_backfill.load(std::memory_order_relaxed)) {
    consider_live_copy(dest);
    consider_live_copy(src);
  }
  if (auto item = find_list(list); item && item->id == id)
    item->submission_proof.state_disjoint_work(17);
  if (idle_callback())
    return;
  const OwnedWork guard;
  runtime::manager().record_copy_after_forward(list, src, dest, allowed, id);
}
void copy_texture(void*,
                  ID3D12GraphicsCommandList* list,
                  std::uint64_t id,
                  const D3D12_TEXTURE_COPY_LOCATION* dest,
                  UINT x,
                  UINT y,
                  UINT z,
                  const D3D12_TEXTURE_COPY_LOCATION* src,
                  const D3D12_BOX* box,
                  bool allowed) noexcept {
  if (registry().live_backfill.load(std::memory_order_relaxed)) {
    consider_live_copy(dest ? dest->pResource : nullptr);
    consider_live_copy(src ? src->pResource : nullptr);
  }
  if (auto item = find_list(list); item && item->id == id)
    item->submission_proof.state_disjoint_work(16);
  if (idle_callback())
    return;
  const OwnedWork guard;
  runtime::manager().record_texture_copy_after_forward(list, dest, x, y, z, src, box, allowed, id);
}
void invalidate(void*, ID3D12GraphicsCommandList* native, std::uint64_t id, std::uint32_t reasons) noexcept {
  const OwnedWork guard;
  if (registry().live_backfill.load(std::memory_order_relaxed))
    registry().live_bind.clear(native);
  auto list = find_list(native);
  if (!list || list->id != id)
    return;
  if (reasons & ~boundary::InvalidationPassBegin)
    list->submission_proof.invalidate(PfdSubmissionProof::Refusal::boundary_invalidation);
  if (recording_observed(*list)) {
    if (reasons & ~boundary::InvalidationSplitBarrier)
      list->copy_proof.invalidate();
    if (reasons == boundary::InvalidationPassBegin)
      flush_pfd(native, id);
    else
      list->pending_pfds = {};
    list->pending_rt = {};
    list->pfd_dirty = false;
  }
  const bool scoped =
      (reasons & ~(boundary::InvalidationPassBegin | boundary::InvalidationSplitBarrier | boundary::InvalidationAliasOrDiscard)) == 0;
  if (scoped && list->count) {
    std::array<ID3D12Resource*, 8> targets{};
    std::array<std::uint64_t, 8> generations{};
    UINT count = 0;
    for (UINT i = 0; i < list->count; ++i) {
      const auto& target = list->targets[i].resource;
      if (target && target->alive) {
        targets[count] = target->native;
        generations[count++] = target->id;
      }
    }
    if (count) {
      runtime::manager().invalidate_source_targets(native, id, count, targets.data(), generations.data());
      return;
    }
  }
  runtime::manager().invalidate_source_recording(native, id, true, reasons);
}
void after_draw(void*, ID3D12GraphicsCommandList* native, std::uint64_t id, bool allowed) noexcept {
  const OwnedWork guard;
  auto list = find_list(native);
  if (!registry().ready || !list || list->id != id || !list->ready)
    return;
  list->submission_proof.gpu_work(12);
  auto& r = registry();
  const bool observed = recording_observed(*list);
  // Idle keeps only candidate activity/source proof. In particular ordinary
  // draws without any tracked RTV avoid even the global diagnostic counter.
  if (!observed && !list->count)
    return;
  if (observed)
    ++r.draws;
  std::array<ID3D12Resource*, 8> sources;
  std::array<std::uint64_t, 8> ids;
  UINT count = 0;
  for (UINT i = 0; i < list->count; ++i) {
    const auto& target = list->targets[i];
    if (target.resource && target.resource->alive && !target.mip) {
      target.resource->draws.fetch_add(1, std::memory_order_relaxed);
      if (observed) {
        if (allowed)
          list->copy_proof.after_draw({reinterpret_cast<std::uint64_t>(target.resource->native), target.resource->id});
        else
          list->copy_proof.invalidate();
        const auto selected_mask = r.selected_mask.load(std::memory_order_relaxed);
        if (((selected_mask & 1) && target.resource->id == r.selected_ids[0].load(std::memory_order_relaxed)) ||
            ((selected_mask & 2) && target.resource->id == r.selected_ids[1].load(std::memory_order_relaxed)))
          ++r.selected_draws;
      }
      sources[count] = target.resource->native;
      ids[count++] = target.resource->id;
    }
  }
  runtime::manager().observe_source_draw_after(native, id, count, sources.data(), ids.data(), allowed);
  if (!observed)
    return;
  // Mark damage only. Compositing between individual HTML/glyph draws both
  // multiplies fill cost and unnecessarily disturbs the application's state.
  list->pfd_dirty = allowed && list->count == 1 && list->depth_known;
  list->pfd_transition = false;
}
// Stage only actual typed RTVs established by a nonzero native draw.
void stage_pfd(ID3D12GraphicsCommandList* native, std::uint64_t id) noexcept {
  if (!observation_enabled())
    return;
  auto list = find_list(native);
  if (!registry().ready || !list || list->id != id || !list->ready || !list->pfd_dirty || !recording_observed(*list))
    return;
  list->pfd_dirty = false;
  if (list->count != 1 || !list->depth_known)
    return;
  auto& r = registry();
  const auto& view = list->targets[0];
  if (!view.resource || !view.resource->alive || view.mip)
    return;
  const std::lock_guard lock(r.mutex);
  if (!profiles::matches_display(*r.profile, static_cast<UINT>(view.resource->desc.Width), view.resource->desc.Height,
                                 view.resource->desc.MipLevels, static_cast<UINT>(view.resource->desc.Format)))
    return;
  for (unsigned side = 0; side < 2; ++side)
    if (r.routes.targets[side] == view.resource->id && ((r.active_mask | r.calibration_mask) & (1u << side))) {
      list->pending_pfds[side] = view;
      list->pending_rt[side] = !list->pfd_transition && list->raw_om_known && list->snapshot_rtvs;
      if (view.recovered && list->copy_proof.mode({reinterpret_cast<std::uint64_t>(view.resource->native), view.resource->id}) ==
                                PfdCopyProof::Mode::unknown) {
        // A temporal OM association is not descriptor identity. Only an actual
        // complete RT entry in this recording can authorize writing our owned
        // view. A later native RT-exit callback supplies its own state proof.
        list->pending_rt[side] = false;
        continue;
      }
      D3D12_CPU_DESCRIPTOR_HANDLE retained{};
      if (list->raw_om_known && list->snapshot_rtvs) {
        retained = {list->snapshot_rtvs->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{8 + side} * r.rtv_stride};
        if (view.recovered) {
          const auto format = output_format(view);
          if (format == DXGI_FORMAT_UNKNOWN) {
            list->pending_rt[side] = false;
            continue;
          }
          D3D12_RENDER_TARGET_VIEW_DESC desc{};
          desc.Format = format;
          desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
          const OwnedWork owned;
          r.device->CreateRenderTargetView(view.resource->native, &desc, retained);
        } else
          r.device->CopyDescriptorsSimple(1, retained, list->raw_rtvs[0], D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
      }
      // A nonzero native draw established this bound RTV as render-target data.
      // Clear-only calibration needs no guessed legacy/enhanced transition and
      // changes no graphics bindings. An intervening transition or unsafe pass
      // still refuses; calibration changes no graphics or query state.
      const auto current = r.rtvs.find(view.rtv);
      if ((r.calibration_mask & (1u << side)) && retained.ptr && !list->pfd_transition &&
          boundary::recording_allows_injection(native, id) && current != r.rtvs.end() && current->second.resource == view.resource &&
          current->second.format == view.format && current->second.recovered == view.recovered && !current->second.mip &&
          r.calibration_budget.try_acquire(0, GetTickCount64())) {
        const auto area = profiles::display_rect(*r.profile, side);
        const boundary::ScopedBypass bypass;
        if (record_calibration(native, retained, area.right - area.left, view.resource->desc.Height, GetTickCount64() / 16, area.left))
          ++r.calibration_clears;
      }
    }
}
void drain_pfds(ID3D12GraphicsCommandList* native, std::uint64_t id, bool recording_end = false) noexcept {
  if (!observation_enabled())
    return;
  auto list = find_list(native);
  if (!registry().ready || !list || list->id != id || !list->ready || !recording_observed(*list))
    return;
  bool pending = false;
  for (unsigned side = 0; side < 2; ++side)
    pending |= list->pending_rt[side] && bool(list->pending_pfds[side].resource);
  if (!pending)
    return;
  auto& r = registry();
  if (!boundary::recording_allows_injection(native, id)) {
    ++r.fallback_state_refused;
    return;
  }
  const std::lock_guard lock(r.mutex);
  for (unsigned side = 0; side < 2; ++side) {
    const auto view = list->pending_pfds[side];
    if (!list->pending_rt[side] || !view.resource || !view.resource->alive || !view.rtv || view.mip || !(r.active_mask & (1u << side)) ||
        (r.calibration_mask & (1u << side)) || r.routes.targets[side] != view.resource->id)
      continue;
    const auto current = r.rtvs.find(view.rtv);
    const auto format = output_format(view);
    if (current == r.rtvs.end() || current->second.resource != view.resource || current->second.format != view.format ||
        current->second.recovered != view.recovered || current->second.mip || format == DXGI_FORMAT_UNKNOWN ||
        !profiles::matches_display(*r.profile, static_cast<UINT>(view.resource->desc.Width), view.resource->desc.Height,
                                   view.resource->desc.MipLevels, static_cast<UINT>(view.resource->desc.Format))) {
      list->pending_rt[side] = false;
      ++r.fallback_state_refused;
      continue;
    }
    const auto area = profiles::display_rect(*r.profile, side), content = profiles::display_content_rect(*r.profile, side);
    const D3D12_RECT destination{static_cast<LONG>(area.left), static_cast<LONG>(area.top), static_cast<LONG>(area.right),
                                 static_cast<LONG>(area.bottom)};
    const D3D12_RECT inner{static_cast<LONG>(content.left), static_cast<LONG>(content.top), static_cast<LONG>(content.right),
                           static_cast<LONG>(content.bottom)};
    const PfdCopyProof::Key proof_key{reinterpret_cast<std::uint64_t>(view.resource->native), view.resource->id};
    const auto model = list->copy_proof.mode(proof_key);
    if (model != PfdCopyProof::Mode::unknown) {
      ++r.preferred_copy_attempts;
      // ensure_list's boundary registration has already proved identical QI7.
      auto* enhanced = model == PfdCopyProof::Mode::enhanced_rt ? static_cast<ID3D12GraphicsCommandList7*>(native) : nullptr;
      if (runtime::copy_patch(native, r.key, view.resource->native, view.resource->desc, format, destination, inner, enhanced)) {
        ++r.preferred_copy_stamps;
        r.preferred_copy_reason = model == PfdCopyProof::Mode::legacy_rt ? "copied_legacy" : "copied_enhanced";
        list->pending_rt[side] = false;
        list->pending_pfds[side] = {};
        continue;
      }
      r.preferred_copy_reason = "private_copy_refused";
    } else {
      ++r.preferred_copy_no_proof;
      r.preferred_copy_reason = list->copy_proof.reason(proof_key);
    }
    // Shader delivery is terminal work on a DIRECT recording. Intermediate
    // boundaries retain the typed target, without replaying application roots.
    if (!recording_end) {
      ++r.shader_deferred;
      continue;
    }
    if (!r.close_forward_verified) {
      ++r.close_forward_refused;
      continue;
    }
    // The private copy neither changes graphics bindings nor contributes query
    // samples. These original admission checks continue to guard shader draws.
    if (!list->queries.known_empty()) {
      ++r.fallback_query_refused;
      continue;
    }
    const auto root = r.roots.find(list->graphics.root());
    if (!list->raw_om_known || !list->graphics.complete() || root == r.roots.end() || !root->second->alive ||
        root->second->id != list->graphics.layout_generation()) {
      ++r.fallback_state_refused;
      continue;
    }
    const boundary::ScopedBypass bypass;
    const D3D12_CPU_DESCRIPTOR_HANDLE target{list->snapshot_rtvs->GetCPUDescriptorHandleForHeapStart().ptr +
                                             SIZE_T{8 + side} * r.rtv_stride};
    native->OMSetRenderTargets(1, &target, FALSE, nullptr);
    ++r.fallback_attempts;
    // This is the last work recorded before native Close. Leave our bindings
    // in place: no application commands follow and no root replay is needed.
    if (runtime::stamp_at_recording_end(native, list->graphics, r.key, format, static_cast<UINT>(view.resource->desc.Width),
                                        view.resource->desc.Height, DXGI_FORMAT_UNKNOWN, &destination, &inner)) {
      ++r.fallback_stamps;
      ++r.recording_end_draws;
      list->pending_rt[side] = false;
      list->pending_pfds[side] = {};
    }
  }
}
void flush_pfd(ID3D12GraphicsCommandList* native, std::uint64_t id, bool draw_fallback) noexcept {
  stage_pfd(native, id);
  if (draw_fallback)
    drain_pfds(native, id);
}
UINT selected_legacy_targets(void*, ID3D12GraphicsCommandList* native, std::uint64_t id, ID3D12Resource** targets, UINT capacity) noexcept {
  if (!targets || capacity < 2 || !registry().ready || !observation_enabled())
    return 0;
  const auto list = find_list(native);
  if (!list || list->id != id || !list->ready || !recording_observed(*list))
    return 0;
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  refresh_selected(r);
  UINT count = 0;
  for (unsigned side = 0; side < 2; ++side) {
    const auto& item = r.selected_resources[side];
    if (((r.active_mask | r.calibration_mask) & (1u << side)) && item && item->alive &&
        profiles::matches_display(*r.profile, static_cast<UINT>(item->desc.Width), item->desc.Height, item->desc.MipLevels,
                                  static_cast<UINT>(item->desc.Format)))
      targets[count++] = item->native;
  }
  return count;
}
void copy_pending_pfd(ID3D12GraphicsCommandList* native,
                      std::uint64_t id,
                      ID3D12Resource* target,
                      ID3D12GraphicsCommandList7* enhanced) noexcept {
  if (!observation_enabled() || !maybe_selected(target))
    return;
  flush_pfd(native, id, false);
  auto list = find_list(native);
  if (!registry().ready || !list || list->id != id || !list->ready || !recording_observed(*list) ||
      !boundary::recording_allows_injection(native, id))
    return;
  auto& r = registry();
  View pending;
  for (auto& view : list->pending_pfds)
    if (view.resource && view.resource->native == target) {
      pending = view;
      list->pending_rt[static_cast<unsigned>(&view - list->pending_pfds.data())] = false;
      view = {};
      break;
    }
  View view;
  bool selected = false;
  profiles::DisplayRect area{}, content{};
  {
    const std::lock_guard lock(r.mutex);
    refresh_selected(r);
    std::shared_ptr<Resource> item;
    unsigned side = 0;
    for (unsigned i = 0; i < 2; ++i)
      if (((r.active_mask | r.calibration_mask) & (1u << i)) && r.selected_resources[i] && r.selected_resources[i]->alive &&
          r.selected_resources[i]->native == target) {
        item = r.selected_resources[i];
        side = i;
        break;
      }
    if (!item || !profiles::matches_display(*r.profile, static_cast<UINT>(item->desc.Width), item->desc.Height, item->desc.MipLevels,
                                            static_cast<UINT>(item->desc.Format)))
      return;
    ++r.selected_rt_callbacks;
    const auto current = r.rtvs.find(pending.rtv);
    const bool typed = output_format(pending) != DXGI_FORMAT_UNKNOWN;
    if (pending.resource == item && !pending.mip && typed && current != r.rtvs.end() && current->second.resource == item &&
        current->second.format == pending.format && current->second.recovered == pending.recovered && current->second.mip == 0) {
      view = pending;
      ++r.selected_pending_matches;
    } else {
      unsigned count = 0;
      for (unsigned i = 0; i < TypedFormats.size(); ++i)
        if (item->typed_rtv_refs[i]) {
          view = {item, TypedFormats[i], 0, 0};
          ++count;
        }
      if (!count)
        for (const auto& [handle, recovered] : r.rtvs) {
          (void)handle;
          if (recovered.resource == item && recovered.recovered && !recovered.mip && output_format(recovered) != DXGI_FORMAT_UNKNOWN) {
            // A recovered bind supplies exact resource/base-mip identity. The
            // copy uses our explicitly chosen compatible encoding, not an
            // inferred application RTV format or borrowed descriptor.
            view = recovered;
            count = 1;
            break;
          }
        }
      if (count != 1) {
        ++r.selected_view_rejected;
        r.copy_error = count ? "typed_rtv_conflict" : "typed_rtv_missing";
        return;
      }
    }
    ++r.selected_view_resolved;
    area = profiles::display_rect(*r.profile, side);
    content = profiles::display_content_rect(*r.profile, side);
    selected = r.routes.matches(item->id, r.active_mask);
  }
  // Calibration is delivered only from the retained descriptor snapshot in stage_pfd.
  if (!selected)
    return;
  const boundary::ScopedBypass bypass;
  const D3D12_RECT destination{static_cast<LONG>(area.left), static_cast<LONG>(area.top), static_cast<LONG>(area.right),
                               static_cast<LONG>(area.bottom)};
  const D3D12_RECT inner{static_cast<LONG>(content.left), static_cast<LONG>(content.top), static_cast<LONG>(content.right),
                         static_cast<LONG>(content.bottom)};
  ++r.copy_attempts;
  if (!runtime::copy_patch(native, r.key, view.resource->native, view.resource->desc, output_format(view), destination, inner, enhanced)) {
    ++r.copy_rejected;
    r.copy_error = "runtime_copy_rejected";
  } else
    r.copy_error = "copied";
}
void pass_targets(void*,
                  ID3D12GraphicsCommandList*,
                  std::uint64_t,
                  UINT,
                  const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                  const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*) noexcept;
void pass_ended(void*, ID3D12GraphicsCommandList* native, std::uint64_t id) noexcept {
  if (auto item = find_list(native); item && item->id == id) {
    item->submission_proof.end_pass();
    flush_pfd(native, id);
    item->pfd_dirty = false;
    item->targets = {};
    item->count = 0;
    item->depth_known = false;
    item->raw_om_known = false;
    item->pending_rt = {};
  }
}
void submission_pass_began(void*,
                           ID3D12GraphicsCommandList* native,
                           std::uint64_t id,
                           D3D12_RENDER_PASS_FLAGS flags,
                           bool ordinary) noexcept {
  if (auto item = find_list(native); item && item->id == id)
    item->submission_proof.begin_pass(flags, ordinary);
}
void submission_enhanced(void*, ID3D12GraphicsCommandList* native, std::uint64_t id) noexcept {
  if (auto item = find_list(native); item && item->id == id)
    item->submission_proof.invalidate(PfdSubmissionProof::Refusal::enhanced_barrier);
}
const boundary::Callbacks Boundaries{nullptr,
                                     before_legacy,
                                     before_enhanced,
                                     observe_legacy,
                                     observe_enhanced,
                                     copy_resource,
                                     copy_texture,
                                     after_draw,
                                     invalidate,
                                     pass_targets,
                                     pass_ended,
                                     selected_legacy_targets,
                                     metadata_begin,
                                     metadata_end,
                                     submission_pass_began,
                                     submission_enhanced};
std::shared_ptr<List> ensure_list(ID3D12GraphicsCommandList* native, bool observed = false) {
  if (auto existing = find_list(native))
    return existing;
  auto& r = registry();
  const std::lock_guard observation_lock(r.observation_mutex);
  if (!r.ready || native->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !same_native_device(native, r.device))
    return {};
  std::shared_ptr<List> item;
  {
    const std::lock_guard lock(r.mutex);
    if (r.lists.size() >= 4096 || r.next_id == UINT64_MAX) {
      error("command_list_capacity");
      return {};
    }
    item = std::make_shared<List>();
    item->native = native;
    item->id = ++r.next_id;
    item->ready = observed;
    item->observation_epoch = observed ? r.observation_epoch.load(std::memory_order_acquire) : 0;
    item->graphics.reset(item->recording, observed);
    item->queries.reset(observed);
    item->copy_proof.reset(observed);
    item->submission_proof.reset(item->recording, observed);
    item->raw_om_known = observed;
    r.lists[native] = item;
    r.live_bind.clear(native);
  }
  const auto hooked = boundary::register_list(native, item->id, Boundaries);
  const bool registered = observed ? runtime::manager().register_command_list(native, r.key, item->id)
                                   : runtime::manager().register_unobserved_command_list(native, r.key, item->id);
  if (!hooked.ready || !hooked.protection_restored || !registered || !attach(native, item)) {
    item->retire();
    error("native_list_registration_failed");
    return {};
  }
  if (observed)
    boundary::successful_reset(native, item->id);
  else
    boundary::reset_failed(native, item->id);
  return item;
}
void unknown_list(void*, ID3D12GraphicsCommandList* list, std::uint64_t) noexcept {
  const OwnedWork guard;
  observe_safely([&] { ensure_list(list); });
}
// Called before manager/runtime submission serialization. The patch snapshot
// is nonblocking, and the registry is only try-locked: missing metadata must
// never make ExecuteCommandLists wait for discovery or composition.
void plan_display_submission(void*,
                             ID3D12CommandQueue*,
                             UINT count,
                             ID3D12CommandList* const* lists,
                             SceneCaptureManager::DisplaySubmissionPlan& plan) noexcept {
  auto& r = registry();
  // An unknown downstream Close chain could append unobserved work after our
  // terminal bookkeeping. Only the verified native endpoint seals this proof.
  const auto outcome = [&](DisplaySubmissionOutcome value) noexcept {
    r.queue_outcomes[static_cast<unsigned>(value)].fetch_add(1, std::memory_order_relaxed);
  };
  if (!r.ready) {
    outcome(DisplaySubmissionOutcome::not_ready);
    return;
  }
  if (!r.close_forward_verified) {
    outcome(DisplaySubmissionOutcome::unverified_close);
    return;
  }
  if (!lists || !count || count > PfdSubmissionProof::maximum_batch) {
    outcome(DisplaySubmissionOutcome::invalid_batch);
    return;
  }
  const auto generation = r.queue_patch_generation.load(std::memory_order_acquire);
  runtime::QueuePatchSnapshot patches;
  runtime::try_snapshot_queue_patches(r.key, generation, patches);
  const std::unique_lock lock(r.mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    outcome(DisplaySubmissionOutcome::registry_busy);
    return;
  }
  if (generation != r.queue_patch_generation.load(std::memory_order_acquire)) {
    outcome(DisplaySubmissionOutcome::generation_changed);
    return;
  }
  std::array<PfdSubmissionProof::Recording, PfdSubmissionProof::maximum_batch> batch{};
  const auto session_floor = r.session_recording_floor.load(std::memory_order_acquire);
  for (UINT i = 0; i < count; ++i) {
    const auto found = r.lists.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
    if (found == r.lists.end() || !found->second->alive) {
      outcome(DisplaySubmissionOutcome::unknown_list);
      return;
    }
    const auto& item = *found->second;
    if (item.observation_epoch.load(std::memory_order_acquire) < session_floor) {
      outcome(DisplaySubmissionOutcome::generation_changed);
      return;
    }
    const auto recording = item.closed_recording.load(std::memory_order_acquire);
    if (!recording || recording != item.recording.load(std::memory_order_acquire)) {
      outcome(DisplaySubmissionOutcome::unclosed_list);
      return;
    }
    batch[i] = {&item.submission_proof, recording};
  }
  if (!PfdSubmissionProof::batch_allows(batch.data(), count)) {
    for (UINT i = 0; i < count; ++i)
      if (!batch[i].proof->complete(batch[i].generation)) {
        const auto reason = batch[i].proof->refusal();
        r.queue_proof_refusals[static_cast<unsigned>(reason)].fetch_add(1, std::memory_order_relaxed);
        r.queue_last_proof_flags.store(batch[i].proof->diagnostic_flags(batch[i].generation), std::memory_order_relaxed);
        break;
      }
    outcome(DisplaySubmissionOutcome::incomplete_proof);
    return;
  }
  const auto no_position = count * 2;
  std::array<UINT, 2> positions{no_position, no_position};
  std::array<PfdSubmissionProof::Candidate, 2> selected{};
  unsigned candidates = 0;
  for (UINT i = 0; i < count; ++i) {
    for (std::size_t slot = 0; slot < PfdSubmissionProof::maximum_resources; ++slot) {
      auto exit = batch[i].proof->candidate(slot, batch[i].generation);
      bool before = false;
      if (!exit) {
        exit = batch[i].proof->prefix_candidate(slot, batch[i].generation);
        before = bool(exit);
      }
      if (!exit) {
        const auto refusal = batch[i].proof->prefix_refusal(slot, batch[i].generation);
        if (refusal.operation < r.queue_prefix_blockers.size()) {
          const auto target = r.resources.find(reinterpret_cast<ID3D12Resource*>(refusal.key.resource));
          if (target != r.resources.end() && target->second->id == refusal.key.generation && display_item(r, *target->second))
            r.queue_prefix_blockers[refusal.operation].fetch_add(1, std::memory_order_relaxed);
        }
        continue;
      }
      auto* native = reinterpret_cast<ID3D12Resource*>(exit.key.resource);
      const auto found = r.resources.find(native);
      if (found == r.resources.end() || !found->second->alive || found->second->id != exit.key.generation ||
          !display_item(r, *found->second))
        continue;
      ++candidates;
      // A submitted, identity-qualified RT completion is independent of camera
      // demand. Keep this distinct from actual draws and preserve side ordering.
      found->second->submission_activity.fetch_add(1, std::memory_order_relaxed);
      for (unsigned side = 0; side < 2; ++side)
        if (patches.ready_mask & (1u << side))
          if (r.selected_resources[side] == found->second) {
            positions[side] = i * 2 + (before ? 0u : 1u);
            selected[side] = exit;
          }
    }
  }
  if (!candidates) {
    outcome(DisplaySubmissionOutcome::no_display_exit);
    return;
  }
  if (patches.generation != generation || patches.profile != r.profile->id) {
    outcome(DisplaySubmissionOutcome::patch_not_ready);
    return;
  }
  plan.device_key = r.key;
  plan.generation = generation;
  plan.current_generation = &r.queue_patch_generation;
  auto empty_reason = DisplaySubmissionOutcome::no_selected_exit;
  for (unsigned order = 0; order < 2; ++order) {
    const unsigned side = positions[0] <= positions[1] ? order : 1u - order;
    if (positions[side] == no_position || !selected[side])
      continue;
    const auto& target = r.selected_resources[side];
    const auto& patch = patches.sides[side];
    if (!target || !target->alive || !patch.buffer || !display_item(r, *target)) {
      empty_reason = DisplaySubmissionOutcome::unavailable_target;
      continue;
    }
    if (patch.calibration && !r.calibration_budget.try_acquire(0, GetTickCount64())) {
      empty_reason = DisplaySubmissionOutcome::calibration_budget;
      continue;
    }
    auto& item = plan.items[plan.count++];
    item.after_list = positions[side] / 2;
    item.before = positions[side] % 2 == 0;
    item.copy = {target->native,
                 patch.buffer,
                 patch.footprint,
                 item.before ? selected[side].first_before : selected[side].state_after,
                 static_cast<UINT>(patch.destination.left),
                 static_cast<UINT>(patch.destination.top),
                 static_cast<UINT>(patch.destination.right - patch.destination.left),
                 static_cast<UINT>(patch.destination.bottom - patch.destination.top)};
    item.copy.target->AddRef();
    item.copy.source->AddRef();
  }
  if (plan.count)
    r.queue_patch_plans.fetch_add(1, std::memory_order_relaxed);
  outcome(plan.count ? DisplaySubmissionOutcome::planned : empty_reason);
}
void unknown_queue(ID3D12CommandQueue* queue) noexcept {
  if (owned_depth || !registry().ready || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return;
  const OwnedWork guard;
  if (!same_native_device(queue, registry().device)) {
    error("queue_device_mismatch");
    return;
  }
  if (!runtime::init_queue(registry().key, queue))
    error("queue_registration_failed");
}
std::array<NativeSlot, 10> creations;
constexpr std::array<unsigned, 10> CreationSlots{27, 29, 30, 53, 55, 69, 70, 76, 77, 78};
constexpr std::array<unsigned, 10> StateArgs{3, 3, 1, 3, 1, 3, 3, 3, 3, 1};
source_state::Model model(D3D12_RESOURCE_STATES s) {
  return s == D3D12_RESOURCE_STATE_RENDER_TARGET ? source_state::Model::legacy_rt : source_state::Model::unknown;
}
source_state::Model model(D3D12_BARRIER_LAYOUT s) {
  return s == D3D12_BARRIER_LAYOUT_RENDER_TARGET ? source_state::Model::enhanced_rt : source_state::Model::unknown;
}
template <unsigned I, class Signature>
struct Creation;
template <unsigned I, class C, class... Args>
struct Creation<I, HRESULT (STDMETHODCALLTYPE C::*)(Args...)> {
  using F = HRESULT(STDMETHODCALLTYPE*)(C*, Args...);
  static HRESULT STDMETHODCALLTYPE call(C* self, Args... args) noexcept {
    const bool observe = owned_depth == 0;
    const OwnedWork guard;
    const auto hr = creations[I].forward<F>()(self, args...);
    if (observe && SUCCEEDED(hr)) {
      const auto tuple = std::forward_as_tuple(args...);
      auto** out = std::get<sizeof...(Args) - 1>(tuple);
      if (out && *out)
        observe_safely([&] {
          observe_resource(reinterpret_cast<ID3D12Device*>(self), static_cast<IUnknown*>(*out), model(std::get<StateArgs[I]>(tuple)));
        });
    }
    return hr;
  }
};
const std::array<void*, 10> CreationWrappers{
    reinterpret_cast<void*>(&Creation<0, decltype(&ID3D12Device::CreateCommittedResource)>::call),
    reinterpret_cast<void*>(&Creation<1, decltype(&ID3D12Device::CreatePlacedResource)>::call),
    reinterpret_cast<void*>(&Creation<2, decltype(&ID3D12Device::CreateReservedResource)>::call),
    reinterpret_cast<void*>(&Creation<3, decltype(&ID3D12Device4::CreateCommittedResource1)>::call),
    reinterpret_cast<void*>(&Creation<4, decltype(&ID3D12Device4::CreateReservedResource1)>::call),
    reinterpret_cast<void*>(&Creation<5, decltype(&ID3D12Device8::CreateCommittedResource2)>::call),
    reinterpret_cast<void*>(&Creation<6, decltype(&ID3D12Device8::CreatePlacedResource1)>::call),
    reinterpret_cast<void*>(&Creation<7, decltype(&ID3D12Device10::CreateCommittedResource3)>::call),
    reinterpret_cast<void*>(&Creation<8, decltype(&ID3D12Device10::CreatePlacedResource2)>::call),
    reinterpret_cast<void*>(&Creation<9, decltype(&ID3D12Device10::CreateReservedResource2)>::call)};

NativeSlot root_creation, rtv_creation, dsv_creation, descriptor_copy, descriptor_copy_simple, create_list, create_list1;
HRESULT STDMETHODCALLTYPE root_create(ID3D12Device* device, UINT node, const void* blob, SIZE_T bytes, REFIID iid, void** out) noexcept {
  using F = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
  const bool observe = !owned_depth && registry().ready && same_device(device);
  const OwnedWork guard;
  const auto hr = root_creation.forward<F>()(device, node, blob, bytes, iid, out);
  if (observe && SUCCEEDED(hr) && out && *out)
    observe_safely([&] {
      ID3D12RootSignature* native{};
      if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&native)))) {
        auto item = std::make_shared<Root>();
        item->layout = native_root_layout(blob, bytes);
        auto& r = registry();
        {
          const std::lock_guard lock(r.mutex);
          if (r.roots.size() < 16384 && r.next_id != UINT64_MAX) {
            item->id = ++r.next_id;
            r.roots[native] = item;
          }
        }
        if (!item->id || !attach(native, item))
          error("root_lifetime_registration_failed");
        native->Release();
      }
    });
  return hr;
}
void STDMETHODCALLTYPE rtv_create(ID3D12Device* device,
                                  ID3D12Resource* native,
                                  const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                  D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept {
  using F = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
  rtv_creation.forward<F>()(device, native, desc, handle);
  if (owned_depth || !registry().ready || !same_device(device))
    return;
  const OwnedWork guard;
  observe_safely([&] {
    if (native && !resource(native))
      observe_resource(device, native, source_state::Model::unknown);
    auto item = resource(native);
    auto& r = registry();
    View view;
    if (item && (!desc || desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D)) {
      view = {item, desc ? desc->Format : item->desc.Format, desc ? desc->Texture2D.MipSlice : 0};
    }
    const std::lock_guard lock(r.mutex);
    replace_view(r, handle.ptr, view);
    maybe_stop_live_backfill(r);
  });
}
void STDMETHODCALLTYPE dsv_create(ID3D12Device* device,
                                  ID3D12Resource* native,
                                  const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                  D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept {
  using F = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
  dsv_creation.forward<F>()(device, native, desc, handle);
  if (owned_depth || !registry().ready || !same_device(device))
    return;
  observe_safely([&] {
    DXGI_FORMAT format = desc ? desc->Format : native ? native->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
    auto& r = registry();
    const std::lock_guard lock(r.mutex);
    if (r.dsvs.size() < 16384)
      r.dsvs[handle.ptr] = format;
  });
}
void copy_descriptors(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
  auto& r = registry();
  if (count > 65536) {
    const std::lock_guard lock(r.mutex);
    clear_views(r);
    error("descriptor_copy_capacity");
    return;
  }
  const std::lock_guard lock(r.mutex);
  const UINT stride = type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? r.rtv_stride : r.dsv_stride;
  for (UINT i = 0; i < count; ++i) {
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV) {
      const auto found = r.rtvs.find(src.ptr);
      replace_view(r, dest.ptr, found != r.rtvs.end() ? found->second : View{});
    } else if (type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) {
      const auto found = r.dsvs.find(src.ptr);
      if (found != r.dsvs.end() && r.dsvs.size() < 16384)
        r.dsvs[dest.ptr] = found->second;
      else
        r.dsvs.erase(dest.ptr);
    }
    src.ptr += stride;
    dest.ptr += stride;
  }
}
void STDMETHODCALLTYPE descriptors_simple(ID3D12Device* device,
                                          UINT count,
                                          D3D12_CPU_DESCRIPTOR_HANDLE dest,
                                          D3D12_CPU_DESCRIPTOR_HANDLE src,
                                          D3D12_DESCRIPTOR_HEAP_TYPE type) noexcept {
  using F =
      void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
  descriptor_copy_simple.forward<F>()(device, count, dest, src, type);
  if (!owned_depth && registry().ready && (type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) &&
      same_device(device))
    observe_safely([&] { copy_descriptors(count, dest, src, type); });
}
void STDMETHODCALLTYPE descriptors(ID3D12Device* device,
                                   UINT nd,
                                   const D3D12_CPU_DESCRIPTOR_HANDLE* dest,
                                   const UINT* ds,
                                   UINT ns,
                                   const D3D12_CPU_DESCRIPTOR_HANDLE* src,
                                   const UINT* ss,
                                   D3D12_DESCRIPTOR_HEAP_TYPE type) noexcept {
  using F = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                     const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
  descriptor_copy.forward<F>()(device, nd, dest, ds, ns, src, ss, type);
  if (owned_depth || !registry().ready || (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV) ||
      !same_device(device))
    return;
  observe_safely([&] {
    auto& r = registry();
    if (nd > 4096 || ns > 4096 || !dest || !src) {
      const std::lock_guard lock(r.mutex);
      clear_views(r);
      r.dsvs.clear();
      error("descriptor_ranges_refused");
      return;
    }
    const UINT stride = type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? r.rtv_stride : r.dsv_stride;
    UINT di = 0, si = 0, dp = 0, sp = 0, total = 0;
    while (di < nd && si < ns) {
      const UINT dcount = ds ? ds[di] : 1, scount = ss ? ss[si] : 1;
      if (dp == dcount) {
        ++di;
        dp = 0;
        continue;
      }
      if (sp == scount) {
        ++si;
        sp = 0;
        continue;
      }
      const UINT n = std::min(dcount - dp, scount - sp);
      if (n > 65536 - total) {
        const std::lock_guard lock(r.mutex);
        clear_views(r);
        r.dsvs.clear();
        error("descriptor_ranges_overflow");
        return;
      }
      copy_descriptors(n, {dest[di].ptr + SIZE_T{dp} * stride}, {src[si].ptr + SIZE_T{sp} * stride}, type);
      dp += n;
      sp += n;
      total += n;
    }
  });
}
HRESULT STDMETHODCALLTYPE list_create(ID3D12Device* device,
                                      UINT node,
                                      D3D12_COMMAND_LIST_TYPE type,
                                      ID3D12CommandAllocator* allocator,
                                      ID3D12PipelineState* pso,
                                      REFIID iid,
                                      void** out) noexcept {
  using F = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID,
                                        void**);
  const bool observe = !owned_depth;
  const OwnedWork guard;
  const auto hr = create_list.forward<F>()(device, node, type, allocator, pso, iid, out);
  if (observe && SUCCEEDED(hr) && out && *out && type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
    ID3D12GraphicsCommandList* native{};
    if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&native)))) {
      observe_safely([&] {
        if (auto item = ensure_list(native, true))
          item->graphics.bind_pipeline(pso);
      });
      native->Release();
    }
  }
  return hr;
}
HRESULT STDMETHODCALLTYPE list_create1(ID3D12Device4* device,
                                       UINT node,
                                       D3D12_COMMAND_LIST_TYPE type,
                                       D3D12_COMMAND_LIST_FLAGS flags,
                                       REFIID iid,
                                       void** out) noexcept {
  using F = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
  const bool observe = !owned_depth;
  const OwnedWork guard;
  const auto hr = create_list1.forward<F>()(device, node, type, flags, iid, out);
  if (observe && SUCCEEDED(hr) && out && *out && type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
    ID3D12GraphicsCommandList* native{};
    if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&native)))) {
      observe_safely([&] { ensure_list(native); });
      native->Release();  // CreateCommandList1 creates a closed list; actual Reset admits it.
    }
  }
  return hr;
}
NativeSlot list_reset, list_close;
// The captured forward must be a native runtime endpoint: no downstream
// add-on may append instrument draws after we deliberately leave our state set.
bool native_close_endpoint(const void* address) noexcept {
  HMODULE owner{};
  if (!image_region(address, true) ||
      !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &owner))
    return false;
  return owner == GetModuleHandleW(L"D3D12Core.dll") || owner == GetModuleHandleW(L"d3d12.dll") ||
         owner == GetModuleHandleW(L"D3D12SDKLayers.dll");
}
HRESULT STDMETHODCALLTYPE close(ID3D12GraphicsCommandList* native) noexcept {
  using F = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
  if (!owned_depth && registry().ready) {
    const OwnedWork guard;
    clear_live_bind(native);
    observe_safely([&] {
      if (auto item = find_list(native); item && item->ready && !item->closing) {
        item->closing = true;
        stage_pfd(native, item->id);
        drain_pfds(native, item->id, true);
        // Success or failure, no second Close may append another stamp. The
        // capture manager retains submission/reexecution leases independently.
        item->pfd_dirty = false;
        item->pending_pfds = {};
        item->pending_rt = {};
        item->ready = false;
        item->graphics.invalidate("native_recording_closed");
      }
    });
  }
  const auto result = list_close.forward<F>()(native);
  if (!owned_depth && registry().ready) {
    const OwnedWork guard;
    if (auto item = find_list(native)) {
      item->submission_proof.close(item->recording, SUCCEEDED(result));
      item->closed_recording.store(SUCCEEDED(result) ? item->recording.load() : 0, std::memory_order_release);
    }
  }
  return result;
}
HRESULT STDMETHODCALLTYPE reset(ID3D12GraphicsCommandList* native, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso) noexcept {
  using F = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
  if (owned_depth)
    return list_reset.forward<F>()(native, allocator, pso);
  const OwnedWork guard;
  const auto observation_epoch = registry().observation_epoch.load(std::memory_order_acquire);
  std::shared_ptr<List> item;
  observe_safely([&] { item = ensure_list(native); });
  if (item)
    item->closed_recording.store(0, std::memory_order_release);
  const auto hr = list_reset.forward<F>()(native, allocator, pso);
  if (item) {
    // Demand cannot change between qualifying this native Reset and publishing
    // its PFD-state proof. Source state remains continuously observed separately.
    const std::lock_guard observation_lock(registry().observation_mutex);
    item->observation_epoch = observation_epoch == registry().observation_epoch.load(std::memory_order_acquire) ? observation_epoch : 0;
    item->pfd_dirty = false;
    item->pending_pfds = {};
    item->pending_rt = {};
    item->raw_rtvs = {};
    item->raw_dsv = {};
    item->raw_rtv_count = 0;
    item->raw_has_dsv = false;
    item->targets = {};
    item->count = 0;
    item->depth = DXGI_FORMAT_UNKNOWN;
    item->depth_known = true;
    item->ready = hr == S_OK && item->recording < UINT64_MAX;
    item->queries.reset(item->ready);
    item->copy_proof.reset(item->ready);
    item->raw_om_known = item->ready;
    if (item->ready) {
      item->closing = false;
      item->graphics.reset(++item->recording, true);
      item->submission_proof.reset(item->recording, true);
      item->graphics.bind_pipeline(pso);
      if (registry().live_backfill.load(std::memory_order_relaxed))
        registry().live_bind.clear(native);
      boundary::successful_reset(native, item->id);
      runtime::manager().successful_reset(native, item->id);
    } else {
      item->submission_proof.invalidate();
      item->graphics.invalidate("native_reset_failed");
      boundary::reset_failed(native, item->id);
    }
  }
  return hr;
}
struct Pipeline {
  static void apply(List& l, ID3D12PipelineState* p) { l.graphics.bind_pipeline(p); }
};
struct PipelineStateObject {
  static void apply(List& l, ID3D12StateObject*) { l.graphics.invalidate("state_object_binding_not_restorable"); }
};
struct ClearState {
  static void apply(List& l, ID3D12PipelineState* p) {
    ++registry().clear_states;
    l.copy_proof.clear_state();
    // ClearState unbinds the targets and graphics arguments immediately. A
    // deferred stamp must not reuse the pre-clear bindings at the next target
    // switch, nor restore the old pipeline over the caller's supplied PSO.
    l.pfd_dirty = false;
    l.pending_pfds = {};
    l.pending_rt = {};
    l.raw_rtvs = {};
    l.raw_dsv = {};
    l.raw_rtv_count = 0;
    l.raw_has_dsv = false;
    l.raw_om_known = l.ready;
    l.pfd_transition = false;
    l.targets = {};
    l.count = 0;
    l.depth = DXGI_FORMAT_UNKNOWN;
    l.depth_known = true;
    l.graphics.reset(l.recording, l.ready);
    l.graphics.bind_pipeline(p);
    if (registry().live_backfill.load(std::memory_order_relaxed))
      registry().live_bind.clear(l.native);
    // This is still the same recording. Keep boundary/capture invalidations,
    // render-pass scope and readiness; only an actual Reset can renew them.
  }
};
struct Heaps {
  static void apply(List& l, UINT n, ID3D12DescriptorHeap* const* p) { l.graphics.descriptor_heaps(n, p); }
};
struct GraphicsRoot {
  static void apply(List& l, ID3D12RootSignature* p) {
    auto& r = registry();
    const std::lock_guard lock(r.mutex);
    auto it = r.roots.find(p);
    if (p && (it == r.roots.end() || !it->second->alive) && r.roots.size() < 16384 && r.next_id != UINT64_MAX &&
        same_native_device(p, r.device)) {
      auto item = std::make_shared<Root>();
      item->id = ++r.next_id;
      r.roots[p] = item;
      if (!attach(p, item)) {
        item->retire();
        error("existing_root_lifetime_registration_failed");
      }
      it = r.roots.find(p);
    }
    if (it != r.roots.end() && it->second->alive)
      l.graphics.bind_observed_root(p, it->second->id);
    else
      l.graphics.bind_observed_root(nullptr, 0);
  }
};
struct Table {
  static void apply(List& l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) { l.graphics.table(i, h.ptr); }
};
struct Constant {
  static void apply(List& l, UINT i, UINT v, UINT offset) { l.graphics.constants(i, offset, 1, &v); }
};
struct Constants {
  static void apply(List& l, UINT i, UINT n, const void* p, UINT offset) { l.graphics.constants(i, offset, n, p); }
};
template <PfdRootKind Kind>
struct Address {
  static void apply(List& l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS p) { l.graphics.descriptor(i, Kind, p); }
};
struct Topology {
  static void apply(List& l, D3D12_PRIMITIVE_TOPOLOGY t) { l.graphics.topology(t); }
};
struct Viewports {
  static void apply(List& l, UINT n, const D3D12_VIEWPORT* p) { l.graphics.viewports(0, n, p); }
};
struct Scissors {
  static void apply(List& l, UINT n, const D3D12_RECT* p) { l.graphics.scissors(0, n, p); }
};
struct Targets {
  static void record(List& l,
                     UINT count,
                     const D3D12_CPU_DESCRIPTOR_HANDLE* handles,
                     BOOL contiguous,
                     const D3D12_CPU_DESCRIPTOR_HANDLE* depth,
                     bool snapshots) {
    auto& r = registry();
    const std::lock_guard lock(r.mutex);
    l.pfd_dirty = false;
    l.pfd_transition = false;
    l.targets = {};
    l.count = 0;
    l.depth = DXGI_FORMAT_UNKNOWN;
    l.depth_known = depth == nullptr;
    l.raw_om_known = false;
    l.raw_rtvs = {};
    l.raw_rtv_count = 0;
    l.raw_has_dsv = depth != nullptr;
    l.raw_dsv = {};
    if (depth) {
      const auto found = r.dsvs.find(depth->ptr);
      if (found != r.dsvs.end()) {
        l.depth = found->second;
        l.depth_known = true;
      }
    }
    if (count > 8 || (count && !handles) || (contiguous && count && handles[0].ptr > SIZE_MAX - SIZE_T{count - 1} * r.rtv_stride))
      return;
    bool relevant = false;
    for (unsigned side = 0; side < 2; ++side)
      relevant |= l.pending_rt[side];
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> input{};
    l.count = count;
    for (UINT i = 0; i < count; ++i) {
      const auto handle = contiguous ? handles[0].ptr + SIZE_T{i} * r.rtv_stride : handles[i].ptr;
      if (!handle)
        return;
      input[i] = {handle};
      const auto it = r.rtvs.find(handle);
      if (it != r.rtvs.end()) {
        l.targets[i] = it->second;
        l.targets[i].rtv = handle;
        if (it->second.resource && r.routes.matches(it->second.resource->id, r.active_mask | r.calibration_mask))
          relevant = true;
      }
    }
    if (r.live_backfill.load(std::memory_order_relaxed)) {
      if (count == 1 && !l.targets[0].resource && input[0].ptr)
        relevant |= bind_live_rtv(r, l, input[0].ptr);
      else
        r.live_bind.clear(l.native);
    }
    if (!snapshots || !relevant || !recording_observed(l))
      return;
    // Non-shader-visible input descriptors may be reused immediately after the
    // native OM call. Snapshot their contents BEFORE forwarding, never retain
    // only their borrowed CPU handle values for a later query-end restoration.
    if (!l.snapshot_rtvs) {
      D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 10, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
      if (FAILED(r.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&l.snapshot_rtvs))))
        return;
    }
    if (!l.snapshot_dsvs) {
      D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
      if (FAILED(r.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&l.snapshot_dsvs))))
        return;
    }
    const auto base = l.snapshot_rtvs->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < count; ++i) {
      l.raw_rtvs[i] = {base.ptr + SIZE_T{i} * r.rtv_stride};
      r.device->CopyDescriptorsSimple(1, l.raw_rtvs[i], input[i], D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    if (depth) {
      l.raw_dsv = l.snapshot_dsvs->GetCPUDescriptorHandleForHeapStart();
      r.device->CopyDescriptorsSimple(1, l.raw_dsv, *depth, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }
    l.raw_rtv_count = count;
    l.raw_om_known = true;
  }
  static void before(List& l,
                     UINT count,
                     const D3D12_CPU_DESCRIPTOR_HANDLE* handles,
                     BOOL contiguous,
                     const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    flush_pfd(l.native, l.id);
    record(l, count, handles, contiguous, depth, true);
  }
  static void apply(List&, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*) {}
};
void pass_targets(void*,
                  ID3D12GraphicsCommandList* native,
                  std::uint64_t id,
                  UINT count,
                  const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets,
                  const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth) noexcept {
  auto item = find_list(native);
  if (!item || item->id != id)
    return;
  item->queries.invalidate();
  item->pending_rt = {};
  flush_pfd(native, id, false);
  if (count > 8 || (count && !targets)) {
    item->pfd_dirty = false;
    item->targets = {};
    item->count = 0;
    item->depth_known = false;
    return;
  }
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> handles{};
  for (UINT i = 0; i < count; ++i)
    handles[i] = targets[i].cpuDescriptor;
  item->queries.invalidate();
  item->pending_rt = {};
  Targets::record(*item, count, handles.data(), FALSE, depth ? &depth->cpuDescriptor : nullptr, false);
}
struct SamplePositions {
  static void apply(List& l, UINT samples, UINT pixels, D3D12_SAMPLE_POSITION* positions) {
    ++registry().sample_position_calls;
    l.graphics.sample_positions(static_cast<ID3D12GraphicsCommandList1*>(l.native), samples, pixels, positions);
  }
};
struct DynamicDepthBias {
  static void apply(List& l, FLOAT bias, FLOAT clamp, FLOAT slope) {
    ++registry().dynamic_depth_bias_calls;
    l.graphics.depth_bias(static_cast<d3d12_extended::CommandList9*>(l.native), bias, clamp, slope);
  }
};
struct DynamicStripCut {
  static void apply(List& l, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE value) {
    ++registry().dynamic_strip_cut_calls;
    l.graphics.strip_cut(static_cast<d3d12_extended::CommandList9*>(l.native), value);
  }
};
struct QueryBegin {
  static void before(List& l, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
    l.submission_proof.state_disjoint_work(52);
    l.queries.begin(reinterpret_cast<std::uint64_t>(heap), static_cast<std::uint32_t>(type), index);
  }
  static void apply(List&, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) {}
};
struct QueryEnd {
  static void apply(List& l, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
    l.submission_proof.state_disjoint_work(53);
    l.queries.end(reinterpret_cast<std::uint64_t>(heap), static_cast<std::uint32_t>(type), index);
    if (l.queries.known_empty())
      drain_pfds(l.native, l.id);
  }
};
struct GpuWork {
  template <class... Args>
  static void apply(List& l, Args...) {
    l.submission_proof.gpu_work();
  }
};
struct StateDisjointGpuWork {};
struct Unsupported {
  template <class... Args>
  static void apply(List& l, Args...) {
    l.copy_proof.invalidate();
    l.submission_proof.invalidate(PfdSubmissionProof::Refusal::unsupported_command);
    l.graphics.invalidate("unsupported_native_work");
    boundary::invalidate_recording(l.native, l.id);
  }
};
struct Predication {
  static void apply(List& list, ID3D12Resource* buffer, UINT64, D3D12_PREDICATION_OP) {
    // NULL disables conditional execution; D3D11On12 emits it before an
    // ordinary capture copy. It neither executes work nor changes resource
    // states. Never revive a recording invalidated by an earlier real predicate.
    if (buffer)
      Unsupported::apply(list);
  }
};
// Every signature is derived from the pinned Windows COM declaration.
// These hooks observe, forward exactly once, and never alter application inputs.
template <unsigned Slot, class Signature, class Action>
struct StateHook;
template <unsigned Slot, class C, class... Args, class Action>
struct StateHook<Slot, void (STDMETHODCALLTYPE C::*)(Args...), Action> {
  using F = void(STDMETHODCALLTYPE*)(C*, Args...);
  static inline NativeSlot slot;
  // The native active-pass table has distinct cells even where the target
  // functions are identical. Retain a second, pinned cell for that exact table;
  // Each table forwards its own immutable original, including if another hook
  // changes a cell between the admission read and the installation CAS.
  static inline NativeSlot active_slot;
  static void invoke(F forward, C* native, Args... args) noexcept {
    if (owned_depth || !registry().ready) {
      forward(native, args...);
      return;
    }
    // Target bindings still feed aircraft autodetection, and ClearState must
    // clear those bindings. Other state can be omitted only because an epoch
    // mismatch forbids injection until a real, fully observed native Reset.
    if constexpr (!std::is_same_v<Action, Targets> && !std::is_same_v<Action, ClearState> && !std::is_same_v<Action, Unsupported> &&
                  !std::is_same_v<Action, Predication> && !std::is_same_v<Action, GpuWork> &&
                  !std::is_same_v<Action, StateDisjointGpuWork> && !std::is_same_v<Action, QueryBegin> &&
                  !std::is_same_v<Action, QueryEnd>) {
      if (!observation_enabled()) {
        auto& r = registry();
        if (r.diagnostics_enabled.load(std::memory_order_relaxed))
          r.idle_state_bypasses.fetch_add(1, std::memory_order_relaxed);
        const OwnedWork guard;
        forward(native, args...);
        return;
      }
    }
    // A runtime implementation can re-enter a patched setter while forwarding
    // the caller's operation. Keep one guard across forwarding and observation,
    // just as Reset does, so only the outer call updates the tracked state.
    const OwnedWork guard;
    if constexpr (requires(List& l) { Action::before(l, args...); }) {
      observe_safely([&] {
        if (auto item = find_list(reinterpret_cast<ID3D12GraphicsCommandList*>(native)))
          Action::before(*item, args...);
      });
    }
    forward(native, args...);
    if (!registry().ready)
      return;
    observe_safely([&] {
      if (auto item = ensure_list(reinterpret_cast<ID3D12GraphicsCommandList*>(native))) {
        if constexpr (std::is_same_v<Action, StateDisjointGpuWork>)
          item->submission_proof.state_disjoint_work(Slot);
        else if constexpr (std::is_same_v<Action, GpuWork>)
          item->submission_proof.gpu_work(Slot);
        else
          Action::apply(*item, args...);
      }
    });
  }
  static void STDMETHODCALLTYPE call(C* native, Args... args) noexcept { invoke(slot.forward<F>(), native, args...); }
  static void STDMETHODCALLTYPE active_call(C* native, Args... args) noexcept { invoke(active_slot.forward<F>(), native, args...); }
  static bool install(ID3D12GraphicsCommandList* list, bool active = false) {
    if (!active)
      return slot.install(list, Slot, reinterpret_cast<void*>(&call));
    auto** address = *reinterpret_cast<void***>(list) + Slot;
    if (address == slot.address)
      return *address == reinterpret_cast<void*>(&call);
    if (active_slot.address)
      return active_slot.install(list, Slot, reinterpret_cast<void*>(&active_call)) &&
             active_slot.original.load(std::memory_order_acquire) == slot.original.load(std::memory_order_acquire);
    if (!slot.address || *address != slot.original.load(std::memory_order_acquire))
      return false;
    // If installation observed a changed chain, refuse graphics admission.
    // The installed active trampoline still forwards its exact captured chain;
    // no transient caller can be sent to a different table's implementation.
    return active_slot.install(list, Slot, reinterpret_cast<void*>(&active_call)) &&
           active_slot.original.load(std::memory_order_acquire) == slot.original.load(std::memory_order_acquire);
  }
};
#define STATE(Slot, Method, Action) StateHook<Slot, decltype(&ID3D12GraphicsCommandList::Method), Action>
bool hook_state(ID3D12GraphicsCommandList* list, bool active = false) {
  bool ok = true;
  if (!active) {
    ok = list_reset.install(list, 10, reinterpret_cast<void*>(&reset));
    ok &= list_close.install(list, 9, reinterpret_cast<void*>(&close));
  }
  ok &= STATE(11, ClearState, ClearState)::install(list, active);
  ok &= STATE(25, SetPipelineState, Pipeline)::install(list, active);
  ok &= STATE(28, SetDescriptorHeaps, Heaps)::install(list, active);
  ok &= STATE(30, SetGraphicsRootSignature, GraphicsRoot)::install(list, active);
  ok &= STATE(32, SetGraphicsRootDescriptorTable, Table)::install(list, active);
  ok &= STATE(34, SetGraphicsRoot32BitConstant, Constant)::install(list, active);
  ok &= STATE(36, SetGraphicsRoot32BitConstants, Constants)::install(list, active);
  ok &= STATE(38, SetGraphicsRootConstantBufferView, Address<PfdRootKind::cbv>)::install(list, active);
  ok &= STATE(40, SetGraphicsRootShaderResourceView, Address<PfdRootKind::srv>)::install(list, active);
  ok &= STATE(42, SetGraphicsRootUnorderedAccessView, Address<PfdRootKind::uav>)::install(list, active);
  ok &= STATE(20, IASetPrimitiveTopology, Topology)::install(list, active);
  ok &= STATE(21, RSSetViewports, Viewports)::install(list, active);
  ok &= STATE(22, RSSetScissorRects, Scissors)::install(list, active);
  ok &= STATE(46, OMSetRenderTargets, Targets)::install(list, active);
  ok &= STATE(27, ExecuteBundle, Unsupported)::install(list, active);
  ok &= STATE(59, ExecuteIndirect, Unsupported)::install(list, active);
  ok &= STATE(55, SetPredication, Predication)::install(list, active);
  ok &= STATE(52, BeginQuery, QueryBegin)::install(list, active);
  ok &= STATE(53, EndQuery, QueryEnd)::install(list, active);
  ok &= STATE(51, DiscardResource, Unsupported)::install(list, active);
  // Pass entry poisons queries and copy proof until actual Reset. No fallback
  // can consume dynamic PSO state in that recording; keep the runtime's distinct
  // active-pass implementations for these optional newer setters untouched.
  if (active)
    return ok;
  ok &= STATE(14, Dispatch, StateDisjointGpuWork)::install(list);
  ok &= STATE(15, CopyBufferRegion, StateDisjointGpuWork)::install(list);
  ok &= STATE(18, CopyTiles, StateDisjointGpuWork)::install(list);
  ok &= STATE(19, ResolveSubresource, StateDisjointGpuWork)::install(list);
  ok &= STATE(47, ClearDepthStencilView, StateDisjointGpuWork)::install(list);
  ok &= STATE(48, ClearRenderTargetView, GpuWork)::install(list);
  ok &= STATE(49, ClearUnorderedAccessViewUint, StateDisjointGpuWork)::install(list);
  ok &= STATE(50, ClearUnorderedAccessViewFloat, StateDisjointGpuWork)::install(list);
  ok &= STATE(54, ResolveQueryData, StateDisjointGpuWork)::install(list);
  ID3D12GraphicsCommandList1* sample_list{};
  const auto sample_interface = list->QueryInterface(IID_PPV_ARGS(&sample_list));
  if (sample_interface != E_NOINTERFACE) {
    if (FAILED(sample_interface) || !sample_list || static_cast<ID3D12GraphicsCommandList*>(sample_list) != list)
      ok = false;
    else {
      ok &= StateHook<63, decltype(&ID3D12GraphicsCommandList1::SetSamplePositions), SamplePositions>::install(list);
      ok &= StateHook<60, decltype(&ID3D12GraphicsCommandList1::AtomicCopyBufferUINT), GpuWork>::install(list);
      ok &= StateHook<61, decltype(&ID3D12GraphicsCommandList1::AtomicCopyBufferUINT64), GpuWork>::install(list);
      ok &= StateHook<64, decltype(&ID3D12GraphicsCommandList1::ResolveSubresourceRegion), StateDisjointGpuWork>::install(list);
    }
  }
  if (sample_list)
    sample_list->Release();
  ID3D12GraphicsCommandList2* immediate_list{};
  const auto immediate_interface = list->QueryInterface(IID_PPV_ARGS(&immediate_list));
  if (immediate_interface != E_NOINTERFACE) {
    if (FAILED(immediate_interface) || !immediate_list || static_cast<ID3D12GraphicsCommandList*>(immediate_list) != list)
      ok = false;
    else
      ok &= StateHook<66, decltype(&ID3D12GraphicsCommandList2::WriteBufferImmediate), StateDisjointGpuWork>::install(list);
  }
  if (immediate_list)
    immediate_list->Release();
  ID3D12GraphicsCommandList4* state_object_list{};
  const auto state_object_interface = list->QueryInterface(IID_PPV_ARGS(&state_object_list));
  if (state_object_interface != E_NOINTERFACE) {
    if (FAILED(state_object_interface) || !state_object_list || static_cast<ID3D12GraphicsCommandList*>(state_object_list) != list) {
      ok = false;
    } else {
      ok &= StateHook<75, decltype(&ID3D12GraphicsCommandList4::SetPipelineState1), PipelineStateObject>::install(list, active);
      ok &= StateHook<67, decltype(&ID3D12GraphicsCommandList3::SetProtectedResourceSession), Unsupported>::install(list);
      ok &= StateHook<70, decltype(&ID3D12GraphicsCommandList4::InitializeMetaCommand), Unsupported>::install(list);
      ok &= StateHook<71, decltype(&ID3D12GraphicsCommandList4::ExecuteMetaCommand), Unsupported>::install(list);
      ok &= StateHook<72, decltype(&ID3D12GraphicsCommandList4::BuildRaytracingAccelerationStructure), StateDisjointGpuWork>::install(list);
      ok &= StateHook<73, decltype(&ID3D12GraphicsCommandList4::EmitRaytracingAccelerationStructurePostbuildInfo),
                      StateDisjointGpuWork>::install(list);
      ok &= StateHook<74, decltype(&ID3D12GraphicsCommandList4::CopyRaytracingAccelerationStructure), StateDisjointGpuWork>::install(list);
      ok &= StateHook<76, decltype(&ID3D12GraphicsCommandList4::DispatchRays), StateDisjointGpuWork>::install(list);
    }
  }
  if (state_object_list)
    state_object_list->Release();
  ID3D12GraphicsCommandList6* mesh_list{};
  const auto mesh_interface = list->QueryInterface(IID_PPV_ARGS(&mesh_list));
  if (mesh_interface != E_NOINTERFACE) {
    if (FAILED(mesh_interface) || !mesh_list || static_cast<ID3D12GraphicsCommandList*>(mesh_list) != list)
      ok = false;
    else
      ok &= StateHook<79, decltype(&ID3D12GraphicsCommandList6::DispatchMesh), GpuWork>::install(list);
  }
  if (mesh_list)
    mesh_list->Release();
  d3d12_extended::CommandList9* extended{};
  const auto extension = list->QueryInterface(d3d12_extended::CommandList9Id, reinterpret_cast<void**>(&extended));
  if (extension != E_NOINTERFACE) {
    // Only inspect expanded slots after public QI proves this identical native
    // interface. Older objects without List9 cannot expose these setters.
    if (FAILED(extension) || !extended || static_cast<ID3D12GraphicsCommandList*>(extended) != list) {
      ok = false;
    } else {
      ok &= StateHook<d3d12_extended::DepthBiasSlot, decltype(&d3d12_extended::CommandList9::RSSetDepthBias), DynamicDepthBias>::install(
          list, active);
      ok &= StateHook<d3d12_extended::StripCutSlot, decltype(&d3d12_extended::CommandList9::IASetIndexBufferStripCutValue),
                      DynamicStripCut>::install(list, active);
    }
  }
  if (extended)
    extended->Release();
  return ok;
}
#undef STATE
bool same_interface(ID3D12Device* device, REFIID iid) {
  IUnknown* p{};
  const bool ok = SUCCEEDED(device->QueryInterface(iid, reinterpret_cast<void**>(&p))) && p == device;
  if (p)
    p->Release();
  return ok;
}
}  // namespace

bool initialize_graphics(IUnknown* reported) noexcept {
  const OwnedWork guard;
  ID3D12Device* device{};
  if (!resolve_native_device(reported, &device)) {
    error("native_device_resolution_failed");
    return false;
  }
  const auto release = [](ID3D12Device* p) { p->Release(); };
  const std::unique_ptr<ID3D12Device, decltype(release)> reference(device, release);
  // Hook below cooperating proxies: translated descriptors, unwrapped queue
  // submissions, and Close after downstream add-on callbacks. The existing
  // native Close endpoint check remains mandatory for final writes.
  auto& r = registry();
  if (r.device)
    return r.device == device && r.ready;
  if (!device || !same_interface(device, __uuidof(ID3D12Device10))) {
    error("native_device10_required");
    return false;
  }
  r.device = device;
  device->AddRef();
  r.key = reinterpret_cast<std::uint64_t>(device);
  if (!scene_handoff().register_device(r.key) || !runtime::init_device(r.key, device)) {
    error("native_device_registration_failed");
    return false;
  }
  r.rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  r.dsv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  ID3D12CommandAllocator* allocator{};
  ID3D12GraphicsCommandList* list{};
  ID3D12CommandQueue* queue{};
  D3D12_COMMAND_QUEUE_DESC q{};
  q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator))) ||
      FAILED(device->CreateCommandList(0, q.Type, allocator, nullptr, IID_PPV_ARGS(&list))) ||
      FAILED(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)))) {
    error("bootstrap_objects_failed");
    return false;
  }
  // Bootstrap objects, modules and the device remain pinned. No runtime unload.
  bool ok = hook_state(list);
  r.close_forward_verified = native_close_endpoint(list_close.original.load(std::memory_order_acquire));
  const auto base = boundary::register_list(list, ++r.next_id, Boundaries);
  ok &= base.ready && base.protection_restored;
  // Discover the runtime's real active-pass table on this owned, empty list,
  // before admitting application recordings. No simulator resource is bound.
  // The state setters are hooked in both tables; native Begin/End and all
  // existing pass/suspend/alias capture guards remain owned by the boundary
  // observer. A different forwarding implementation is refused, never guessed.
  ID3D12GraphicsCommandList4* pass_list{};
  const bool same_pass_interface = SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&pass_list))) && pass_list == list;
  if (base.ready && base.protection_restored && same_pass_interface) {
    pass_list->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    ok &= hook_state(list, true);
    pass_list->EndRenderPass();
  } else {
    ok = false;
  }
  if (pass_list)
    pass_list->Release();
  boundary::unregister_list(list, r.next_id);
  list->Close();
  ok &= runtime::init_queue(r.key, queue);
  ok &= queue_hook::set_unknown_queue_observer(unknown_queue);
  ok &= runtime::manager().set_unknown_list_observer(unknown_list, nullptr);
  ok &= runtime::manager().set_display_submission_planner(plan_display_submission, nullptr);
  for (unsigned i = 0; i < creations.size(); ++i)
    ok &= creations[i].install(device, CreationSlots[i], CreationWrappers[i]);
  ok &= root_creation.install(device, 16, reinterpret_cast<void*>(&root_create));
  ok &= rtv_creation.install(device, 20, reinterpret_cast<void*>(&rtv_create));
  ok &= dsv_creation.install(device, 21, reinterpret_cast<void*>(&dsv_create));
  ok &= descriptor_copy.install(device, 23, reinterpret_cast<void*>(&descriptors));
  ok &= descriptor_copy_simple.install(device, 24, reinterpret_cast<void*>(&descriptors_simple));
  ok &= create_list.install(device, 12, reinterpret_cast<void*>(&list_create));
  ok &= create_list1.install(device, 51, reinterpret_cast<void*>(&list_create1));
  r.ready = ok;
  r.error = ok ? "native_graphics_ready" : "native_hook_installation_failed";
  if (ok) {
    // Late attachment observes the application's existing native D3D12 work.
    // Do not create an unrelated D3D11On12 device or suspend application threads
    // to install instruction patches while the simulator is rendering.
    r.backfill_started_ms = 0;
    r.backfill_inventory_ms = 0;
    r.live_backfill.store(true, std::memory_order_relaxed);
  } else
    ++r.failures;
  return ok;
}
bool initialize_graphics() noexcept {
  const OwnedWork guard;
  ID3D12Device* reported{};
  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&reported)))) {
    error("d3d12_device_unavailable");
    return false;
  }
  const bool ok = initialize_graphics(reported);
  reported->Release();
  return ok;
}
bool graphics_ready() noexcept {
  return registry().ready.load(std::memory_order_acquire);
}
void set_graphics_observation_demand(bool enabled) noexcept {
  auto& r = registry();
  if (observation_enabled() == enabled)
    return;
  const std::lock_guard lock(r.observation_mutex);
  const auto previous = r.observation_epoch.load(std::memory_order_acquire);
  if (!previous || bool(previous & 1u) == enabled)
    return;
  // Suppress new capture insertion without discarding the continuous layout
  // model or previously recorded replay/consumer obligations. The epoch below
  // invalidates only PFD state omitted while idle, until a real native Reset.
  runtime::manager().set_capture_enabled(enabled);
  r.observation_epoch.store(previous == UINT64_MAX ? 0 : previous + 1, std::memory_order_release);
  if (!enabled)
    r.observation_invalidations.fetch_add(1, std::memory_order_relaxed);
}
void set_graphics_diagnostics_enabled(bool enabled) noexcept {
  registry().diagnostics_enabled.store(enabled, std::memory_order_relaxed);
}
GraphicsStatus graphics_status() noexcept {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  GraphicsStatus result{r.ready,
                        r.key,
                        r.resources.size(),
                        r.lists.size(),
                        r.draws,
                        r.failures,
                        r.clear_states,
                        r.error,
                        r.selected_draws,
                        r.selected_rt_metadata,
                        r.selected_rt_callbacks,
                        r.selected_pending_matches,
                        r.selected_view_resolved,
                        r.selected_view_rejected,
                        r.copy_attempts,
                        r.copy_rejected,
                        r.copy_error};
  result.target_detection = r.detector.snapshot().status;
  for (unsigned i = 0; i < result.selected_exit_scopes.size(); ++i)
    result.selected_exit_scopes[i] = r.selected_exit_scopes[i].load(std::memory_order_relaxed);
  result.calibration_clears = r.calibration_clears.load(std::memory_order_relaxed);
  result.selected_exit_base = r.selected_exit_base.load(std::memory_order_relaxed);
  result.selected_exit_nonbase = r.selected_exit_nonbase.load(std::memory_order_relaxed);
  result.selected_exit_split = r.selected_exit_split.load(std::memory_order_relaxed);
  result.fallback_attempts = r.fallback_attempts.load();
  result.fallback_stamps = r.fallback_stamps.load();
  result.fallback_query_refused = r.fallback_query_refused.load();
  result.fallback_state_refused = r.fallback_state_refused.load();
  result.preferred_copy_attempts = r.preferred_copy_attempts.load();
  result.preferred_copy_stamps = r.preferred_copy_stamps.load();
  result.preferred_copy_no_proof = r.preferred_copy_no_proof.load();
  result.preferred_copy_reason = r.preferred_copy_reason.load();
  result.sample_position_calls = r.sample_position_calls.load();
  result.recording_end_draws = r.recording_end_draws.load();
  result.shader_deferred = r.shader_deferred.load();
  result.close_forward_refused = r.close_forward_refused.load();
  result.dynamic_depth_bias_calls = r.dynamic_depth_bias_calls.load();
  result.dynamic_strip_cut_calls = r.dynamic_strip_cut_calls.load();
  result.observing = observation_enabled();
  result.observation_epoch = r.observation_epoch.load(std::memory_order_relaxed);
  result.observation_invalidations = r.observation_invalidations.load(std::memory_order_relaxed);
  result.list_lookup_calls = r.list_lookup_calls.load(std::memory_order_relaxed);
  result.list_cache_hits = r.list_cache_hits.load(std::memory_order_relaxed);
  result.list_registry_lookups = r.list_registry_lookups.load(std::memory_order_relaxed);
  result.idle_state_bypasses = r.idle_state_bypasses.load(std::memory_order_relaxed);
  result.idle_callback_bypasses = r.idle_callback_bypasses.load(std::memory_order_relaxed);
  result.queue_patch_plans = r.queue_patch_plans.load(std::memory_order_relaxed);
  result.queue_close_verified = r.close_forward_verified;
  for (unsigned i = 0; i < result.queue_outcomes.size(); ++i)
    result.queue_outcomes[i] = r.queue_outcomes[i].load(std::memory_order_relaxed);
  for (unsigned i = 0; i < result.queue_proof_refusals.size(); ++i)
    result.queue_proof_refusals[i] = r.queue_proof_refusals[i].load(std::memory_order_relaxed);
  result.queue_last_proof_flags = r.queue_last_proof_flags.load(std::memory_order_relaxed);
  for (unsigned i = 0; i < result.queue_prefix_blockers.size(); ++i)
    result.queue_prefix_blockers[i] = r.queue_prefix_blockers[i].load(std::memory_order_relaxed);
  return result;
}
static std::vector<PfdTargetObservation> pfd_inventory_locked(Registry& r) {
  std::vector<PfdTargetObservation> result;
  for (const auto& [p, item] : r.resources) {
    (void)p;
    if (item->alive && profiles::matches_display(*r.profile, static_cast<UINT>(item->desc.Width), item->desc.Height, item->desc.MipLevels,
                                                 static_cast<UINT>(item->desc.Format)))
      result.push_back({item->id, item->draws, static_cast<UINT>(item->desc.Width), item->desc.Height, item->desc.MipLevels,
                        static_cast<UINT>(item->desc.Format), item->submission_activity.load()});
  }
  return result;
}
std::vector<PfdTargetObservation> pfd_inventory() {
  std::vector<PfdTargetObservation> result;
  {
    auto& r = registry();
    const std::lock_guard lock(r.mutex);
    result = pfd_inventory_locked(r);
  }
  // The numeric snapshot owns no native references; UI ranking does not need
  // the registry lock. Never truncate the inventory used by autodetection.
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.draws > b.draws; });
  return result;
}
void service_live_backfill(std::uint64_t now, std::size_t) noexcept {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  if (!r.live_backfill.load(std::memory_order_relaxed))
    return;
  // The registry, not the previous UI snapshot, decides whether the set exists.
  // Cockpit textures often already exist when the bridge attaches and are first
  // used after LiveBackfillAdmitMs. Closing the scan then leaves the dropdown empty.
  if (!backfill_resources_ready(r)) {
    if (!r.backfill_started_ms)
      r.backfill_started_ms = now ? now : 1;
    // LiveBackfillAdmitMs used to stop this scan. Textures first used after
    // that budget never reached the dropdown, so an incomplete set stays open.
    return;
  }
  maybe_stop_live_backfill(r);
  if (!r.live_backfill.load(std::memory_order_relaxed))
    return;
  if (!r.backfill_inventory_ms)
    r.backfill_inventory_ms = now ? now : 1;
  // Set present; keep associating RTVs. Do not freeze stamps or calibration
  // once the profile's displays are visible.
  if (now >= r.backfill_inventory_ms && now - r.backfill_inventory_ms >= LiveBackfillAssociateMs)
    r.live_backfill.store(false, std::memory_order_relaxed);
}
void service_display_patches() noexcept {
  auto& r = registry();
  runtime::QueuePatchConfig config;
  {
    const std::lock_guard lock(r.mutex);
    if (!r.ready)
      return;
    refresh_selected(r);
    config.generation = r.queue_patch_generation.load(std::memory_order_acquire);
    config.profile = r.profile->id;
    for (unsigned side = 0; side < 2; ++side) {
      const auto& target = r.selected_resources[side];
      if (!target || !display_item(r, *target))
        continue;
      View owned_encoding;
      owned_encoding.resource = target;
      owned_encoding.recovered = true;
      config.formats[side] = output_format(owned_encoding);
      if (config.formats[side] == DXGI_FORMAT_UNKNOWN)
        continue;
      config.camera_mask |= r.active_mask & (1u << side);
      config.calibration_mask |= r.calibration_mask & (1u << side);
    }
  }
  runtime::configure_queue_patches(r.key, config);
}
bool assign_targets(std::uint64_t left, std::uint64_t right) noexcept {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  bool l = !left, rr = !right;
  for (const auto& [p, item] : r.resources) {
    (void)p;
    if (!item->alive || !profiles::matches_display(*r.profile, static_cast<UINT>(item->desc.Width), item->desc.Height, item->desc.MipLevels,
                                                   static_cast<UINT>(item->desc.Format)))
      continue;
    l |= item->id == left;
    rr |= item->id == right;
  }
  if (!l || !rr)
    return false;
  const bool assigned = r.routes.select_explicit({left, right});
  if (assigned && live_display_rtvs(r) != live_display_resources(r))
    rearm_live_backfill(r);
  refresh_selected(r);
  return assigned;
}
void set_calibration(unsigned mask, unsigned budget) noexcept {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  r.calibration_mask = mask & 3u;
  refresh_selected(r);
  r.calibration_budget.set_limit(budget);
}
void set_target_mask(unsigned mask) noexcept {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  r.active_mask = mask & 3u;
  refresh_selected(r);
}
std::array<std::uint64_t, 2> target_ids() noexcept {
  auto& r = registry();
  const std::lock_guard lock(r.mutex);
  return r.routes.targets;
}
void reset_display_session() noexcept {
  auto& r = registry();
  // The same order is used by native Reset/list registration. Do not mutate
  // recording-local proofs from this control thread: atomically exclude old
  // recordings until an actual native Reset observes the new session.
  const std::lock_guard observation_lock(r.observation_mutex);
  runtime::manager().set_capture_enabled(false);
  const auto previous = r.observation_epoch.load(std::memory_order_acquire);
  const auto increment = (previous & 1u) ? 1u : 2u;
  const auto next = previous && previous <= UINT64_MAX - increment ? previous + increment : 0;
  r.observation_epoch.store(next, std::memory_order_release);
  r.session_recording_floor.store(next ? next : UINT64_MAX, std::memory_order_release);
  r.observation_invalidations.fetch_add(1, std::memory_order_relaxed);
  const std::lock_guard lock(r.mutex);
  r.active_mask = r.calibration_mask = 0;
  r.routes.reset();
  r.detector.reset();
  for (const auto& [native, item] : r.resources) {
    (void)native;
    item->draws.store(0, std::memory_order_relaxed);
    item->submission_activity.store(0, std::memory_order_relaxed);
  }
  for (auto i = r.rtvs.begin(); i != r.rtvs.end();) {
    if (i->second.recovered) {
      account_view(i->second, false);
      i = r.rtvs.erase(i);
    } else
      ++i;
  }
  rearm_live_backfill(r);
  // Even an already empty/off setup must invalidate its published patch.
  r.queue_patch_generation.fetch_add(1, std::memory_order_release);
  refresh_selected(r);
}
void set_aircraft_profile(std::uint32_t id) noexcept {
  const auto* profile = profiles::find(id);
  if (!profile)
    return;
  auto& r = registry();
  {
    const std::lock_guard lock(r.mutex);
    r.active_mask = r.calibration_mask = 0;
    // This entry point starts an explicit aircraft/profile session, including a
    // reload of the same adapter. Ordinary texture replacement uses forget().
    r.routes.reset();
    r.profile = profile;
    r.detector.configure(*profile);
    // Retry discovery from fresh activity and fresh late-bind evidence. Keep
    // creation-proven native identities/descriptors: discarding those would
    // turn an otherwise valid resident bridge into another blind late attach.
    for (const auto& [native, item] : r.resources) {
      (void)native;
      if (item->alive) {
        item->draws.store(0, std::memory_order_relaxed);
        item->submission_activity.store(0, std::memory_order_relaxed);
      }
    }
    std::erase_if(r.rtvs, [](const auto& entry) { return entry.second.recovered; });
    rearm_live_backfill(r);
    refresh_selected(r);
  }
  if (!runtime::set_patch_profile(r.key, id))
    error("private_patch_profile_failed");
}
void discover_pfds(std::uint64_t now) noexcept {
  observe_safely([&] {
    auto& r = registry();
    const std::lock_guard lock(r.mutex);
    for (auto i = r.resources.begin(); i != r.resources.end();) {
      if (!i->second->alive) {
        r.routes.forget(i->second->id);
        i = r.resources.erase(i);
      } else
        ++i;
    }
    std::erase_if(r.roots, [](const auto& p) { return !p.second->alive; });
    std::erase_if(r.lists, [](const auto& p) { return !p.second->alive; });
    for (auto i = r.rtvs.begin(); i != r.rtvs.end();) {
      if (!i->second.resource || !i->second.resource->alive) {
        account_view(i->second, false);
        i = r.rtvs.erase(i);
      } else
        ++i;
    }
    const bool ranked_group = r.profile->pfd_detection == profiles::PfdDetectionPolicy::ini_a380_allocation_group;
    if (now && (ranked_group || !r.routes.targets[0] || !r.routes.targets[1])) {
      // Take the full inventory after retirement cleanup, under the same
      // registry lock used for detector configuration and target assignment.
      // The detector sorts by incarnation itself. Avoid the unrelated UI
      // draw-count sort while holding the registry lock.
      auto inventory = pfd_inventory_locked(r);
      const auto& detection = r.detector.observe(inventory.data(), inventory.size(), now, r.pfd_inventory_complete.load());
      if (detection.valid)
        r.routes.adopt_detected(detection.targets);
      else if (ranked_group && detection.invalidates_targets)
        r.routes.forget_detected();
      if (!ranked_group && !r.routes.targets[0] && !r.routes.targets[1]) {
        if (const auto pair = dominant_activity_pair(*r.profile, inventory); pair[0])
          r.routes.adopt_detected(pair);
      } else if (ranked_group && (!r.routes.targets[0] || !r.routes.targets[1])) {
        // After incomplete→complete the detector reports ini_group_changed
        // (invalidates). Still adopt a filled idle eight so ready/calibration
        // are not stuck behind all-eight draw increments or a stale automatic
        // singleton. A live explicit selection remains the user's anchor.
        if (const auto pair = allocation_group_pair(*r.profile, inventory); pair[0]) {
          if (!r.routes.adopt_detected(pair) && (!r.routes.targets[0] || !r.routes.targets[1]))
            r.routes.replace_detected(pair);
        }
      }
    }
    refresh_selected(r);
  });
}
}  // namespace taxi_camera::standalone
