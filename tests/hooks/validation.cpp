#include "../../src/hooks/observer_hook.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
using namespace taxi_camera::engine_hook;

struct alignas(16) RegisterState {
  std::uint64_t rax;
  std::uint64_t rcx;
  std::uint64_t rdx;
  std::uint64_t r8;
  std::uint64_t r9;
  std::uint64_t r10;
  std::uint64_t r11;
  std::uint64_t flags;
  std::array<std::array<std::uint8_t, 16>, 6> xmm;
  std::array<std::uint64_t, 4> stack;
  std::uint64_t rsp;
  std::uint32_t mxcsr;
};

struct Invocation {
  RegisterState requested{};
  RegisterState observed{};
  std::uint64_t calls = 0;
  std::uint64_t returned_rax = 0;
  alignas(16) std::array<std::uint64_t, 2> returned_xmm0{};
};

static_assert(sizeof(RegisterState) == 208 && offsetof(RegisterState, xmm) == 64 && offsetof(RegisterState, stack) == 160 &&
              offsetof(RegisterState, rsp) == 192 && offsetof(RegisterState, mxcsr) == 200);
static_assert(offsetof(Invocation, observed) == 208 && offsetof(Invocation, calls) == 416 && offsetof(Invocation, returned_rax) == 424 &&
              offsetof(Invocation, returned_xmm0) == 432);

extern "C" {
void validation_invoke(Invocation*, void**);
void validation_target();
void validation_observer(void*) noexcept;
void taxi_engine_hook_thunk();
extern const unsigned char taxi_hook_after_push_rbp;
extern const unsigned char taxi_hook_after_push_flags;
extern const unsigned char taxi_hook_after_alloc;
extern const unsigned char taxi_hook_after_frame;
extern const unsigned char taxi_hook_before_restore_flags;
extern const unsigned char taxi_hook_after_restore_push;
extern const unsigned char taxi_hook_after_restore_flags;
extern const unsigned char taxi_hook_after_stack_restore;
extern const unsigned char taxi_hook_after_frame_restore;
}

std::uint64_t observer_calls = 0;
void* last_rcx = nullptr;
bool unwind_seen = false;
DWORD64 invoke_begin = 0;
DWORD64 invoke_end = 0;
void** mock_slot = nullptr;
Invocation* nested = nullptr;

void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

DWORD protection(void* address) {
  MEMORY_BASIC_INFORMATION region{};
  require(VirtualQuery(address, &region, sizeof(region)) == sizeof(region), "Cannot query mock table protection");
  return region.Protect;
}

struct MockTable {
  void** slot = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));

  MockTable() {
    require(slot != nullptr, "Cannot allocate mock table data");
    *slot = reinterpret_cast<void*>(&validation_target);
    DWORD old = 0;
    require(VirtualProtect(slot, 4096, PAGE_READONLY, &old) != FALSE, "Cannot protect mock vtable as read-only");
  }
  ~MockTable() { VirtualFree(slot, 0, MEM_RELEASE); }
  void replace_for_test(void* pointer) {
    DWORD old = 0;
    require(VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old) != FALSE, "Cannot simulate foreign slot owner");
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), pointer);
    DWORD discarded = 0;
    require(VirtualProtect(slot, sizeof(void*), old, &discarded) != FALSE, "Cannot restore mock page protection");
  }
};

void prepare(Invocation& invocation, std::uint64_t seed) {
  invocation = {};
  auto& state = invocation.requested;
  state.rax = 0x1122334455667788 ^ seed;
  state.rcx = reinterpret_cast<std::uintptr_t>(&invocation);
  state.rdx = 0x8877665544332211 ^ seed;
  state.r8 = 0xabcdef0123456789 ^ seed;
  state.r9 = 0x9988776655443322 ^ seed;
  state.r10 = 0x7766554433221100 ^ seed;
  state.r11 = 0x123456789abcdef0 ^ seed;
  state.flags = 0x202 | (seed & 0x8d5);  // Ordinary arithmetic flags; DF/TF remain clear.
  state.mxcsr = 0x1f80 | static_cast<std::uint32_t>(seed & 0x3f);
  for (std::size_t i = 0; i < state.xmm.size(); ++i) {
    for (std::size_t j = 0; j < state.xmm[i].size(); ++j)
      state.xmm[i][j] = static_cast<std::uint8_t>(seed + i * 29 + j * 7);
  }
  for (std::size_t i = 0; i < state.stack.size(); ++i)
    state.stack[i] = 0xfedcba9876543210 ^ (seed + i * 97);
}

void verify(const Invocation& invocation) {
  const auto& wanted = invocation.requested;
  const auto& actual = invocation.observed;
  require(invocation.calls == 1, "The original target did not execute exactly once");
  require(std::memcmp(&wanted, &actual, 7 * sizeof(std::uint64_t)) == 0, "A volatile GPR argument was changed");
  require(((wanted.flags ^ actual.flags) & 0x8d5) == 0, "Arithmetic RFLAGS were changed before the original target");
  require(wanted.xmm == actual.xmm, "An XMM0..5 argument was changed");
  require(wanted.stack == actual.stack && wanted.rsp == actual.rsp, "Stack arguments or the original entry RSP were changed");
  require(wanted.mxcsr == actual.mxcsr, "MXCSR status or control bits were changed");
  require(invocation.returned_rax == 0x1020304050607080 &&
              invocation.returned_xmm0 == std::array<std::uint64_t, 2>{0x8899aabbccddeeff, 0x0011223344556677},
          "The original scalar/vector return was changed");
}

void verify_virtual_unwind() {
  DWORD64 image_base = 0;
  const auto function = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(&taxi_engine_hook_thunk), &image_base, nullptr);
  require(function != nullptr, "The thunk has no Win64 runtime-function/unwind entry");
  struct Site {
    const void* address;
    std::uint64_t stack_used;
    bool frame_set;
  };
  const std::array sites{
      Site{reinterpret_cast<const void*>(&taxi_engine_hook_thunk), 0, false},
      Site{&taxi_hook_after_push_rbp, 8, false},
      Site{&taxi_hook_after_push_flags, 16, false},
      Site{&taxi_hook_after_alloc, 232, false},
      Site{&taxi_hook_after_frame, 232, true},
      Site{&taxi_hook_before_restore_flags, 232, true},
      Site{&taxi_hook_after_restore_push, 240, true},
      Site{&taxi_hook_after_restore_flags, 232, true},
      Site{&taxi_hook_after_stack_restore, 8, true},
      Site{&taxi_hook_after_frame_restore, 0, false},
  };
  for (const auto& site : sites) {
    alignas(16) std::array<DWORD64, 128> stack{};
    const auto original_rsp = reinterpret_cast<DWORD64>(&stack[97]);
    constexpr DWORD64 original_rbp = 0x1122334455667788;
    const auto return_address = reinterpret_cast<DWORD64>(&validation_target);
    stack[97] = return_address;
    stack[96] = original_rbp;
    stack[95] = 0x202;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    context.Rip = reinterpret_cast<DWORD64>(site.address);
    context.Rsp = original_rsp - site.stack_used;
    context.Rbp = site.frame_set ? original_rsp - 8 : original_rbp;
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher, nullptr);
    require(context.Rip == return_address && context.Rsp == original_rsp + 8 && context.Rbp == original_rbp,
            "Virtual unwind failed at a prolog, body, flags-restoration or epilogue boundary");
  }
}
}  // namespace

extern "C" void validation_observer_body(void* original_rcx) noexcept {
  ++observer_calls;
  last_rcx = original_rcx;
  void* frames[32]{};
  const auto count = CaptureStackBackTrace(0, 32, frames, nullptr);
  for (USHORT i = 0; i < count; ++i) {
    const auto address = reinterpret_cast<DWORD64>(frames[i]);
    if (address >= invoke_begin && address < invoke_end)
      unwind_seen = true;
  }
  if (nested != nullptr) {
    auto* invocation = nested;
    nested = nullptr;
    validation_invoke(invocation, mock_slot);
  }
}

int main() {
  try {
    MockTable table;
    mock_slot = table.slot;
    const auto original = *table.slot;
    require(protection(table.slot) == PAGE_READONLY, "Mock vtable is not read-only");
    require(remove().status == Status::not_installed, "Removal without installation was accepted");
    require(install(nullptr, original).status == Status::invalid_argument, "Null slot was accepted");
    require(
        install(reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(table.slot) + 1), original).status == Status::invalid_argument,
        "An unaligned slot was accepted");
    require(install(reinterpret_cast<void**>(8), original).status == Status::invalid_slot_memory, "Unmapped slot memory was accepted");
    require(install(table.slot, table.slot).status == Status::invalid_code_pointer, "A data pointer was accepted as executable original");
    require(install(table.slot, reinterpret_cast<void*>(&validation_observer)).status == Status::original_mismatch,
            "A mismatched expected original was accepted");
    require(*table.slot == original && protection(table.slot) == PAGE_READONLY, "A refused install changed the slot or protection");

    Invocation baseline;
    prepare(baseline, 0x895);
    validation_invoke(&baseline, table.slot);
    verify(baseline);
    require(observer_calls == 0, "Observer ran without installation");

    DWORD64 image_base = 0;
    const auto entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(&validation_invoke), &image_base, nullptr);
    require(entry != nullptr, "Mock invocation has no unwind metadata");
    invoke_begin = image_base + entry->BeginAddress;
    invoke_end = image_base + entry->EndAddress;
    const auto installed = install(table.slot, original, &validation_observer);
    require(installed.status == Status::installed && installed.pointer_changed && installed.protection_restored,
            "Expected-original installation failed");
    require(protection(table.slot) == PAGE_READONLY && *table.slot != original,
            "Installation failed to restore protection or hook the slot");
    verify_virtual_unwind();
    for (std::uint64_t seed = 0; seed < 128; ++seed) {
      Invocation invocation;
      prepare(invocation, seed * 97);
      const auto before = observer_calls;
      unwind_seen = false;
      validation_invoke(&invocation, table.slot);
      verify(invocation);
      require(observer_calls == before + 1 && last_rcx == &invocation, "Observer did not receive original RCX exactly once");
      require(unwind_seen, "A real stack capture failed to unwind across the observer thunk to its caller");
    }

    Invocation outer;
    Invocation inner;
    prepare(outer, 0xdead);
    prepare(inner, 0xbeef);
    nested = &inner;
    const auto before_nested = observer_calls;
    validation_invoke(&outer, table.slot);
    verify(outer);
    verify(inner);
    require(observer_calls == before_nested + 1 && last_rcx == &outer, "Reentrant observation was not suppressed");

    const auto thunk = *table.slot;
    table.replace_for_test(original);
    require(remove().status == Status::slot_changed && *table.slot == original && protection(table.slot) == PAGE_READONLY,
            "Removal overwrote a foreign slot replacement");
    table.replace_for_test(thunk);
    const auto removed_result = remove();
    require(removed_result.status == Status::removed && removed_result.pointer_changed && removed_result.protection_restored &&
                *table.slot == original && protection(table.slot) == PAGE_READONLY,
            "Removal did not restore the original pointer and page protection");
    require(remove().status == Status::not_installed, "Repeated removal changed a removed hook");
    require(install(table.slot, original, &validation_observer).status == Status::installation_consumed,
            "The module allowed retargeting/reinstallation after a successful exchange");
    Invocation after;
    prepare(after, 0x321);
    const auto before_after = observer_calls;
    validation_invoke(&after, table.slot);
    verify(after);
    require(observer_calls == before_after, "Observer ran through the restored slot");
    std::puts(
        "PASS: readonly-slot install/refusal/removal; 128 Win64/SSE/MXCSR register-stack-return cases; original RCX; reentry; 10 unwind "
        "sites and real stack captures.");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
