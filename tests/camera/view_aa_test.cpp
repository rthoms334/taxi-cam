#include "../../src/camera/view_aa.hpp"
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "../../src/camera/local_memory.hpp"

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
  nc::CameraImageLayout layout = nc::observed_store_layout();
  std::array<std::uint64_t, 2> overrides{4, 0};
  unsigned reads = 0, fail_at = 0, change_at = 0;
  unsigned protect_at = 0;
  void* protect_address = nullptr;
  DWORD protection = PAGE_NOACCESS;
  taxi_camera::discovery::ReadWindow query(std::uint32_t, std::uint32_t) override { return {}; }
  bool read(std::uint32_t rva, void* output, std::size_t size) override {
    ++reads;
    if (reads == protect_at) {
      DWORD previous = 0;
      require(VirtualProtect(protect_address, 4096, protection, &previous) != FALSE, "override-time protection change failed");
    }
    if (reads == fail_at || size != 8 || (rva != layout.view_flag_clear_override && rva != layout.view_flag_set_override))
      return false;
    auto value = overrides[rva == layout.view_flag_set_override];
    if (reads == change_at)
      value ^= 16;
    std::memcpy(output, &value, 8);
    return true;
  }
};
struct Fixture {
  unsigned char* allocation = nullptr;
  unsigned char* view_memory = nullptr;
  ec::OwnedViewSnapshot view;
  Image image;
  std::array<unsigned char, 128> before{};
  explicit Fixture(unsigned offset = 0, DWORD protection = PAGE_READWRITE)
      : allocation(static_cast<unsigned char*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, protection))) {
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
void accounting(const nc::LocalMemoryMetrics& metrics) {
  require(metrics.query_calls == metrics.query_allocation_calls + metrics.query_page_calls + metrics.query_fallback_calls,
          "AA query families did not sum to the total");
  require(metrics.query_calls > 0, "AA mapping queries bypassed local-memory metrics");
}
void success(unsigned offset) {
  Fixture fixture(offset);
  require(VirtualLock(fixture.allocation, 8192) != FALSE, "could not pin the small AA fixture");
  nc::LocalMemoryMetrics metrics;
  const auto result = [&] {
    nc::ScopedLocalMemoryMetrics measured(metrics);
    return nc::disable_owned_view_aa(fixture.view, fixture.image);
  }();
  require(result.complete && result.write_attempted && !*result.error, "AA disable failed");
  accounting(metrics);
  require(metrics.query_allocation_calls > 0 && metrics.query_page_calls > 0 && metrics.query_fallback_calls == 0,
          "resident AA flags did not use bounded page validation");
  // The mock image owns its override reads. Only the two exact flag reads count.
  require(metrics.read_calls == 2 && metrics.requested_bytes == 32, "AA flag reads were uncounted or widened");
  auto expected = fixture.before;
  auto flags = fixture.view.flags;
  flags[0] &= ~nc::kViewAaFlag;
  std::memcpy(expected.data() + 48, flags.data(), 16);
  require(!std::memcmp(expected.data(), fixture.view_memory, expected.size()), "changed bits outside AA flag");
  require((flags[0] & 1) && flags[1] == fixture.view.flags[1], "gate or second flag word changed");
  require(fixture.image.reads == 4, "override bracket incomplete");
  fixture.view.flags = flags;
  fixture.save();
  metrics = {};
  const auto repeated = [&] {
    nc::ScopedLocalMemoryMetrics measured(metrics);
    return nc::disable_owned_view_aa(fixture.view, fixture.image);
  }();
  require(repeated.complete && !repeated.write_attempted, "already-disabled view rewritten");
  accounting(metrics);
  require(metrics.read_calls == 2 && metrics.requested_bytes == 32, "already-disabled AA skipped its exact read bracket");
  require(fixture.image.reads == 8, "already-disabled AA skipped its override bracket");
  fixture.unchanged();
  require(VirtualUnlock(fixture.allocation, 8192) != FALSE, "could not unpin the AA fixture");
}
void protected_flags() {
  for (const DWORD protection : {DWORD(PAGE_READONLY), DWORD(PAGE_NOACCESS), DWORD(PAGE_READWRITE | PAGE_GUARD), DWORD(PAGE_EXECUTE_READ),
                                 DWORD(PAGE_EXECUTE_READWRITE)}) {
    for (const unsigned offset : {0u, 4096u - 56u}) {
      for (const bool already_clear : {false, true}) {
        Fixture fixture(offset);
        if (already_clear) {
          fixture.view.flags[0] &= ~nc::kViewAaFlag;
          fixture.save();
        }
        // In the straddling case only P+56 is protected: the first word alone
        // must never authorize the write or the no-op success path.
        auto* protected_page = fixture.allocation + (offset ? 4096 : 0);
        DWORD previous = 0;
        require(VirtualProtect(protected_page, 4096, protection, &previous) != FALSE, "AA protection setup failed");
        nc::LocalMemoryMetrics metrics;
        const auto result = [&] {
          nc::ScopedLocalMemoryMetrics measured(metrics);
          return nc::disable_owned_view_aa(fixture.view, fixture.image);
        }();
        require(!result.complete && !result.write_attempted && std::strcmp(result.error, "aa_flags_not_writable") == 0,
                "non-RW AA flags accepted");
        require(fixture.image.reads == 0 && metrics.read_calls == 0 && metrics.requested_bytes == 0,
                "invalid AA mapping reached object or override reads");
        accounting(metrics);
        MEMORY_BASIC_INFORMATION observed{};
        require(VirtualQuery(protected_page, &observed, sizeof(observed)) == sizeof(observed) && observed.Protect == protection,
                "AA validation consumed a guard or changed protection");
        require(VirtualProtect(protected_page, 4096, PAGE_READWRITE, &previous) != FALSE, "AA protection restore failed");
        fixture.unchanged();
      }
    }
  }
  for (const DWORD modifier : {DWORD(PAGE_NOCACHE), DWORD(PAGE_WRITECOMBINE)}) {
    Fixture fixture(0, PAGE_READWRITE | modifier);
    MEMORY_BASIC_INFORMATION observed{};
    require(VirtualQuery(fixture.allocation, &observed, sizeof(observed)) == sizeof(observed) &&
                observed.Protect == (PAGE_READWRITE | modifier),
            "AA protection modifier fixture was not established");
    const auto result = nc::disable_owned_view_aa(fixture.view, fixture.image);
    require(!result.complete && !result.write_attempted && fixture.image.reads == 0, "AA accepted an RW protection modifier");
    fixture.unchanged();
  }
}
void invalid_allocation_types() {
  struct Mapping {
    HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 8192, nullptr);
    void* view = nullptr;
    ~Mapping() {
      if (view)
        UnmapViewOfFile(view);
      if (handle)
        CloseHandle(handle);
    }
  } mapping;
  require(mapping.handle != nullptr, "AA mapping fixture creation failed");
  for (const DWORD access : {DWORD(FILE_MAP_READ | FILE_MAP_WRITE), DWORD(FILE_MAP_COPY)}) {
    mapping.view = MapViewOfFile(mapping.handle, access, 0, 0, 8192);
    require(mapping.view != nullptr, "AA mapping fixture view failed");
    Fixture fixture;
    fixture.view.view_address = reinterpret_cast<std::uintptr_t>(mapping.view);
    // Writing the copy-on-write view makes that page private to this process;
    // it still belongs to a mapped allocation and must remain refused.
    std::memcpy(static_cast<unsigned char*>(mapping.view) + 48, fixture.view.flags.data(), 16);
    std::array<unsigned char, 128> before{};
    std::memcpy(before.data(), mapping.view, before.size());
    const auto result = nc::disable_owned_view_aa(fixture.view, fixture.image);
    require(!result.complete && !result.write_attempted && fixture.image.reads == 0, "AA accepted mapped or copy-on-write flags");
    require(std::memcmp(before.data(), mapping.view, before.size()) == 0, "AA refusal modified mapped flags");
    require(UnmapViewOfFile(mapping.view) != FALSE, "AA mapping fixture unmap failed");
    mapping.view = nullptr;
  }
  Fixture decommitted(4096 - 56);
  require(VirtualFree(decommitted.allocation + 4096, 4096, MEM_DECOMMIT) != FALSE, "AA decommit fixture failed");
  const auto result = nc::disable_owned_view_aa(decommitted.view, decommitted.image);
  require(!result.complete && !result.write_attempted && decommitted.image.reads == 0, "AA accepted a decommitted second flag word");
  require(std::memcmp(decommitted.view_memory + 48, decommitted.before.data() + 48, 8) == 0,
          "AA modified the first word before rejecting the second");
}
void access_changes_during_update() {
  for (const bool already_clear : {false, true}) {
    Fixture fixture;
    if (already_clear) {
      fixture.view.flags[0] &= ~nc::kViewAaFlag;
      fixture.save();
    }
    // The second initial override read changes access after the first flag
    // read. Both the attempted write and the no-op's reread must fail safely.
    fixture.image.protect_at = 2;
    fixture.image.protect_address = fixture.allocation;
    nc::LocalMemoryMetrics metrics;
    const auto result = [&] {
      nc::ScopedLocalMemoryMetrics measured(metrics);
      return nc::disable_owned_view_aa(fixture.view, fixture.image);
    }();
    require(!result.complete && result.write_attempted == !already_clear, "AA accepted flags made inaccessible during update");
    require(std::strcmp(result.error, already_clear ? "aa_recheck_failed" : "aa_write_failed") == 0,
            "AA changed-access failure was misclassified");
    require(metrics.read_calls == (already_clear ? 2u : 1u) && metrics.requested_bytes == (already_clear ? 32u : 16u),
            "AA failed access attempts were uncounted or widened");
    DWORD previous = 0;
    require(VirtualProtect(fixture.allocation, 4096, PAGE_READWRITE, &previous) != FALSE, "AA changed-access restore failed");
    fixture.unchanged();
  }
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
void resolved_layout() {
  auto layout = nc::observed_store_layout();
  layout.view_flag_clear_override += 0x10000;
  layout.view_flag_set_override += 0x20000;
  Fixture moved;
  moved.image.layout = layout;
  const auto result = nc::disable_owned_view_aa(moved.view, moved.image, layout);
  require(result.complete && result.write_attempted && moved.image.reads == 4,
          "Resolved AA globals did not preserve the complete override bracket");
  Fixture mismatch;
  mismatch.image.layout = layout;
  require(!nc::disable_owned_view_aa(mismatch.view, mismatch.image).write_attempted,
          "Legacy AA globals were used after image addresses moved");
  mismatch.unchanged();
  for (const auto bad : {nc::CameraImageLayout{},
                         [&] {
                           auto value = layout;
                           value.view_flag_set_override = UINT32_MAX;
                           return value;
                         }(),
                         [&] {
                           auto value = layout;
                           value.view_flag_set_override = value.view_flag_clear_override;
                           return value;
                         }()}) {
    Fixture invalid;
    const auto refused = nc::disable_owned_view_aa(invalid.view, invalid.image, bad);
    require(!refused.complete && !refused.write_attempted && invalid.image.reads == 0, "Malformed AA layout reached reads or writes");
    invalid.unchanged();
  }
}
void restore_after_clear() {
  // Clear, then hand the pooled view back: exactly bit31 returns, P+56 and every
  // other bit are untouched, and a second restore is a no-op without a write.
  Fixture fixture;
  require(nc::disable_owned_view_aa(fixture.view, fixture.image).complete, "AA disable failed before restore");
  std::array<std::uint64_t, 2> words{};
  std::memcpy(words.data(), fixture.view_memory + 48, 16);
  require((words[0] & nc::kViewAaFlag) == 0 && words[1] == fixture.view.flags[1], "AA disable changed unexpected bits");
  const auto restored = nc::restore_view_aa_flag(fixture.view.view_address);
  require(restored.complete && restored.write_attempted && !*restored.error, "AA restore failed");
  std::memcpy(words.data(), fixture.view_memory + 48, 16);
  require(words == fixture.view.flags, "AA restore did not return the original flag words");
  const auto again = nc::restore_view_aa_flag(fixture.view.view_address);
  require(again.complete && !again.write_attempted, "A set AA bit was rewritten");
  require(!std::memcmp(fixture.before.data(), fixture.view_memory, fixture.before.size()), "Restore modified bytes beyond bit31");
  // An open gate (bit0 clear) refuses: only closed views are handed back.
  Fixture open;
  open.view.flags[0] &= ~1ull;
  open.view.flags[0] &= ~nc::kViewAaFlag;
  open.save();
  const auto refused = nc::restore_view_aa_flag(open.view.view_address);
  require(!refused.complete && !refused.write_attempted && !std::strcmp(refused.error, "aa_gate_open"), "Open gate accepted an AA restore");
  open.unchanged();
  // Unaligned, null and unwritable addresses refuse without a write.
  require(!nc::restore_view_aa_flag(0).complete, "Null view restored");
  require(!nc::restore_view_aa_flag(fixture.view.view_address + 4).complete, "Unaligned view restored");
  Fixture readonly;
  DWORD previous = 0;
  require(VirtualProtect(readonly.allocation, 8192, PAGE_READONLY, &previous) != FALSE, "could not protect the AA fixture");
  const auto unwritable = nc::restore_view_aa_flag(readonly.view.view_address);
  require(VirtualProtect(readonly.allocation, 8192, PAGE_READWRITE, &previous) != FALSE, "could not unprotect the AA fixture");
  require(!unwritable.complete && !unwritable.write_attempted && !std::strcmp(unwritable.error, "aa_flags_not_writable"),
          "Read-only view accepted an AA restore");
  // Ledger: one renderer, bounded, exact addresses, forget/pending semantics.
  nc::ClearedAaLedger ledger;
  require(!ledger.pending() && !ledger.note(0, 0x1000) && !ledger.note(0x2000, 0), "Empty identities entered the AA ledger");
  require(ledger.note(0x2000, 0x1000) && ledger.note(0x2000, 0x1000) && ledger.contains(0x2000, 0x1000) && ledger.pending(),
          "AA ledger did not record a cleared view");
  require(!ledger.contains(0x3000, 0x1000), "AA ledger matched a different renderer");
  for (unsigned i = 1; i < nc::ClearedAaLedger::Capacity; ++i)
    require(ledger.note(0x2000, 0x1000 + i * 8), "AA ledger capacity was too small for a pair and its replacement");
  require(!ledger.note(0x2000, 0x9000), "AA ledger accepted more than its capacity");
  ledger.forget(0x1000);
  require(!ledger.contains(0x2000, 0x1000) && ledger.pending(), "AA ledger forget removed the wrong entry");
  require(ledger.note(0x3000, 0x4000) && !ledger.contains(0x2000, 0x1008) && ledger.contains(0x3000, 0x4000),
          "A new renderer did not replace the AA ledger");
}
}  // namespace
int main() {
  try {
    success(0);
    success(4096 - 56);  // The two flag words straddle writable pages.
    protected_flags();
    invalid_allocation_types();
    access_changes_during_update();
    refusals();
    resolved_layout();
    restore_after_clear();
    std::printf("View AA guard tests passed: %u checks\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
