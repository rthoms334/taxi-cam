#include "../../src/camera/view_resize.hpp"

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
namespace nc = taxi_camera::native_camera;
namespace ec = taxi_camera::engine_camera;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}

struct Fixture {
  std::uint8_t* allocation = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  std::uint8_t* memory = nullptr;
  ec::OwnedViewSnapshot view;
  nc::ViewDimensions desired{};
  unsigned feed = 0;
  unsigned refreshed = 0;
  unsigned allocated = 0;
  unsigned failure = 0;
  std::array<std::uint8_t, 256> before{};

  explicit Fixture(unsigned offset = 0, unsigned requested_feed = 0) : feed(requested_feed) {
    require(allocation != nullptr, "fixture allocation");
    memory = allocation + offset;
    std::memset(allocation, 0xa5, 8192);
    view.complete = view.ready = true;
    view.status = ec::OwnedViewStatus::ready;
    view.view_address = reinterpret_cast<std::uintptr_t>(memory);
    view.dimensions.fill({3413, 913});
    view.flags = {0x123456789abcdef1ull, 0xfedcba9876543210ull};
    std::memcpy(memory + 16, &view.dimensions, sizeof(view.dimensions));
    std::memcpy(memory + 48, &view.flags, sizeof(view.flags));
    std::memcpy(before.data(), memory, before.size());
    require(nc::plan_view_resize(view.dimensions, feed, desired), "fixture dimensions");
  }
  ~Fixture() { VirtualFree(allocation, 0, MEM_RELEASE); }
  nc::ViewResizeCallbacks callbacks() {
    return {this,
            [](void* opaque, std::uint64_t view) noexcept {
              auto& self = *static_cast<Fixture*>(opaque);
              ++self.refreshed;
              if (view != self.view.view_address || self.allocated || std::memcmp(self.memory + 16, &self.desired, 24))
                return false;
              if (self.failure == 3)
                self.memory[48] ^= 1;
              if (self.failure == 4)
                self.memory[16] ^= 1;
              return self.failure != 1;
            },
            [](void* opaque, std::uint64_t view) noexcept -> std::uint64_t {
              auto& self = *static_cast<Fixture*>(opaque);
              ++self.allocated;
              if (self.refreshed != 1 || self.failure == 2)
                return 0;
              return view + 144;
            }};
  }
  void unchanged_bytes() {
    require(std::memcmp(before.data(), memory, before.size()) == 0, "refusal changed fixture bytes");
    require(refreshed == 0 && allocated == 0, "refusal invoked a callback");
  }
};

void planning() {
  nc::ViewDimensions input{}, output{};
  for (const auto inherited : std::array<std::array<std::int32_t, 2>, 7>{
           {{3413, 913}, {1920, 1080}, {768, 763}, {512, 512}, {16384, 16384}, {32, 32}, {16384, 32}}}) {
    input.fill(inherited);
    for (unsigned feed = 0; feed < 2; ++feed) {
      require(nc::plan_view_resize(input, feed, output), "valid dimensions refused");
      for (const auto pair : output)
        require(pair == std::array<std::int32_t, 2>{736, feed == 0 ? 251 : 496}, "output differs from its exact PFD pane");
    }
  }
  for (const auto value : {std::numeric_limits<std::int32_t>::min(), -1, 0, 31, 16385, std::numeric_limits<std::int32_t>::max()}) {
    input.fill({value, 913});
    require(!nc::plan_view_resize(input, 0, output) && output == nc::ViewDimensions{}, "invalid width accepted/leaked output");
    input.fill({3413, value});
    require(!nc::plan_view_resize(input, 1, output) && output == nc::ViewDimensions{}, "invalid height accepted/leaked output");
  }
  input.fill({3413, 913});
  input[1][0] = 3414;
  require(!nc::plan_view_resize(input, 0, output), "different dimension pair accepted");
  input.fill({3413, 913});
  for (const auto feed : {2u, 3u, std::numeric_limits<unsigned>::max()})
    require(!nc::plan_view_resize(input, feed, output) && output == nc::ViewDimensions{}, "unknown feed accepted/leaked output");
}
void success(unsigned feed) {
  Fixture fixture(0, feed);
  const auto result = nc::resize_owned_view(fixture.view, fixture.feed, fixture.desired, fixture.callbacks());
  require(result.complete && result.write_attempted && result.status == nc::ViewResizeStatus::resized, "resize failed");
  require(fixture.refreshed == 1 && fixture.allocated == 1, "projection/allocation not called exactly once");
  require(std::memcmp(fixture.memory + 16, &fixture.desired, 24) == 0, "dimension write mismatch");
  for (unsigned i = 0; i < fixture.before.size(); ++i)
    if (i < 16 || i >= 40)
      require(fixture.memory[i] == fixture.before[i], "write escaped exact 24-byte dimension span");
  fixture.view.dimensions = fixture.desired;
  const auto again = nc::resize_owned_view(fixture.view, fixture.feed, fixture.desired, fixture.callbacks());
  require(again.complete && !again.write_attempted && again.status == nc::ViewResizeStatus::unchanged, "unchanged resize mutated");
  require(fixture.refreshed == 1 && fixture.allocated == 1, "unchanged view reallocated");
}

void refusals() {
  for (unsigned choice = 0; choice < 15; ++choice) {
    Fixture fixture;
    auto callbacks = fixture.callbacks();
    switch (choice) {
      case 0:
        fixture.view.complete = false;
        break;
      case 1:
        fixture.view.ready = false;
        break;
      case 2:
        fixture.view.read_failures = 1;
        break;
      case 3:
        fixture.view.error = "failed";
        break;
      case 4:
        fixture.view.view_address = 0;
        break;
      case 5:
        fixture.view.view_address |= 1;
        break;
      case 6:
        fixture.view.view_address = std::numeric_limits<std::uint64_t>::max() - 7;
        break;
      case 7:
        fixture.view.flags[0] &= ~1ull;
        break;
      case 8:
        callbacks.refresh_projection = nullptr;
        break;
      case 9:
        callbacks.ensure_output = nullptr;
        break;
      case 10:
        fixture.desired[0][0] = 769;
        break;
      case 11:
        fixture.view.dimensions[0][0] = 3412;
        break;
      case 12:
        fixture.view.flags[1] ^= 8;
        break;
      case 13:
        fixture.feed = 2;
        break;
      case 14:
        fixture.desired.fill({736, 496});  // The other feed's valid size must still be refused.
        break;
    }
    const auto result = nc::resize_owned_view(fixture.view, fixture.feed, fixture.desired, callbacks);
    require(!result.complete && !result.write_attempted, "invalid proof caused write");
    fixture.unchanged_bytes();
  }
  for (const auto protection : {PAGE_READONLY, PAGE_NOACCESS, PAGE_EXECUTE_READWRITE, PAGE_READWRITE | PAGE_GUARD}) {
    Fixture fixture;
    DWORD prior = 0;
    require(VirtualProtect(fixture.memory, 4096, protection, &prior) != 0, "fixture protection");
    const auto result = nc::resize_owned_view(fixture.view, fixture.feed, fixture.desired, fixture.callbacks());
    require(!result.complete && !result.write_attempted && result.status == nc::ViewResizeStatus::invalid_mapping,
            "unsafe region accepted");
    MEMORY_BASIC_INFORMATION region{};
    VirtualQuery(fixture.memory, &region, sizeof(region));
    require(region.Protect == static_cast<DWORD>(protection), "refusal consumed guard/changed page protection");
    DWORD ignored = 0;
    require(VirtualProtect(fixture.memory, 4096, prior, &ignored) != 0, "fixture protection restore");
    fixture.unchanged_bytes();
  }
}

void callback_failures() {
  for (unsigned failure = 1; failure <= 4; ++failure) {
    Fixture fixture;
    fixture.failure = failure;
    const auto result = nc::resize_owned_view(fixture.view, fixture.feed, fixture.desired, fixture.callbacks());
    require(!result.complete && result.write_attempted, "failed callback accepted");
    require(fixture.refreshed == 1 && fixture.allocated == (failure == 2 ? 1u : 0u), "failure callback order/count");
    require(result.status == (failure == 1   ? nc::ViewResizeStatus::refresh_failed
                              : failure == 2 ? nc::ViewResizeStatus::allocation_failed
                                             : nc::ViewResizeStatus::changed),
            "wrong failure status");
  }
}

void stale_fields_and_boundary() {
  for (unsigned offset = 16; offset < 64; ++offset) {
    if (offset >= 40 && offset < 48)
      continue;
    Fixture fixture;
    fixture.memory[offset] ^= 4;
    std::memcpy(fixture.before.data(), fixture.memory, fixture.before.size());
    const auto result = nc::resize_owned_view(fixture.view, fixture.feed, fixture.desired, fixture.callbacks());
    require(!result.complete && !result.write_attempted && result.status == nc::ViewResizeStatus::changed,
            "changed dimension/flag byte was not refused");
    fixture.unchanged_bytes();
  }
  Fixture fixture(4096 - 24);
  DWORD prior = 0;
  require(VirtualProtect(fixture.allocation + 4096, 4096, PAGE_READONLY, &prior) != 0, "boundary protection");
  const auto result = nc::resize_owned_view(fixture.view, fixture.feed, fixture.desired, fixture.callbacks());
  require(!result.complete && !result.write_attempted && result.status == nc::ViewResizeStatus::invalid_mapping,
          "partially writable 24-byte span was accepted");
  fixture.unchanged_bytes();
}

void retained_dimension_restore() {
  // Live DLSS -> TAA transition: primary render size differs from display
  // size, but each owned Bitmap retains its original A350 pane allocation.
  for (unsigned feed = 0; feed < 2; ++feed) {
    Fixture f(0, feed);
    const taxi_camera::profiles::CameraPanes panes{{{774, 251}, {774, 496}}};
    f.desired.fill(panes[feed]);
    f.view.mode = 2;
    f.view.resource_present = true;
    f.view.output_dimensions = panes[feed];
    f.view.dimensions = {{{1695, 901}, {2542, 1351}, {2542, 1351}}};
    std::memcpy(f.memory + 16, &f.view.dimensions, 24);
    auto callbacks = f.callbacks();
    callbacks.ensure_output = nullptr;
    const auto result = nc::restore_owned_view_dimensions(f.view, feed, f.desired, callbacks, panes);
    require(result.complete && result.write_attempted && result.status == nc::ViewResizeStatus::dimensions_restored,
            "DLSS/TAA mixed primary sizes prevented retained camera recovery");
    require(f.refreshed == 1 && f.allocated == 0, "AA recovery must refresh without allocating a texture");
    for (unsigned i = 0; i < f.before.size(); ++i)
      if (i < 16 || i >= 40)
        require(f.memory[i] == f.before[i], "AA recovery changed flags or bytes outside the dimensions");
  }
  for (unsigned pair = 0; pair < 3; ++pair) {
    for (unsigned axis = 0; axis < 2; ++axis) {
      for (const auto invalid : {0, 31, 16385, std::numeric_limits<std::int32_t>::max()}) {
        Fixture f;
        f.view.mode = 2;
        f.view.resource_present = true;
        f.view.output_dimensions = f.desired[0];
        f.view.dimensions = {{{1695, 901}, {2542, 1351}, {2542, 1351}}};
        f.view.dimensions[pair][axis] = invalid;
        std::memcpy(f.memory + 16, &f.view.dimensions, 24);
        std::memcpy(f.before.data(), f.memory, f.before.size());
        const auto result = nc::restore_owned_view_dimensions(f.view, 0, f.desired, f.callbacks());
        require(!result.complete && !result.write_attempted && result.status == nc::ViewResizeStatus::invalid_dimensions,
                "AA recovery admitted an out-of-bounds inherited dimension");
        f.unchanged_bytes();
      }
    }
  }
  for (unsigned feed = 0; feed < 2; ++feed) {
    Fixture f(0, feed);
    f.view.mode = 2;
    f.view.resource_present = true;
    f.view.output_dimensions = f.desired[0];
    auto callbacks = f.callbacks();
    callbacks.ensure_output = nullptr;  // Established outputs must never be replaced.
    for (unsigned cycle = 0; cycle < 20; ++cycle) {
      f.view.dimensions.fill({cycle % 2 ? 2560 : 3413, cycle % 2 ? 1440 : 913});
      std::memcpy(f.memory + 16, &f.view.dimensions, 24);
      const auto result = nc::restore_owned_view_dimensions(f.view, feed, f.desired, callbacks);
      require(result.complete && result.write_attempted && result.status == nc::ViewResizeStatus::dimensions_restored,
              "primary-size overwrite did not restore existing pane fields");
      require(f.allocated == 0 && f.refreshed == cycle + 1, "retained recovery allocated an output or missed projection");
      for (unsigned i = 0; i < f.before.size(); ++i)
        if (i < 16 || i >= 40)
          require(f.memory[i] == f.before[i], "retained recovery wrote outside24sizebytes");
    }
  }
  for (unsigned choice = 0; choice < 5; ++choice) {
    Fixture f;
    f.view.mode = 2;
    f.view.resource_present = true;
    f.view.output_dimensions = f.desired[0];
    auto callbacks = f.callbacks();
    if (choice == 0)
      f.view.mode = 1;
    if (choice == 1)
      f.view.resource_present = false;
    if (choice == 2)
      ++f.view.output_dimensions[0];
    if (choice == 3)
      f.view.flags[0] &= ~1ull;
    if (choice == 4)
      callbacks.refresh_projection = nullptr;
    const auto result = nc::restore_owned_view_dimensions(f.view, f.feed, f.desired, callbacks);
    require(!result.complete && !result.write_attempted, "unproven retained-output restoration was admitted");
    f.unchanged_bytes();
  }
  for (unsigned failure : {1u, 3u, 4u}) {
    Fixture f;
    f.view.mode = 2;
    f.view.resource_present = true;
    f.view.output_dimensions = f.desired[0];
    f.failure = failure;
    const auto result = nc::restore_owned_view_dimensions(f.view, f.feed, f.desired, f.callbacks());
    require(!result.complete && result.write_attempted && f.allocated == 0,
            "partial restoration did not refuse without output replacement");
  }
}

void empty_manager_warmup() {
  nc::ViewResizeWarmup warmup;
  const std::array<std::uint64_t, 2> ids{1003, 1004};
  require(!warmup.begin({0, 1004}, 1) && !warmup.pending(), "zero owned ID admitted");
  require(!warmup.begin({1003, 1003}, 1) && !warmup.pending(), "duplicate owned ID admitted");
  require(!warmup.begin(ids, 0) && !warmup.pending(), "unobserved iteration admitted");
  Fixture nose;
  Fixture tail(0, 1);
  unsigned manager_entries = 0;
  std::array<std::int32_t, 2> manager_cache{};
  const std::array<std::int32_t, 2> primary{3413, 913};
  const auto original_update = [&] {
    if (!manager_entries)
      return;  // Captured early return at17648645/17648649.
    if (manager_cache == primary)
      return;
    manager_cache = primary;
    for (auto* fixture : {&nose, &tail}) {
      nc::ViewDimensions inherited;
      inherited.fill(manager_cache);
      std::memcpy(fixture->memory + 16, &inherited, sizeof(inherited));
    }
  };
  original_update();
  require(manager_cache == std::array<std::int32_t, 2>{}, "empty manager populated its cache");
  manager_entries = 2;
  require(warmup.begin(ids, 10), "new pair warmup refused");
  require(!warmup.may_resize(ids, 10) && !warmup.finish(ids, 10), "same-update resize or activation admitted");
  require(!warmup.may_resize({1003, 1005}, 11) && !warmup.may_resize(ids, 9), "stale pair/iteration admitted");
  require(nose.refreshed == 0 && nose.allocated == 0 && tail.refreshed == 0 && tail.allocated == 0,
          "private resize called before original warmup");
  original_update();
  require(manager_cache == primary, "first nonempty original update did not initialize cache");
  require(warmup.may_resize(ids, 11) && warmup.pending(), "next observer did not allow closed resize");
  require(nc::resize_owned_view(nose.view, nose.feed, nose.desired, nose.callbacks()).complete, "nose resize after warmup");
  require(nc::resize_owned_view(tail.view, tail.feed, tail.desired, tail.callbacks()).complete, "tail resize after warmup");
  require(warmup.finish(ids, 11) && !warmup.pending(), "successful resize did not permit scheduling");
  original_update();
  require(std::memcmp(nose.memory + 16, &nose.desired, 24) == 0 && std::memcmp(tail.memory + 16, &tail.desired, 24) == 0,
          "stable primary dimensions overwrote resized mode2 outputs");
  require(nose.refreshed == 1 && nose.allocated == 1 && tail.refreshed == 1 && tail.allocated == 1, "warmup caused duplicate allocation");
  require(warmup.begin({1005, 1006}, 12), "replacement pair warmup refused");
  warmup.clear();  // Stop before the next original update must cancel resize.
  require(!warmup.pending() && !warmup.may_resize({1005, 1006}, 13), "stop retained a stale resize request");
  require(warmup.begin(ids, std::numeric_limits<std::uint64_t>::max()), "last iteration refused unexpectedly");
  require(!warmup.may_resize(ids, 1), "wrapped update counter bypassed warmup");
}
}  // namespace

int main() {
  try {
    planning();
    success(0);
    success(1);
    refusals();
    callback_failures();
    stale_fields_and_boundary();
    empty_manager_warmup();
    retained_dimension_restore();
    std::printf("PASS: %u owned-view resize checks; only own private fixture memory and mock callbacks used.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
