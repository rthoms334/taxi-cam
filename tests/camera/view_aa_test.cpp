#include "../../src/camera/view_aa.hpp"
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
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
struct Image final : taxi_camera::discovery::ImageReader {
  std::array<std::uint64_t, 2> overrides{4, 0};
  unsigned reads = 0, fail_at = 0, change_at = 0;
  taxi_camera::discovery::ReadWindow query(std::uint32_t, std::uint32_t) override { return {}; }
  bool read(std::uint32_t rva, void* output, std::size_t size) override {
    ++reads;
    if (reads == fail_at || size != 8 || (rva != nc::kViewFlagClearOverride && rva != nc::kViewFlagSetOverride))
      return false;
    auto value = overrides[rva == nc::kViewFlagSetOverride];
    if (reads == change_at)
      value ^= 16;
    std::memcpy(output, &value, 8);
    return true;
  }
};
struct Fixture {
  unsigned char* allocation = static_cast<unsigned char*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  unsigned char* view_memory = nullptr;
  ec::OwnedViewSnapshot view;
  Image image;
  std::array<unsigned char, 128> before{};
  explicit Fixture(unsigned offset = 0) {
    require(allocation != nullptr, "allocation failed");
    std::memset(allocation, 0xa5, 8192);
    view_memory = allocation + offset;
    view.complete = view.ready = true;
    view.mode = 2;
    view.status = ec::OwnedViewStatus::ready;
    view.view_address = reinterpret_cast<std::uintptr_t>(view_memory);
    view.flags = {0x0019e013ff2220efull, 0xabcdef0123456789ull};
    save();
  }
  void save() {
    std::memcpy(view_memory + 48, view.flags.data(), 16);
    std::memcpy(before.data(), view_memory, before.size());
  }
  void unchanged() { require(!std::memcmp(before.data(), view_memory, before.size()), "refusal modified view"); }
  ~Fixture() { VirtualFree(allocation, 0, MEM_RELEASE); }
};
void success(unsigned offset) {
  Fixture fixture(offset);
  const auto result = nc::disable_owned_view_aa(fixture.view, fixture.image);
  require(result.complete && result.write_attempted && !*result.error, "AA disable failed");
  auto expected = fixture.before;
  auto flags = fixture.view.flags;
  flags[0] &= ~nc::kViewAaFlag;
  std::memcpy(expected.data() + 48, flags.data(), 16);
  require(!std::memcmp(expected.data(), fixture.view_memory, expected.size()), "changed bits outside AA flag");
  require((flags[0] & 1) && flags[1] == fixture.view.flags[1], "gate or second flag word changed");
  require(fixture.image.reads == 4, "override bracket incomplete");
  fixture.view.flags = flags;
  fixture.save();
  const auto repeated = nc::disable_owned_view_aa(fixture.view, fixture.image);
  require(repeated.complete && !repeated.write_attempted, "already-disabled view rewritten");
  fixture.unchanged();
}
void refusals() {
  for (unsigned failure = 0; failure < 14; ++failure) {
    Fixture fixture;
    switch (failure) {
      case 0:
        fixture.view.complete = false;
        break;
      case 1:
        fixture.view.ready = false;
        break;
      case 2:
        fixture.view.mode = 0;
        break;
      case 3:
        fixture.view.status = ec::OwnedViewStatus::changed;
        break;
      case 4:
        fixture.view.read_failures = 1;
        break;
      case 5:
        fixture.view.error = "invalid";
        break;
      case 6:
        fixture.view.view_address = 0;
        break;
      case 7:
        ++fixture.view.view_address;
        break;
      case 8:
        fixture.view.view_address = UINTPTR_MAX - 7;
        break;
      case 9:
        fixture.view.flags[0] &= ~1ull;
        fixture.save();
        break;
      case 10:
        fixture.view_memory[56] ^= 2;
        std::memcpy(fixture.before.data(), fixture.view_memory, 128);
        break;
      case 11:
        fixture.image.overrides[1] = nc::kViewAaFlag;
        break;
      case 12:
        fixture.image.fail_at = 1;
        break;
      case 13:
        fixture.image.fail_at = 2;
        break;
    }
    const auto result = nc::disable_owned_view_aa(fixture.view, fixture.image);
    require(!result.complete && !result.write_attempted && *result.error, "unsafe snapshot accepted");
    fixture.unchanged();
  }
  Fixture readonly;
  DWORD old = 0;
  require(VirtualProtect(readonly.allocation, 8192, PAGE_READONLY, &old), "protection setup failed");
  const auto result = nc::disable_owned_view_aa(readonly.view, readonly.image);
  require(!result.complete && !result.write_attempted, "read-only mapping written");
  readonly.unchanged();
  for (unsigned failure = 3; failure <= 4; ++failure) {
    Fixture changed;
    changed.image.fail_at = failure;
    const auto incomplete = nc::disable_owned_view_aa(changed.view, changed.image);
    require(!incomplete.complete && incomplete.write_attempted, "failed post-write read reported success");
    require(changed.view_memory[48] & 1, "post-write failure opened gate");
  }
  Fixture changed;
  changed.image.change_at = 4;
  const auto incomplete = nc::disable_owned_view_aa(changed.view, changed.image);
  require(!incomplete.complete && incomplete.write_attempted, "changing override accepted");
  Fixture global_clear;
  global_clear.image.overrides = {nc::kViewAaFlag, nc::kViewAaFlag};
  require(nc::disable_owned_view_aa(global_clear.view, global_clear.image).complete, "clear override precedence ignored");
}
}  // namespace
int main() {
  try {
    success(0);
    success(4096 - 56);  // The two flag words straddle writable pages.
    refusals();
    std::printf("View AA guard tests passed: %u checks\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
