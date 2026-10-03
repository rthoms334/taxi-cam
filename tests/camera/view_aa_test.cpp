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
  std::int32_t aa_mode = 7;
  std::uint8_t frame_generation = 0;
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
    if (reads != fail_at && rva == nc::kObservedAaModeRva && size == sizeof(aa_mode)) {
      std::memcpy(output, &aa_mode, sizeof(aa_mode));
      return true;
    }
    if (reads != fail_at && rva == nc::kObservedFrameGenerationRva && size == sizeof(frame_generation)) {
      std::memcpy(output, &frame_generation, sizeof(frame_generation));
      return true;
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
// The development switch's direction: exactly bit31 is set, P+56 and every
// other bit (the closed gate included) stay, a set bit is not rewritten, and a
// clear afterwards returns the original words.
void set_direction(unsigned offset) {
  Fixture fixture(offset);
  const auto original = fixture.view.flags;
  fixture.view.flags[0] &= ~nc::kViewAaFlag;
  fixture.save();
  nc::LocalMemoryMetrics metrics;
  const auto result = [&] {
    nc::ScopedLocalMemoryMetrics measured(metrics);
    return nc::set_owned_view_aa(fixture.view, fixture.image, nc::observed_store_layout(), true);
  }();
  require(result.complete && result.write_attempted && !*result.error, "AA set failed");
  accounting(metrics);
  require(metrics.read_calls == 2 && metrics.requested_bytes == 32, "AA set flag reads were uncounted or widened");
  auto expected = fixture.before;
  std::memcpy(expected.data() + 48, original.data(), 16);
  require(!std::memcmp(expected.data(), fixture.view_memory, expected.size()), "AA set changed bits outside bit31");
  require(fixture.image.reads == 4, "AA set override bracket incomplete");
  fixture.view.flags = original;
  fixture.save();
  const auto repeated = nc::set_owned_view_aa(fixture.view, fixture.image, nc::observed_store_layout(), true);
  require(repeated.complete && !repeated.write_attempted && fixture.image.reads == 8, "already-set AA view rewritten");
  fixture.unchanged();
  require(nc::disable_owned_view_aa(fixture.view, fixture.image).complete, "AA clear after a set failed");
  std::array<std::uint64_t, 2> words{};
  std::memcpy(words.data(), fixture.view_memory + 48, 16);
  require(words[0] == (original[0] & ~nc::kViewAaFlag) && words[1] == original[1], "AA clear after a set changed other bits");
  // The set override alone already shows the bit; setting P+48 too is allowed.
  Fixture forced;
  forced.view.flags[0] &= ~nc::kViewAaFlag;
  forced.save();
  forced.image.overrides = {0, nc::kViewAaFlag};
  const auto with_set = nc::set_owned_view_aa(forced.view, forced.image, nc::observed_store_layout(), true);
  require(with_set.complete && with_set.write_attempted, "AA set refused under a set-only override");
}
void set_refusals() {
  const auto layout = nc::observed_store_layout();
  // Every fixture starts with bit31 clear, so an accepted call would write.
  const auto cleared = [](Fixture& fixture) {
    fixture.view.flags[0] &= ~nc::kViewAaFlag;
    fixture.save();
  };
  const auto refused = [&](Fixture& fixture, const char* error, const char* message) {
    const auto result = nc::set_owned_view_aa(fixture.view, fixture.image, layout, true);
    require(!result.complete && !result.write_attempted && !std::strcmp(result.error, error), message);
    fixture.unchanged();
  };
  {
    Fixture fixture;
    cleared(fixture);
    fixture.image.overrides = {nc::kViewAaFlag, 0};
    refused(fixture, "aa_cleared_by_global_override", "AA set accepted a clearing global override");
  }
  {
    Fixture fixture;
    cleared(fixture);
    fixture.image.overrides = {nc::kViewAaFlag, nc::kViewAaFlag};
    refused(fixture, "aa_cleared_by_global_override", "AA set ignored clear override precedence");
  }
  {
    Fixture fixture;
    cleared(fixture);
    fixture.view.flags[0] &= ~1ull;
    fixture.save();
    refused(fixture, "aa_gate_open", "AA set accepted an open gate");
    require(fixture.image.reads == 0, "AA set with an open gate reached override reads");
  }
  for (const unsigned word : {48u, 56u}) {
    Fixture fixture;
    cleared(fixture);
    fixture.view_memory[word] ^= 4;
    std::memcpy(fixture.before.data(), fixture.view_memory, fixture.before.size());
    refused(fixture, "aa_snapshot_changed", "AA set accepted a changed snapshot");
  }
  {
    Fixture fixture;
    cleared(fixture);
    fixture.view.mode = 0;
    refused(fixture, "aa_invalid_owned_view", "AA set accepted an unverified view");
  }
  for (const unsigned offset : {0u, 4096u - 56u}) {
    Fixture fixture(offset);
    cleared(fixture);
    // In the straddling case only P+56's page is read-only.
    auto* protected_page = fixture.allocation + (offset ? 4096 : 0);
    DWORD previous = 0;
    require(VirtualProtect(protected_page, 4096, PAGE_READONLY, &previous) != FALSE, "AA set protection setup failed");
    const auto result = nc::set_owned_view_aa(fixture.view, fixture.image, layout, true);
    require(VirtualProtect(protected_page, 4096, PAGE_READWRITE, &previous) != FALSE, "AA set protection restore failed");
    require(!result.complete && !result.write_attempted && !std::strcmp(result.error, "aa_flags_not_writable") && fixture.image.reads == 0,
            "AA set accepted an unwritable flag span");
    fixture.unchanged();
  }
  Fixture changed;
  cleared(changed);
  changed.image.change_at = 4;
  const auto incomplete = nc::set_owned_view_aa(changed.view, changed.image, layout, true);
  require(!incomplete.complete && incomplete.write_attempted && !std::strcmp(incomplete.error, "aa_changed_during_update"),
          "AA set accepted an override that changed during the update");
  require(changed.view_memory[48] & 1, "AA set failure opened the gate");
}
// A view kept open every update (continuous schedule) that already carries the
// requested bit is confirmed without a write; a change still needs the gate closed.
void open_gate_unchanged() {
  const auto layout = nc::observed_store_layout();
  for (const bool enabled : {true, false}) {
    Fixture fixture;
    fixture.view.flags[0] &= ~1ull;
    if (enabled)
      fixture.view.flags[0] |= nc::kViewAaFlag;
    else
      fixture.view.flags[0] &= ~nc::kViewAaFlag;
    fixture.save();
    const auto kept = nc::set_owned_view_aa(fixture.view, fixture.image, layout, enabled);
    require(kept.complete && !kept.write_attempted && !*kept.error && fixture.image.reads == 4,
            "An open gate already in the requested AA state was refused or rewritten");
    fixture.unchanged();
    const auto changed = nc::set_owned_view_aa(fixture.view, fixture.image, layout, !enabled);
    require(!changed.complete && !changed.write_attempted && !std::strcmp(changed.error, "aa_gate_open") && fixture.image.reads == 4,
            "An open gate accepted an AA change");
    fixture.unchanged();
  }
  // Already set under a clearing global override still refuses an AA request.
  Fixture overridden;
  overridden.view.flags[0] &= ~1ull;
  overridden.view.flags[0] |= nc::kViewAaFlag;
  overridden.save();
  overridden.image.overrides = {nc::kViewAaFlag, 0};
  const auto refused = nc::set_owned_view_aa(overridden.view, overridden.image, layout, true);
  require(!refused.complete && !refused.write_attempted && !std::strcmp(refused.error, "aa_cleared_by_global_override"),
          "An open gate accepted AA under a clearing global override");
  overridden.unchanged();
}
void global_aa_mode() {
  Image image;
  require(nc::read_global_aa_mode(image, nc::observed_store_layout()) == 7 && image.reads == 1, "AA mode was not read");
  auto moved = nc::observed_store_layout();
  moved.view_flag_set_override += 0x20000;
  require(nc::read_global_aa_mode(image, moved) == -1 && image.reads == 1, "AA mode was read for another image layout");
  image.fail_at = 2;
  require(nc::read_global_aa_mode(image, nc::observed_store_layout()) == -1, "A refused AA mode read reported a value");
}
// Development values: only '4' and '5' clear bit36, always with the AA bit; a
// preparation clears it only with the AA bit and permission, else restores an
// owed view or keeps the engine's state. Only bits 31 and 36 ever change.
void bit36_requests() {
  using B = nc::ViewBit36;
  require(nc::view_aa_request(true, true, false).bit36 == B::clear && nc::view_aa_request(true, true, true).bit36 == B::clear,
          "A bit36 clear request was lost");
  require(nc::view_aa_request(false, true, false).bit36 == B::keep && nc::view_aa_request(false, true, true).bit36 == B::restore,
          "bit36 was cleared without the AA bit");
  require(
      nc::view_aa_request(true, true, true, false).bit36 == B::restore && nc::view_aa_request(true, true, false, false).bit36 == B::keep,
      "A deferred bit36 clear still cleared");
  require(nc::view_aa_request(true, false, false).bit36 == B::keep && nc::view_aa_request(false, false, true).bit36 == B::restore,
          "An owed bit36 was not restored");
  const std::uint64_t word = 0x0019e013ff2220efull;
  for (const bool aa : {false, true})
    for (const auto bit36 : {B::keep, B::clear, B::restore})
      for (const std::uint64_t start : {word, word & ~nc::kViewDirectOutputFlag, word & ~nc::kViewAaFlag}) {
        const auto result = nc::requested_view_flags(start, {aa, bit36});
        require(((result ^ start) & ~(nc::kViewAaFlag | nc::kViewDirectOutputFlag)) == 0, "A request changed another flag bit");
        require(((result & nc::kViewAaFlag) != 0) == aa, "A request left the wrong AA bit");
        require(bit36 == B::keep ? ((result ^ start) & nc::kViewDirectOutputFlag) == 0
                                 : ((result & nc::kViewDirectOutputFlag) != 0) == (bit36 == B::restore),
                "A request left the wrong bit36");
      }
}
// '4'/'5' from AA on: exactly bit36 clears in one write, P+56 and every other
// bit stay, a cleared bit is not rewritten, and a restore returns the words.
void bit36_clear_restore(unsigned offset) {
  Fixture fixture(offset);
  const auto original = fixture.view.flags;
  require((original[0] & nc::kViewDirectOutputFlag) && (original[0] & nc::kViewAaFlag), "bit36 fixture lacks the engine's bits");
  nc::LocalMemoryMetrics metrics;
  const auto result = [&] {
    nc::ScopedLocalMemoryMetrics measured(metrics);
    return nc::set_owned_view_aa(fixture.view, fixture.image, nc::observed_store_layout(), {true, nc::ViewBit36::clear});
  }();
  require(result.complete && result.write_attempted && result.changed == nc::kViewDirectOutputFlag && !*result.error, "bit36 clear failed");
  accounting(metrics);
  require(metrics.read_calls == 2 && metrics.requested_bytes == 32 && fixture.image.reads == 5, "bit36 clear skipped its read brackets");
  auto expected = fixture.before;
  auto cleared = original;
  cleared[0] &= ~nc::kViewDirectOutputFlag;
  std::memcpy(expected.data() + 48, cleared.data(), 16);
  require(!std::memcmp(expected.data(), fixture.view_memory, expected.size()), "bit36 clear changed other bytes");
  fixture.view.flags = cleared;
  fixture.save();
  const auto repeated = nc::set_owned_view_aa(fixture.view, fixture.image, nc::observed_store_layout(), {true, nc::ViewBit36::clear});
  require(repeated.complete && !repeated.write_attempted && !repeated.changed && fixture.image.reads == 10,
          "A cleared bit36 was rewritten");
  fixture.unchanged();
  // The bool overload keeps bit36 cleared: '1' after '4' without an owed restore.
  require(nc::set_owned_view_aa(fixture.view, fixture.image, nc::observed_store_layout(), true).complete, "AA keep failed");
  fixture.unchanged();
  const auto restored = nc::set_owned_view_aa(fixture.view, fixture.image, nc::observed_store_layout(), {true, nc::ViewBit36::restore});
  require(restored.complete && restored.changed == nc::kViewDirectOutputFlag, "bit36 restore failed");
  std::array<std::uint64_t, 2> words{};
  std::memcpy(words.data(), fixture.view_memory + 48, 16);
  require(words == original, "bit36 restore did not return the original words");
}
// From the default (bit31 cleared) to '4' and back to '0': one write each, both bits.
void bit36_combined() {
  Fixture fixture;
  const auto original = fixture.view.flags;
  fixture.view.flags[0] &= ~nc::kViewAaFlag;
  fixture.save();
  const auto engine_state = fixture.view.flags;
  const auto both = nc::kViewAaFlag | nc::kViewDirectOutputFlag;
  const auto on = nc::set_owned_view_aa(fixture.view, fixture.image, nc::observed_store_layout(), {true, nc::ViewBit36::clear});
  require(on.complete && on.changed == both && fixture.image.reads == 5, "AA on with bit36 cleared was not one write");
  std::array<std::uint64_t, 2> words{};
  std::memcpy(words.data(), fixture.view_memory + 48, 16);
  require(words[0] == ((original[0] | nc::kViewAaFlag) & ~nc::kViewDirectOutputFlag) && words[1] == original[1],
          "AA on with bit36 cleared left the wrong words");
  fixture.view.flags = words;
  const auto off = nc::set_owned_view_aa(fixture.view, fixture.image, nc::observed_store_layout(), {false, nc::ViewBit36::restore});
  require(off.complete && off.changed == both, "Returning to the default was not one write");
  std::memcpy(words.data(), fixture.view_memory + 48, 16);
  require(words == engine_state, "Returning to the default left the wrong words");
}
void bit36_refusals() {
  const auto layout = nc::observed_store_layout();
  const nc::ViewAaRequest clear{true, nc::ViewBit36::clear};
  const auto refused = [&](Fixture& fixture, nc::ViewAaRequest request, const char* error, unsigned reads, const char* message) {
    const auto result = nc::set_owned_view_aa(fixture.view, fixture.image, layout, request);
    require(
        !result.complete && !result.write_attempted && !result.changed && !std::strcmp(result.error, error) && fixture.image.reads == reads,
        message);
    fixture.unchanged();
  };
  {
    Fixture fixture;
    refused(fixture, {false, nc::ViewBit36::clear}, "bit36_without_aa", 0, "bit36 cleared without the AA bit");
  }
  {
    auto moved = layout;
    moved.view_flag_set_override += 0x20000;
    Fixture fixture;
    fixture.image.layout = moved;
    const auto result = nc::set_owned_view_aa(fixture.view, fixture.image, moved, clear);
    require(!result.complete && !result.write_attempted && !std::strcmp(result.error, "bit36_unverified_image") && fixture.image.reads == 0,
            "bit36 cleared on an image it was not decoded on");
    fixture.unchanged();
    // A hand-back stays possible there.
    fixture.view.flags[0] &= ~nc::kViewDirectOutputFlag;
    fixture.save();
    require(nc::set_owned_view_aa(fixture.view, fixture.image, moved, {true, nc::ViewBit36::restore}).complete,
            "bit36 restore refused on a moved image");
  }
  {
    Fixture fixture;
    fixture.image.overrides = {0, nc::kViewDirectOutputFlag};
    refused(fixture, clear, "bit36_forced_by_global_override", 2, "bit36 cleared under a set-only override");
  }
  {
    // bit31's override refusal keeps its precedence and error.
    Fixture fixture;
    fixture.image.overrides = {nc::kViewAaFlag, nc::kViewDirectOutputFlag};
    refused(fixture, clear, "aa_cleared_by_global_override", 2, "bit31 override lost its precedence");
  }
  for (const auto overrides : {std::array<std::uint64_t, 2>{nc::kViewDirectOutputFlag, nc::kViewDirectOutputFlag},
                               std::array<std::uint64_t, 2>{nc::kViewDirectOutputFlag, 0}}) {
    Fixture fixture;
    fixture.image.overrides = overrides;
    require(nc::set_owned_view_aa(fixture.view, fixture.image, layout, clear).complete, "A clear-override bit36 refused a clear");
  }
  for (const auto overrides :
       {std::array<std::uint64_t, 2>{0, nc::kViewDirectOutputFlag}, std::array<std::uint64_t, 2>{nc::kViewDirectOutputFlag, 0}}) {
    Fixture fixture;
    fixture.view.flags[0] &= ~nc::kViewDirectOutputFlag;
    fixture.save();
    fixture.image.overrides = overrides;
    const auto result = nc::set_owned_view_aa(fixture.view, fixture.image, layout, {true, nc::ViewBit36::restore});
    require(result.complete && result.changed == nc::kViewDirectOutputFlag, "A global override refused a bit36 hand-back");
  }
  {
    Fixture fixture;
    fixture.view.flags[0] &= ~1ull;
    fixture.save();
    refused(fixture, clear, "aa_gate_open", 0, "bit36 cleared on an open gate");
  }
  {
    Fixture fixture;
    fixture.view.flags[0] &= ~(1ull | nc::kViewDirectOutputFlag);
    fixture.save();
    refused(fixture, {false, nc::ViewBit36::restore}, "aa_gate_open", 0, "bit36 restored on an open gate");
    // Continuous '4': already in the requested state, confirmed without a write.
    const auto kept = nc::set_owned_view_aa(fixture.view, fixture.image, layout, clear);
    require(kept.complete && !kept.write_attempted && fixture.image.reads == 5, "An open '4' view was refused or rewritten");
    fixture.unchanged();
  }
  {
    Fixture fixture;
    fixture.view_memory[52] ^= 0x10;  // bit36 of P+48 changed since the snapshot
    std::memcpy(fixture.before.data(), fixture.view_memory, fixture.before.size());
    refused(fixture, clear, "aa_snapshot_changed", 2, "bit36 cleared over a changed snapshot");
  }
  for (const unsigned offset : {0u, 4096u - 56u}) {
    Fixture fixture(offset);
    auto* protected_page = fixture.allocation + (offset ? 4096 : 0);
    DWORD previous = 0;
    require(VirtualProtect(protected_page, 4096, PAGE_READONLY, &previous) != FALSE, "bit36 protection setup failed");
    const auto result = nc::set_owned_view_aa(fixture.view, fixture.image, layout, clear);
    require(VirtualProtect(protected_page, 4096, PAGE_READWRITE, &previous) != FALSE, "bit36 protection restore failed");
    require(!result.complete && !result.write_attempted && !std::strcmp(result.error, "aa_flags_not_writable") && fixture.image.reads == 0,
            "bit36 cleared over an unwritable span");
    fixture.unchanged();
  }
  Fixture changed;
  changed.image.change_at = 4;
  const auto incomplete = nc::set_owned_view_aa(changed.view, changed.image, layout, clear);
  require(!incomplete.complete && incomplete.write_attempted && incomplete.changed == nc::kViewDirectOutputFlag &&
              !std::strcmp(incomplete.error, "aa_changed_during_update") && (changed.view_memory[48] & 1),
          "bit36 clear accepted a changed override or opened the gate");
  Fixture reread;
  reread.image.fail_at = 4;  // The first override reread, after the frame-generation read.
  const auto unread = nc::set_owned_view_aa(reread.view, reread.image, layout, clear);
  require(!unread.complete && unread.write_attempted && (unread.changed & nc::kViewDirectOutputFlag),
          "A failed reread hid its bit36 write");
}
// Frame generation: a view without bit36 copies the global mode, so a clear
// (or an open view confirmed cleared) needs the byte to read 0 on the decoded
// image. Bit31 alone and the bit36 hand-back never depend on it.
void bit36_frame_generation() {
  const auto layout = nc::observed_store_layout();
  {
    Image image;
    image.frame_generation = 1;
    require(nc::read_global_frame_generation(image, layout) == 1 && image.reads == 1, "Frame generation was not read");
    auto moved = layout;
    moved.view_flag_set_override += 0x20000;
    require(nc::read_global_frame_generation(image, moved) == -1 && image.reads == 1, "Frame generation was read on another image layout");
    image.fail_at = 2;
    require(nc::read_global_frame_generation(image, layout) == -1, "A refused frame-generation read reported a value");
  }
  for (const std::uint8_t mode : {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{0x80}}) {
    Fixture fixture;
    fixture.image.frame_generation = mode;
    const auto result = nc::set_owned_view_aa(fixture.view, fixture.image, layout, {true, nc::ViewBit36::clear});
    require(!result.complete && !result.write_attempted && !result.changed && !std::strcmp(result.error, "bit36_frame_generation_on") &&
                fixture.image.reads == 3,
            "bit36 cleared while frame generation was on");
    fixture.unchanged();
    // The same preparation without the clear: bit31 and the hand-back proceed.
    require(nc::set_owned_view_aa(fixture.view, fixture.image, layout, {true, nc::ViewBit36::keep}).complete,
            "Frame generation refused the AA bit");
    fixture.unchanged();
    fixture.view.flags[0] &= ~nc::kViewDirectOutputFlag;
    fixture.save();
    const auto restored = nc::set_owned_view_aa(fixture.view, fixture.image, layout, {true, nc::ViewBit36::restore});
    require(restored.complete && restored.changed == nc::kViewDirectOutputFlag, "Frame generation refused a bit36 hand-back");
  }
  {
    // An unreadable byte is not "off".
    Fixture fixture;
    fixture.image.fail_at = 3;
    const auto result = nc::set_owned_view_aa(fixture.view, fixture.image, layout, {true, nc::ViewBit36::clear});
    require(!result.complete && !result.write_attempted && !std::strcmp(result.error, "bit36_frame_generation_on"),
            "bit36 cleared over an unreadable frame-generation byte");
    fixture.unchanged();
  }
  {
    // A continuous '4' view already cleared is not confirmed while it is on.
    Fixture fixture;
    fixture.view.flags[0] &= ~(1ull | nc::kViewDirectOutputFlag);
    fixture.save();
    fixture.image.frame_generation = 1;
    const auto result = nc::set_owned_view_aa(fixture.view, fixture.image, layout, {true, nc::ViewBit36::clear});
    require(!result.complete && !result.write_attempted && !std::strcmp(result.error, "bit36_frame_generation_on"),
            "An open cleared view was confirmed with frame generation on");
    fixture.unchanged();
  }
}
// Hand-backs: only the owed bits return, one write for both, closed gate only.
void restore_flags() {
  const auto both = nc::kViewAaFlag | nc::kViewDirectOutputFlag;
  Fixture fixture;
  const auto original = fixture.view.flags;
  auto cleared = original;
  cleared[0] &= ~both;
  fixture.view.flags = cleared;
  fixture.save();
  // The bit31 restore never hands back bit36.
  const auto aa_only = nc::restore_view_aa_flag(fixture.view.view_address);
  require(aa_only.complete && aa_only.changed == nc::kViewAaFlag, "bit31 restore failed");
  std::array<std::uint64_t, 2> words{};
  std::memcpy(words.data(), fixture.view_memory + 48, 16);
  require(words[0] == (cleared[0] | nc::kViewAaFlag) && words[1] == original[1], "bit31 restore set bit36");
  fixture.view.flags = words;
  fixture.save();
  const auto bit36 = nc::restore_view_flags(fixture.view.view_address, nc::kViewDirectOutputFlag);
  require(bit36.complete && bit36.changed == nc::kViewDirectOutputFlag, "bit36 restore failed");
  std::memcpy(words.data(), fixture.view_memory + 48, 16);
  require(words == original, "bit36 restore did not return the words");
  const auto again = nc::restore_view_flags(fixture.view.view_address, both);
  require(again.complete && !again.write_attempted && !again.changed, "Set bits were rewritten");
  Fixture together;
  together.view.flags[0] &= ~both;
  together.save();
  const auto one = nc::restore_view_flags(together.view.view_address, both);
  require(one.complete && one.changed == both, "Both owed bits were not one write");
  std::memcpy(words.data(), together.view_memory + 48, 16);
  require(words == original, "Both owed bits did not return the words");
  Fixture only36;
  only36.view.flags[0] &= ~both;
  only36.save();
  require(nc::restore_view_flags(only36.view.view_address, nc::kViewDirectOutputFlag).complete, "bit36 alone failed");
  std::memcpy(words.data(), only36.view_memory + 48, 16);
  require(!(words[0] & nc::kViewAaFlag), "A bit36 restore set bit31");
  for (const std::uint64_t bad : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{1} << 5, both | 1u, std::uint64_t{1} << 37}) {
    Fixture refused;
    refused.view.flags[0] &= ~both;
    refused.save();
    const auto result = nc::restore_view_flags(refused.view.view_address, bad);
    require(!result.complete && !result.write_attempted && !std::strcmp(result.error, "aa_invalid_restore_bits"),
            "Other bits were restored");
    refused.unchanged();
  }
  Fixture open;
  open.view.flags[0] &= ~(1ull | both);
  open.save();
  const auto refused = nc::restore_view_flags(open.view.view_address, nc::kViewDirectOutputFlag);
  require(!refused.complete && !refused.write_attempted && !std::strcmp(refused.error, "aa_gate_open"),
          "Open gate accepted a bit36 restore");
  open.unchanged();
  // Two independent ledgers: each owes only its own bit, per renderer.
  nc::ClearedAaLedger aa, b36;
  require(aa.note(0x2000, 0x1000) && b36.note(0x2000, 0x1000) && b36.note(0x2000, 0x1008), "Ledgers refused views");
  require(nc::owed_view_flags(aa, b36, 0x2000, 0x1000) == both &&
              nc::owed_view_flags(aa, b36, 0x2000, 0x1008) == nc::kViewDirectOutputFlag && !nc::owed_view_flags(aa, b36, 0x3000, 0x1000) &&
              !nc::owed_view_flags(aa, b36, 0x2000, 0x1010),
          "Owed bits did not follow the ledgers");
  b36.forget(0x1000);
  require(nc::owed_view_flags(aa, b36, 0x2000, 0x1000) == nc::kViewAaFlag && aa.contains(0x2000, 0x1000), "Forgetting bit36 lost bit31");
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
    set_direction(0);
    set_direction(4096 - 56);
    set_refusals();
    open_gate_unchanged();
    global_aa_mode();
    bit36_requests();
    bit36_clear_restore(0);
    bit36_clear_restore(4096 - 56);
    bit36_combined();
    bit36_refusals();
    bit36_frame_generation();
    restore_flags();
    std::printf("View AA guard tests passed: %u checks\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
