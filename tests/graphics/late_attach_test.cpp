// Exercise production late-attachment metadata without a simulator or GPU.
// Native addresses below are identity tokens and must never be dereferenced.
#include <array>
#include <cstdio>
#include <memory>
#include <thread>
#include "../../src/bridge/d3d12_bridge.cpp"

namespace {
namespace win = taxi_camera::standalone;
namespace profiles = taxi_camera::profiles;
unsigned checks{}, failures{};

void expect(const char* name, unsigned long long actual, unsigned long long expected) {
  ++checks;
  const bool passed = actual == expected;
  failures += !passed;
  if (!passed)
    std::fprintf(stderr, "FAIL %s: actual=%llu expected=%llu\n", name, actual, expected);
}

ID3D12Resource* resource_address(unsigned i) {
  return reinterpret_cast<ID3D12Resource*>(static_cast<std::uintptr_t>(0x10000 + 0x100 * i));
}
ID3D12GraphicsCommandList* list_address(unsigned i) {
  return reinterpret_cast<ID3D12GraphicsCommandList*>(static_cast<std::uintptr_t>(0x100000 + 0x100 * i));
}

std::shared_ptr<win::Resource> display(unsigned i) {
  auto item = std::make_shared<win::Resource>();
  item->native = resource_address(i);
  item->id = i + 1;
  item->desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  item->desc.Width = profiles::IniA380.width;
  item->desc.Height = profiles::IniA380.height;
  item->desc.DepthOrArraySize = item->desc.MipLevels = 1;
  item->desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
  item->desc.SampleDesc.Count = 1;
  item->desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  item->desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  return item;
}

void clear_fixture(win::Registry& r) {
  win::clear_views(r);
  r.resources.clear();
  r.routes.reset();
  r.selected_resources = {};
  r.active_mask = r.calibration_mask = 0;
  win::refresh_selected(r);
  r.profile = &profiles::A380;
  r.live_backfill = true;
  r.backfill_started_ms = r.backfill_inventory_ms = 0;
  r.backfill_full_window = false;
  r.observation_epoch = 2;  // CPU cases need metadata discovery, not graphics replay.
  for (auto& slot : r.seen_resources.slots)
    slot = nullptr;
  for (auto& slot : r.backfill_displays)
    slot = nullptr;
  r.live_bind.clear_all();
}

// Seed the post-QI metadata produced by observe_resource. Profile eligibility
// and descriptor recovery still go through the real production handlers.
void seed_observed_display(win::Registry& r, const std::shared_ptr<win::Resource>& item) {
  r.seen_resources.remember(item->native);
  r.resources[item->native] = item;
  win::remember_backfill_display(r, item->native);
  win::maybe_stop_live_backfill(r);
}

void profile_switch_case() {
  auto& r = win::registry();
  clear_fixture(r);
  std::array<std::shared_ptr<win::Resource>, 8> items{};
  for (unsigned i = 0; i < items.size(); ++i) {
    items[i] = display(i);
    seed_observed_display(r, items[i]);
  }
  expect("ini display passes generic dimension and RT policy", win::relevant(items[0]->desc), true);
  expect("ini display is excluded by initial FBW profile", win::display_item(r, *items[0]), false);
  win::List list;
  list.native = list_address(0);
  win::consider_live_resource(list.native, items[0]->native, taxi_camera::source_state::Model::legacy_rt);
  win::bind_live_rtv(r, list, 0x30000);
  expect("initial FBW profile cannot associate ini RTV", list.targets[0].resource != nullptr, false);
  // The absent runtime device can report private_patch_profile_failed. The
  // actual registry profile update and all metadata checks still execute.
  r.live_backfill = false;
  r.backfill_started_ms = r.backfill_inventory_ms = 1;
  win::set_aircraft_profile(profiles::IniA380.id);
  expect("profile switch rearms expired attachment observation", r.live_backfill.load(), true);
  expect("profile switch resets previous attachment deadlines", r.backfill_started_ms || r.backfill_inventory_ms, false);
  expect("actual profile switch exposes the complete existing inventory", win::pfd_inventory().size(), 8);
  for (unsigned i = 0; i < items.size(); ++i) {
    win::consider_live_resource(list.native, items[i]->native, taxi_camera::source_state::Model::legacy_rt);
    win::bind_live_rtv(r, list, 0x30000 + 32 * i);
    expect("subsequent use recovers each already-observed display", list.targets[0].resource == items[i], true);
  }
  expect("complete switched profile has eight distinct RTV associations", win::live_display_rtvs(r), 8);
  expect("restarted association keeps its full observation window", r.live_backfill.load(), true);
  win::service_live_backfill(1000, 8);
  win::service_live_backfill(1000 + win::LiveBackfillAssociateMs, 8);
  expect("restarted association ends at its bounded deadline", r.live_backfill.load(), false);
  win::discover_pfds(1000);
  expect("full late-discovered group retains automatic last and third-last selection",
         win::target_ids() == std::array<std::uint64_t, taxi_camera::MaxDisplaySides>{items[7]->id, items[5]->id}, true);
  clear_fixture(r);
}

void hint_lifetimes_case() {
  win::Registry::LiveBindHint hints;
  hints.note(list_address(0), resource_address(0));
  expect("first hint is consumed", hints.take(list_address(0)) == resource_address(0), true);
  hints.note(list_address(0), resource_address(0));
  expect("same resource can provide another hint after take", hints.take(list_address(0)) == resource_address(0), true);
  hints.note(list_address(0), resource_address(0));
  hints.clear(list_address(0));
  expect("Reset or ClearState discards prior hint", hints.take(list_address(0)) == nullptr, true);
  hints.note(list_address(0), resource_address(0));
  expect("same resource can provide a fresh recording hint", hints.take(list_address(0)) == resource_address(0), true);
  unsigned associated = 0;
  constexpr unsigned Lifetimes = win::Registry::LiveBindHint::Slots * 2 + 1;
  for (unsigned i = 0; i < Lifetimes; ++i) {
    hints.note(list_address(i), resource_address(i));
    associated += hints.take(list_address(i)) == resource_address(i);
    hints.clear(list_address(i));
  }
  expect("sequential list lifetimes recycle bounded association slots", associated, Lifetimes);
  hints.note(list_address(0), resource_address(0));
  hints.note(list_address(0), resource_address(1));
  expect("multiple resource entries refuse ambiguous association", hints.take(list_address(0)) == nullptr, true);
  hints.clear(list_address(0));
  hints.note(list_address(0), resource_address(0));
  hints.note(list_address(1), resource_address(1));
  hints.clear(list_address(1));
  expect("another list Reset cannot discard this list hint", hints.take(list_address(0)) == resource_address(0), true);
  hints.note(list_address(0), resource_address(0));
  hints.note(list_address(0), resource_address(0));
  expect("repeated same-resource entry remains unambiguous", hints.take(list_address(0)) == resource_address(0), true);

  auto& r = win::registry();
  clear_fixture(r);
  for (unsigned i = 0; i < Lifetimes; ++i) {
    win::List list;
    list.native = list_address(i);
    list.id = i + 1;
    r.live_bind.note(list.native, resource_address(i), i + 1);
    // The native lifetime callback must release its slot without an OM bind,
    // Reset, explicit clear, or take occurring during this recording.
    list.retire();
  }
  const auto next_list = list_address(Lifetimes);
  r.live_bind.note(next_list, resource_address(Lifetimes), Lifetimes + 1);
  expect("actual List retirement recycles slots without consuming hints", r.live_bind.take(next_list) == resource_address(Lifetimes), true);
  expect("actual List retirement discards its stale association", r.live_bind.take(list_address(0)) == nullptr, true);
  clear_fixture(r);
}

void distinct_associations_case() {
  const auto registry = std::make_unique<win::Registry>();
  auto& r = *registry;
  r.profile = &profiles::IniA380;
  r.live_backfill = true;
  std::array<std::shared_ptr<win::Resource>, 8> items{};
  for (unsigned i = 0; i < items.size(); ++i) {
    items[i] = display(i);
    seed_observed_display(r, items[i]);
  }
  for (unsigned i = 0; i < 8; ++i)
    win::replace_view(r, 0x40000 + 32 * i, {items[0], DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0});
  expect("descriptor aliases count as one distinct display association", win::live_display_rtvs(r), 1);
  win::maybe_stop_live_backfill(r);
  expect("eight handles for one display cannot stop attachment", r.live_backfill.load(), true);
  for (unsigned i = 1; i < 7; ++i)
    win::replace_view(r, 0x41000 + 32 * i, {items[i], DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0});
  win::maybe_stop_live_backfill(r);
  expect("seven distinct associations cannot stop ini attachment", r.live_backfill.load(), true);
  win::replace_view(r, 0x41000 + 32 * 7, {items[7], DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0});
  expect("all eight resources have distinct associations", win::live_display_rtvs(r), 8);
  win::maybe_stop_live_backfill(r);
  expect("complete ini attachment can stop extra observation", r.live_backfill.load(), false);
  r.live_backfill = true;
  const auto ninth = display(8);
  seed_observed_display(r, ninth);
  win::maybe_stop_live_backfill(r);
  expect("an additional unassociated eligible resource keeps discovery active", r.live_backfill.load(), true);
  win::replace_view(r, 0x41800, {ninth, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0});
  win::maybe_stop_live_backfill(r);
  expect("discovery finishes once every eligible resource has an association", r.live_backfill.load(), false);

  const auto partial_registry = std::make_unique<win::Registry>();
  auto& partial = *partial_registry;
  partial.profile = &profiles::IniA380;
  partial.live_backfill = true;
  for (unsigned i = 0; i < 2; ++i) {
    seed_observed_display(partial, items[i]);
    win::replace_view(partial, 0x42000 + 32 * i, {items[i], DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0});
  }
  win::maybe_stop_live_backfill(partial);
  expect("partial two-resource ini group keeps attachment enabled", partial.live_backfill.load(), true);
  const auto inventory = win::pfd_inventory_locked(partial);
  const auto targets = win::allocation_group_pair(*partial.profile, inventory);
  expect("partial ini group cannot adopt a PFD pair", targets[0] || targets[1], false);
  win::clear_views(partial);
  win::clear_views(r);
}

void recovered_view_case() {
  auto& r = win::registry();
  clear_fixture(r);
  r.profile = &profiles::IniA380;
  const auto item = display(0);
  seed_observed_display(r, item);
  win::List list;
  list.native = list_address(0);
  constexpr SIZE_T Handle = 0x50000;
  // Only a CPU descriptor handle is visible after late attach. Its native view
  // could be UNORM or sRGB; neither can be inferred from the typeless resource.
  r.live_bind.note(list.native, item->native, item->id);
  win::bind_live_rtv(r, list, Handle);
  expect("recovered binding retains the exact resource", list.targets[0].resource == item, true);
  expect("recovered binding retains the actual CPU handle", list.targets[0].rtv, Handle);
  expect("application view format remains unknown", list.targets[0].format, DXGI_FORMAT_UNKNOWN);
  expect("view records recovered provenance", list.targets[0].recovered, true);
  unsigned typed_refs = 0;
  for (auto count : item->typed_rtv_refs)
    typed_refs += count;
  expect("recovery never invents typed application-view proof", typed_refs, 0);
  list.targets = {};
  r.live_bind.note(list.native, item->native, item->id + 1);
  win::bind_live_rtv(r, list, Handle + 32);
  expect("stale resource incarnation cannot recover an RTV", list.targets[0].resource != nullptr, false);
  item->alive = false;
  r.live_bind.note(list.native, item->native, item->id);
  win::bind_live_rtv(r, list, Handle + 64);
  expect("retired resource cannot recover an RTV", list.targets[0].resource != nullptr, false);
  clear_fixture(r);
}

void rt_exit_case() {
  auto& r = win::registry();
  clear_fixture(r);
  r.profile = &profiles::IniA380;
  const auto item = display(0);
  seed_observed_display(r, item);
  const auto native = list_address(0);
  win::consider_live_resource(native, item->native, taxi_camera::source_state::Model::legacy_rt);
  win::consider_live_resource(native, item->native, taxi_camera::source_state::Model::unknown);
  expect("RT exit invalidates a prior association hint", r.live_bind.take(native) == nullptr, true);
  win::consider_live_resource(native, item->native, taxi_camera::source_state::Model::unknown);
  expect("RT exit alone cannot create a binding hint", r.live_bind.take(native) == nullptr, true);
  win::consider_live_resource(native, item->native, taxi_camera::source_state::Model::enhanced_rt);
  expect("fresh enhanced RT entry can recover a binding hint", r.live_bind.take(native) == item->native, true);
  clear_fixture(r);
}

void reconnect_discovery_case() {
  auto& r = win::registry();
  clear_fixture(r);
  r.profile = &profiles::IniA380;
  const auto first = display(0), second = display(1);
  seed_observed_display(r, first);
  seed_observed_display(r, second);
  first->draws = 500;
  second->draws = 900;
  win::replace_view(r, 0x60000, {first, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0x60000});
  win::replace_view(r, 0x60020, {second, DXGI_FORMAT_UNKNOWN, 0, 0x60020, true});
  r.routes.select_explicit({first->id, second->id});
  r.live_backfill = false;
  r.live_bind.note(list_address(0), second->native, second->id);
  r.backfill_started_ms = r.backfill_inventory_ms = 100;
  win::set_aircraft_profile(profiles::IniA380.id);
  expect("same-profile reconnect clears prior side assignments", win::target_ids() == std::array<std::uint64_t, taxi_camera::MaxDisplaySides>{}, true);
  expect("same-profile reconnect retains exact native incarnations",
         r.resources[first->native] == first && r.resources[second->native] == second, true);
  expect("same-profile reconnect starts a fresh activity baseline", first->draws.load() + second->draws.load(), 0);
  expect("same-profile reconnect retains creation-proven RTV evidence", r.rtvs.count(0x60000), 1);
  expect("same-profile reconnect discards previous recovered associations", r.rtvs.count(0x60020), 0);
  expect("same-profile reconnect retains actual typed RTV references", first->typed_rtv_refs[0], 1);
  expect("same-profile reconnect clears stale binding hints", r.live_bind.take(list_address(0)) == nullptr, true);
  expect("same-profile reconnect reopens incomplete discovery", r.live_backfill.load(), true);
  expect("same-profile reconnect restarts discovery time budgets", r.backfill_started_ms || r.backfill_inventory_ms, false);
  // A typed alias for every candidate must not hide the original opaque RTVs
  // that reconnect deliberately discarded.
  for (unsigned i = 1; i < 8; ++i) {
    const auto item = i == 1 ? second : display(i);
    if (i != 1)
      seed_observed_display(r, item);
    win::replace_view(r, 0x61000 + i * 32, {item, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0x61000 + i * 32});
  }
  win::set_aircraft_profile(profiles::IniA380.id);
  expect("complete typed aliases cannot suppress reconnect's rescan", r.live_backfill.load(), true);
  win::List list;
  list.native = list_address(0);
  win::consider_live_resource(list.native, second->native, taxi_camera::source_state::Model::legacy_rt);
  win::bind_live_rtv(r, list, 0x60020);
  expect("reconnect recovers the original handle alongside its typed alias", r.rtvs.count(0x60020), 1);
  expect("recovered original retains its exact resource", list.targets[0].resource == second, true);
  clear_fixture(r);
}

void a350_power_up_discovery_case() {
  // Replay the observed cold/dark -> AC-powered six-texture inventory through
  // the real control-thread discovery and routing handlers, without a GPU.
  constexpr std::array<std::uint64_t, 8> times{89548890, 89550062, 89551171, 89552296, 89553421, 89554546, 89555671, 89556812};
  constexpr std::array<std::uint64_t, 8> pfds{686562, 689737, 692531, 695198, 698373, 700532, 702691, 705866};
  constexpr std::array<std::uint64_t, 8> other{483285, 485170, 487345, 489085, 490970, 492855, 494740, 496625};
  constexpr std::array<std::uint64_t, 8> quiet{7870, 7896, 7924, 7952, 7978, 8006, 8036, 8064};
  auto& r = win::registry();
  for (const auto* profile : {&profiles::A359, &profiles::A35K}) {
    clear_fixture(r);
    win::set_aircraft_profile(profile->id);
    r.pfd_inventory_complete = true;
    win::discover_pfds(times[0] - 2000);
    expect("cold/dark empty A350 inventory waits for displays", win::target_ids() == std::array<std::uint64_t, taxi_camera::MaxDisplaySides>{}, true);
    std::array<std::shared_ptr<win::Resource>, 6> items{};
    for (unsigned i = 0; i < items.size(); ++i) {
      items[i] = display(i);
      items[i]->id = 1084 + i;
      items[i]->desc.Width = profile->width;
      items[i]->desc.Height = profile->height;
      items[i]->desc.MipLevels = i < 3 ? 5 : 1;
      items[i]->desc.Format = i < 3 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_TYPELESS;
      if (i < 3) {
        items[i]->draws = quiet[0];
        seed_observed_display(r, items[i]);
      }
    }
    win::discover_pfds(times[0] - 1000);
    expect("cold/dark static A350 surfaces do not become PFD targets", win::target_ids() == std::array<std::uint64_t, taxi_camera::MaxDisplaySides>{}, true);
    for (unsigned i = 3; i < items.size(); ++i)
      seed_observed_display(r, items[i]);
    expect("powered A350 inventory retains all six eligible display textures", win::pfd_inventory().size(), 6);
    for (unsigned sample = 0; sample < times.size(); ++sample) {
      for (unsigned i = 0; i < items.size(); ++i)
        items[i]->draws = i < 3 ? quiet[sample] : i == 3 ? other[sample] : pfds[sample];
      win::discover_pfds(times[sample]);
      using Sides = std::array<std::uint64_t, taxi_camera::MaxDisplaySides>;
      const auto expected = sample < 3 ? Sides{} : Sides{1089, 1088};
      expect(sample < 3 ? "powered A350 waits for three complete activity windows"
                        : "powered A350 automatically adopts and retains the reported working PFD pair",
             win::target_ids() == expected, true);
    }
    expect("automatic discovery publishes the correct native resource identities",
           r.selected_resources[0] == items[5] && r.selected_resources[1] == items[4], true);
    expect("explicit A350 side correction remains available after automatic discovery", win::assign_targets(1088, 1089), true);
    win::discover_pfds(times.back() + 1000);
    expect("automatic discovery preserves an explicit A350 side correction", win::target_ids() == std::array<std::uint64_t, taxi_camera::MaxDisplaySides>{1088, 1089},
           true);
  }
  clear_fixture(r);
}

void a350_submission_power_up_case() {
  // A35K live metadata: user confirmed47 is left.45 is the inferred paired
  // candidate; its right-side cockpit identity still needs live confirmation.
  // Replay sampled cumulative counters at detector-window intervals; log
  // cadence itself was approximately five seconds, not the discovery cadence.
  constexpr std::array<std::uint64_t, 6> ids{43, 44, 46, 75, 45, 47};
  constexpr std::array<std::uint64_t, 4> auxiliary{1, 234, 423, 571};
  constexpr std::array<std::uint64_t, 4> efis{1, 118, 224, 318};
  constexpr std::array<std::uint64_t, 4> other{1, 67, 126, 186};
  auto& r = win::registry();
  for (const auto* profile : {&profiles::A359, &profiles::A35K}) {
    clear_fixture(r);
    win::set_aircraft_profile(profile->id);
    r.pfd_inventory_complete = true;
    std::array<std::shared_ptr<win::Resource>, 6> items{};
    for (unsigned i = 0; i < items.size(); ++i) {
      items[i] = display(i);
      items[i]->id = ids[i];
      items[i]->desc.Width = profile->width;
      items[i]->desc.Height = profile->height;
      items[i]->desc.MipLevels = i < 3 ? 5 : 1;
      items[i]->desc.Format = i < 3 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    }
    seed_observed_display(r, items[0]);
    seed_observed_display(r, items[1]);
    win::discover_pfds(1000);
    expect("partial cold A350 outputs cannot bypass automatic activity proof", win::target_ids() == std::array<std::uint64_t, taxi_camera::MaxDisplaySides>{}, true);
    for (unsigned i = 2; i < items.size(); ++i)
      seed_observed_display(r, items[i]);
    win::discover_pfds(2000);
    expect("complete but unpowered A350 inventory remains unassigned", win::target_ids() == std::array<std::uint64_t, taxi_camera::MaxDisplaySides>{}, true);
    for (unsigned sample = 0; sample < auxiliary.size(); ++sample) {
      for (unsigned i = 0; i < items.size(); ++i)
        items[i]->submission_activity = i < 3 ? auxiliary[sample] : i == 3 ? other[sample] : efis[sample];
      win::discover_pfds(3000 + sample * 1000);
      expect("A350 completion-only discovery waits then adopts the EFIS activity pair",
             win::target_ids() == (sample < 3 ? std::array<std::uint64_t, taxi_camera::MaxDisplaySides>{} : std::array<std::uint64_t, taxi_camera::MaxDisplaySides>{47, 45}), true);
    }
    expect("all six candidates remain available for manual selection", win::pfd_inventory().size(), 6);
    expect("automatic routing publishes the EFIS identities", r.selected_resources[0] == items[5] && r.selected_resources[1] == items[4],
           true);
    expect("manual auxiliary selection stays available", win::assign_targets(43, 0), true);
    win::discover_pfds(7000);
    expect("automatic discovery preserves an explicit selection", win::target_ids()[0], 43);
    win::reset_display_session();
    win::set_aircraft_profile(profile->id);
    expect("full reset removes old assignment", win::target_ids() == std::array<std::uint64_t, taxi_camera::MaxDisplaySides>{}, true);
    for (unsigned sample = 0; sample < auxiliary.size(); ++sample) {
      for (unsigned i = 0; i < items.size(); ++i)
        items[i]->submission_activity = i < 3 ? auxiliary[sample] : i == 3 ? other[sample] : efis[sample];
      win::discover_pfds(8000 + sample * 1000);
    }
    expect("A350 automatic discovery recovers after full flight reset", win::target_ids() == std::array<std::uint64_t, taxi_camera::MaxDisplaySides>{47, 45}, true);
  }
  clear_fixture(r);
}

void incomplete_backfill_outlives_admit_budget() {
  auto& r = win::registry();
  clear_fixture(r);
  r.live_backfill = true;
  r.backfill_started_ms = 1000;
  win::service_live_backfill(1000 + win::LiveBackfillAdmitMs, 0);
  expect("empty inventory keeps attachment after the admit budget", r.live_backfill.load(), true);

  r.profile = &profiles::A359;
  for (unsigned i = 0; i < 3; ++i) {
    auto item = display(i);
    item->desc.Width = profiles::A359.width;
    item->desc.Height = profiles::A359.height;
    item->desc.MipLevels = 5;
    item->desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    seed_observed_display(r, item);
    win::replace_view(r, 0x50000 + 32 * i, {item, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0});
  }
  win::maybe_stop_live_backfill(r);
  expect("five-mip A350 views do not end attachment", r.live_backfill.load(), true);
  win::service_live_backfill(1000 + win::LiveBackfillAdmitMs * 4, 3);
  expect("five-mip A350 auxiliaries do not end attachment", r.live_backfill.load(), true);

  for (unsigned i = 3; i < 6; ++i) {
    auto item = display(i);
    item->desc.Width = profiles::A359.width;
    item->desc.Height = profiles::A359.height;
    item->desc.MipLevels = 1;
    item->desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    seed_observed_display(r, item);
  }
  r.backfill_inventory_ms = 0;
  r.backfill_full_window = true;
  constexpr std::uint64_t Ready = 20000;
  win::service_live_backfill(Ready, 6);
  expect("complete A350 typeless group keeps the association window", r.live_backfill.load(), true);
  win::service_live_backfill(Ready + win::LiveBackfillAssociateMs, 6);
  expect("complete A350 group ends at the association deadline", r.live_backfill.load(), false);
  clear_fixture(r);
}

void every_profile_set_keeps_attachment() {
  auto& r = win::registry();
  clear_fixture(r);
  const auto unseen = resource_address(40);
  win::consider_live_resource(list_address(0), unseen, taxi_camera::source_state::Model::legacy_rt);
  win::consider_live_copy(unseen);
  expect("undescribed pointer is not cached", r.seen_resources.contains(unseen), false);

  r.profile = &profiles::A380;
  r.live_backfill = true;
  r.backfill_full_window = true;
  for (unsigned i = 0; i < 2; ++i) {
    auto item = display(i);
    item->desc.MipLevels = profiles::A380.mips;
    item->desc.Format = static_cast<DXGI_FORMAT>(profiles::A380.formats[0]);
    seed_observed_display(r, item);
    if (i == 0) {
      win::service_live_backfill(1000, 1);
      expect("one FlyByWire display does not end attachment", r.live_backfill.load(), true);
    }
  }
  r.backfill_inventory_ms = 0;
  constexpr std::uint64_t FbwReady = 30000;
  win::service_live_backfill(FbwReady, 2);
  expect("FlyByWire pair keeps the association window", r.live_backfill.load(), true);
  win::service_live_backfill(FbwReady + win::LiveBackfillAssociateMs, 2);
  expect("FlyByWire pair ends at the association deadline", r.live_backfill.load(), false);

  clear_fixture(r);
  r.profile = &profiles::IniA380;
  r.live_backfill = true;
  r.backfill_full_window = true;
  r.backfill_started_ms = 1000;
  for (unsigned i = 0; i < 8; ++i) {
    auto item = display(i);
    item->desc.MipLevels = 1;
    item->desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    seed_observed_display(r, item);
  }
  win::service_live_backfill(1000 + win::LiveBackfillAdmitMs * 4, 8);
  expect("one-mip ini A380 auxiliaries do not end attachment", r.live_backfill.load(), true);
  for (unsigned i = 8; i < 16; ++i) {
    auto item = display(i);
    item->desc.MipLevels = 1;
    item->desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    seed_observed_display(r, item);
  }
  r.backfill_inventory_ms = 0;
  constexpr std::uint64_t IniReady = 40000;
  win::service_live_backfill(IniReady, 16);
  expect("eight ini typeless displays keep the association window", r.live_backfill.load(), true);
  win::service_live_backfill(IniReady + win::LiveBackfillAssociateMs, 16);
  expect("eight ini typeless displays end at the association deadline", r.live_backfill.load(), false);
  clear_fixture(r);
}

void ini_explicit_discovery_case() {
  auto& r = win::registry();
  clear_fixture(r);
  win::set_aircraft_profile(profiles::IniA380.id);
  r.pfd_inventory_complete = true;
  for (unsigned i = 0; i < 8; ++i) {
    const auto item = display(i);
    item->id = 163 + i;
    seed_observed_display(r, item);
  }
  std::uint64_t now = 1000;
  win::discover_pfds(now);
  expect("idle complete ini group still automatically selects last/third-last", win::target_ids() == std::array<std::uint64_t, taxi_camera::MaxDisplaySides>{170, 168},
         true);
  for (const auto selection : {std::array<std::uint64_t, 2>{164, 0}, std::array<std::uint64_t, 2>{0, 166}}) {
    expect("accept explicit ini side outside the automatic pair", win::assign_targets(selection[0], selection[1]), true);
    win::set_calibration(selection[0] ? 1 : 2, 4096);
    for (unsigned pass = 0; pass < 4; ++pass) {
      now += 1000;
      win::discover_pfds(now);
      expect("automatic polling preserves the explicit single-side calibration target",
             win::target_ids() == std::array<std::uint64_t, taxi_camera::MaxDisplaySides>{selection[0], selection[1]}, true);
    }
    const unsigned side = selection[0] ? 0 : 1;
    expect("published calibration identity remains the user's selected resource", r.selected_ids[side].load(), selection[side]);
  }
  // An explicit choice remains explicit even when it equals an old automatic
  // ID. Changing the ranked pair must not reinterpret that manual anchor.
  expect("select previously automatic left ID explicitly", win::assign_targets(170, 0), true);
  auto removed = r.resources.find(resource_address(0));
  removed->second->alive = false;
  const auto replacement = display(8);
  replacement->id = 171;
  seed_observed_display(r, replacement);
  now += 1000;
  win::discover_pfds(now);
  expect("changed allocation ranks preserve a same-ID explicit anchor", win::target_ids()[0], 170);

  // Preserve the former automatic-singleton recovery using genuinely detected
  // provenance, rather than mislabelling an explicit user request as stale.
  r.routes.reset();
  expect("seed earlier automatic pair", r.routes.adopt_detected({164, 163}), true);
  r.routes.forget(163);
  now += 1000;
  win::discover_pfds(now);
  expect("stale automatic singleton recovers to the complete ranked group", win::target_ids() == std::array<std::uint64_t, taxi_camera::MaxDisplaySides>{171, 169},
         true);
  r.routes.forget_detected();
  expect("recovered automatic pair retains detected provenance", win::target_ids() == std::array<std::uint64_t, taxi_camera::MaxDisplaySides>{}, true);
  clear_fixture(r);
}
void recovered_output_encoding_case() {
  auto& r = win::registry();
  clear_fixture(r);
  const auto item = display(0);
  seed_observed_display(r, item);
  win::View recovered{item, DXGI_FORMAT_UNKNOWN, 0, 0};
  recovered.recovered = true;
  expect("typeless recovered encoding defaults to UNORM without typed proof",
         static_cast<unsigned>(win::output_format(recovered)), static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM));
  // Account a live application sRGB RTV the same way OM bind does.
  win::replace_view(r, 0x70000, {item, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0, 0x70000});
  expect("sRGB application RTV is remembered on the typeless resource", item->typed_rtv_refs[1], 1);
  expect("recovered encoding follows the sole observed sRGB RTV", static_cast<unsigned>(win::output_format(recovered)),
         static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
  win::replace_view(r, 0x70020, {item, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0x70020});
  expect("UNORM+sRGB conflict refuses a recovered encoding guess", static_cast<unsigned>(win::output_format(recovered)),
         static_cast<unsigned>(DXGI_FORMAT_UNKNOWN));
  clear_fixture(r);
}

// The window's seen set holds a flight load's worth of distinct resources. At
// 1024 slots it filled early and every later copy repeated the classification.
void seen_set_capacity_case() {
  using Seen = win::Registry::SeenResources;
  const auto seen = std::make_unique<Seen>();
  const auto pointer = [](std::uintptr_t base, std::uintptr_t stride, unsigned i) {
    return reinterpret_cast<ID3D12Resource*>(base + stride * i);
  };
  // Heap neighbours 64 bytes apart, and objects 64 KiB apart, whose low 16
  // address bits are equal.
  unsigned remembered = 0, found = 0, filter_range = 0;
  for (unsigned i = 0; i < 24576; ++i)
    remembered += seen->remember(pointer(0x20000000, 0x40, i));
  for (unsigned i = 0; i < 8192; ++i)
    remembered += seen->remember(pointer(0x400000000, 0x10000, i));
  for (unsigned i = 0; i < 24576; ++i)
    found += seen->contains(pointer(0x20000000, 0x40, i));
  for (unsigned i = 0; i < 8192; ++i)
    found += seen->contains(pointer(0x400000000, 0x10000, i));
  expect("seen set remembers 32768 distinct heap and strided resources", remembered, 32768);
  expect("seen set finds every remembered resource", found, 32768);
  expect("seen set does not report an unseen resource", seen->contains(pointer(0x20000000, 0x40, 24576)), false);
  for (unsigned i = 0; i < 4096; ++i) {
    auto x = static_cast<std::uintptr_t>(0x20000000 + 0x40 * i);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    filter_range += Seen::hash(pointer(0x20000000, 0x40, i)) == (static_cast<unsigned>(x) & 1023);
  }
  expect("display filter bits keep their 1024-value hash", filter_range, 4096);

  // A full probe run is reported, and that pointer is classified again later.
  const auto colliding = std::make_unique<Seen>();
  std::array<ID3D12Resource*, Seen::Probes + 1> run{};
  unsigned collected = 0;
  const auto target = Seen::slot(pointer(0x30000000, 0x40, 0));
  for (unsigned i = 0; collected < run.size() && i < (Seen::Capacity << 6); ++i)
    if (Seen::slot(pointer(0x30000000, 0x40, i)) == target)
      run[collected++] = pointer(0x30000000, 0x40, i);
  expect("fixture found a full probe run", collected, run.size());
  unsigned accepted = 0;
  for (unsigned i = 0; i < Seen::Probes; ++i)
    accepted += colliding->remember(run[i]);
  expect("one probe run holds Probes resources", accepted, Seen::Probes);
  expect("a resource past a full probe run is refused", colliding->remember(run[Seen::Probes]), false);
  expect("a refused resource stays unknown", colliding->contains(run[Seen::Probes]), false);
  // Forgetting one keeps later members of its run reachable; it can return.
  colliding->forget(run[0]);
  expect("forgotten resource is unknown", colliding->contains(run[0]), false);
  expect("forgetting keeps the rest of the run", colliding->contains(run[Seen::Probes - 1]), true);
  expect("a forgotten slot is not reused", colliding->remember(run[Seen::Probes]), false);
  colliding->forget(run[Seen::Probes - 1]);
  colliding->forget(run[Seen::Probes]);
  expect("forgetting an absent resource changes nothing", colliding->contains(run[1]), true);
  const auto other = std::make_unique<Seen>();
  other->remember(run[0]);
  other->remember(run[1]);
  other->forget(run[0]);
  expect("a forgotten resource can be remembered again", other->remember(run[0]) && other->contains(run[0]), true);
  expect("its neighbour stays known", other->contains(run[1]), true);
}

// Registered resources need no classification; anything else still gets the
// full one, every time until it is described.
void known_resource_case() {
  auto& r = win::registry();
  clear_fixture(r);
  const bool was_ready = r.ready;
  r.ready = true;
  r.profile = &profiles::IniA380;
  const auto classifications = [&] { return r.backfill_classifications.load(); };
  const auto registered = display(0);
  r.resources[registered->native] = registered;
  r.resource_index.assign(registered->native, registered);
  win::remember_backfill_display(r, registered->native);
  auto before = classifications();
  win::consider_live_copy(registered->native);
  expect("registered copy source needs no classification", classifications() - before, 0);
  expect("registered copy source is remembered", r.seen_resources.contains(registered->native), true);
  const auto second = display(1);
  r.resources[second->native] = second;
  r.resource_index.assign(second->native, second);
  win::remember_backfill_display(r, second->native);
  const auto list = list_address(0);
  before = classifications();
  win::consider_live_resource(list, second->native, taxi_camera::source_state::Model::legacy_rt);
  expect("registered RT entry needs no classification", classifications() - before, 0);
  expect("registered display RT entry still provides its bind hint", r.live_bind.take(list) == second->native, true);
  // An object the pointer checks reject: its first word is no vtable.
  std::array<void*, 4> object{};
  auto* unknown = reinterpret_cast<ID3D12Resource*>(object.data());
  const auto unclassified = r.backfill_unclassified.load();
  before = classifications();
  win::consider_live_copy(unknown);
  win::consider_live_copy(unknown);
  expect("an undescribed resource is classified on every use", classifications() - before, 2);
  expect("both classifications report the failure", r.backfill_unclassified.load() - unclassified, 2);
  expect("an undescribed resource is never remembered", r.seen_resources.contains(unknown), false);
  // A retired registry entry is not evidence.
  auto retired = display(2);
  retired->native = unknown;
  retired->alive = false;
  r.resource_index.assign(unknown, retired);
  before = classifications();
  win::consider_live_resource(list, unknown, taxi_camera::source_state::Model::legacy_rt);
  expect("a retired registry entry still takes the full classification", classifications() - before, 1);
  r.ready = false;
  const auto third = display(3);
  r.resources[third->native] = third;
  r.resource_index.assign(third->native, third);
  before = classifications();
  win::consider_live_copy(third->native);
  expect("before graphics are ready the registry index is not consulted", classifications() - before, 1);
  expect("and the resource stays unremembered", r.seen_resources.contains(third->native), false);
  const auto status = win::graphics_status();
  expect("status reports classifications", status.backfill_classifications, r.backfill_classifications.load());
  expect("status reports failed classifications", status.backfill_unclassified, r.backfill_unclassified.load());
  expect("status reports the open window", status.backfill_open, true);
  for (const auto& item : {registered, second, retired, third})
    r.resource_index.erase(item->native, item.get());
  r.ready = was_ready;
  clear_fixture(r);
}

// A placed-footprint copy location is a buffer and is never classified.
void placed_footprint_case() {
  auto& r = win::registry();
  clear_fixture(r);
  const bool was_ready = r.ready;
  r.ready = true;
  std::array<void*, 4> first{}, second{};
  D3D12_TEXTURE_COPY_LOCATION buffer{}, texture{};
  buffer.pResource = reinterpret_cast<ID3D12Resource*>(first.data());
  buffer.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  texture.pResource = reinterpret_cast<ID3D12Resource*>(second.data());
  texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  const auto list = list_address(9);
  auto before = r.backfill_classifications.load();
  win::copy_texture(nullptr, list, 1, &texture, 0, 0, 0, &buffer, nullptr, false);
  expect("an upload copy classifies only its texture", r.backfill_classifications.load() - before, 1);
  before = r.backfill_classifications.load();
  win::copy_texture(nullptr, list, 1, &buffer, 0, 0, 0, &texture, nullptr, false);
  expect("a readback copy classifies only its texture", r.backfill_classifications.load() - before, 1);
  auto other = texture;
  other.pResource = buffer.pResource;
  before = r.backfill_classifications.load();
  win::copy_texture(nullptr, list, 1, &texture, 0, 0, 0, &other, nullptr, false);
  expect("a texture-to-texture copy classifies both", r.backfill_classifications.load() - before, 2);
  r.live_backfill = false;
  before = r.backfill_classifications.load();
  win::copy_texture(nullptr, list, 1, &texture, 0, 0, 0, &other, nullptr, false);
  expect("a closed window classifies nothing", r.backfill_classifications.load() - before, 0);
  r.ready = was_ready;
  clear_fixture(r);
}

// A creation that misses the registry is learned on first use, so an earlier
// object the window remembered at the same address must not hide it.
struct FakeResource final : ID3D12Resource {
  D3D12_RESOURCE_DESC desc{};
  ULONG references = 1;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12Resource))
      return E_NOINTERFACE;
    *out = static_cast<ID3D12Resource*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override { return --references; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetName(const WCHAR*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void** device) override {
    *device = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE Map(UINT, const D3D12_RANGE*, void**) override { return E_NOTIMPL; }
  void STDMETHODCALLTYPE Unmap(UINT, const D3D12_RANGE*) override {}
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  using ID3D12Resource::GetDesc;
  D3D12_RESOURCE_DESC* STDMETHODCALLTYPE GetDesc(D3D12_RESOURCE_DESC* result) override {
    *result = desc;
    return result;
  }
#else
  D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return desc; }
#endif
  D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override { return 0; }
  HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX*, const void*, UINT, UINT) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ReadFromSubresource(void*, UINT, UINT, UINT, const D3D12_BOX*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS*) override { return E_NOTIMPL; }
};
void creation_miss_case() {
  auto& r = win::registry();
  clear_fixture(r);
  const bool was_ready = r.ready;
  auto* const old_device = r.device;
  int device_token{};
  auto* device = reinterpret_cast<ID3D12Device*>(&device_token);  // Compared by address only.
  r.ready = true;
  r.device = device;
  FakeResource created;
  created.desc = display(0)->desc;
  r.seen_resources.remember(&created);  // An earlier object the window classified at this address.
  std::atomic<bool> held{false}, release{false};
  std::thread holder([&] {
    const std::lock_guard lock(r.mutex);
    held = true;
    while (!release)
      std::this_thread::yield();
  });
  while (!held)
    std::this_thread::yield();
  const bool registered = win::observe_resource(device, &created, taxi_camera::source_state::Model::unknown, true);
  release = true;
  holder.join();
  expect("a creation that misses the registry is not registered", registered, false);
  expect("its address is forgotten for its first use", r.seen_resources.contains(&created), false);
  expect("the missed creation released its query reference", created.references, 1);
  expect("no registry entry was made", r.resources.count(&created), 0);
  r.device = old_device;
  r.ready = was_ready;
  clear_fixture(r);
}

}  // namespace

int main() {
  profile_switch_case();
  hint_lifetimes_case();
  distinct_associations_case();
  recovered_view_case();
  recovered_output_encoding_case();
  rt_exit_case();
  reconnect_discovery_case();
  a350_power_up_discovery_case();
  a350_submission_power_up_case();
  incomplete_backfill_outlives_admit_budget();
  every_profile_set_keeps_attachment();
  ini_explicit_discovery_case();
  seen_set_capacity_case();
  known_resource_case();
  placed_footprint_case();
  creation_miss_case();
  std::printf(
      "%s late-attach metadata: checks=%u failures=%u; profile switch, distinct displays, hint lifetimes, unknown views, RT exits, "
      "seen set, registered and placed resources, missed creations.\n",
      failures ? "FAIL" : "PASS", checks, failures);
  return failures ? 1 : 0;
}
