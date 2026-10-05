#include "../../src/camera/local_memory.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {
using namespace taxi_camera::native_camera;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
struct Faults {
  explicit Faults(LocalMemoryQueryTestFaults value) { set_local_memory_query_test_faults(value); }
  ~Faults() { set_local_memory_query_test_faults({}); }
};
struct Pages {
  std::size_t page;
  std::uint8_t* data;
  Pages() {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    page = info.dwPageSize;
    data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, page * 3, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    require(data != nullptr, "allocation failed");
    std::memset(data, 0x59, page * 3);
    require(VirtualLock(data, page * 3) != FALSE, "could not pin deterministic page fixture");
  }
  ~Pages() {
    VirtualUnlock(data, page * 3);
    VirtualFree(data, 0, MEM_RELEASE);
  }
  std::uint64_t at(std::size_t offset = 0) const { return reinterpret_cast<std::uintptr_t>(data + offset); }
  void protect(std::size_t offset, DWORD value) {
    DWORD before = 0;
    require(VirtualProtect(data + offset, page, value, &before) != FALSE, "could not change fixture protection");
  }
};
void invalid_arguments() {
  Pages fixture;
  LocalMemoryMetrics metrics;
  ScopedLocalMemoryMetrics measure(metrics);
  for (const auto address : {std::uint64_t(0), UINT64_MAX, UINT64_MAX - 7})
    require(!writable_private_span(address, 16), "invalid address accepted");
  require(!writable_private_span(fixture.at(), 0) && !writable_private_span(fixture.at(), kLocalWriteLimit + 1), "unbounded span accepted");
  require(metrics.query_calls == 0 && metrics.read_calls == 0, "invalid input reached OS queries/reads");
  require(writable_private_span(fixture.at(), 1), "one-byte bounded span refused");
  // The 48-byte view-field span (dimensions to flags) and the 64-byte bound,
  // also across a page boundary: still one allocation and two page queries.
  for (const auto size : {std::size_t(17), std::size_t(48), kLocalWriteLimit}) {
    metrics = {};
    require(writable_private_span(fixture.at(fixture.page - 24), size) && metrics.query_fallback_calls == 0 &&
                metrics.query_allocation_calls == 1 && metrics.query_page_calls == 1,
            "a bounded span up to the write limit was refused or scanned its region");
  }
}
void fresh_and_fallback() {
  for (const auto fault : {LocalMemoryQueryTestFaults{}, LocalMemoryQueryTestFaults{true, false, false},
                           LocalMemoryQueryTestFaults{false, true, false}, LocalMemoryQueryTestFaults{false, false, true}}) {
    Pages fixture;
    Faults inject(fault);
    for (const auto offset : {std::size_t(0), fixture.page - 8}) {
      LocalMemoryMetrics metrics;
      ScopedLocalMemoryMetrics measure(metrics);
      require(writable_private_span(fixture.at(offset), 16), "RW span refused");
      require(metrics.read_calls == 0 && metrics.requested_bytes == 0, "metadata validation read flag contents");
      require(metrics.query_calls == metrics.query_allocation_calls + metrics.query_page_calls + metrics.query_fallback_calls,
              "query family accounting mismatch");
      const bool fallback = fault.allocation_unavailable || fault.pages_unavailable || fault.pages_nonresident;
      require((metrics.query_fallback_calls != 0) == fallback, "unexpected fallback selection");
      if (!fallback)
        require(metrics.query_allocation_calls == 1 && metrics.query_page_calls == 1 && metrics.query_calls == 2,
                "hot span did not batch both page queries");
      fixture.protect(fixture.page, PAGE_READONLY);
      require(!writable_private_span(fixture.at(fixture.page - 8), 16), "second readonly flag word accepted");
      fixture.protect(fixture.page, PAGE_READWRITE);
    }
    fixture.protect(0, PAGE_READONLY);
    require(!writable_private_span(fixture.at(), 16), "fresh query reused old writable metadata");
    fixture.protect(0, PAGE_READWRITE);
    LocalMemoryReader reader(16);
    std::uint64_t word = 0;
    ScopedLocalMemoryQueryCache old(LocalMemoryQueryMode::private_pages);
    require(reader.read(fixture.at(), &word, sizeof(word)), "read-only proof setup failed");
    fixture.protect(0, PAGE_READONLY);
    require(!writable_private_span(fixture.at(), 16), "writable guard borrowed an active stale inspection proof");
    require(!old.finish(), "changed inspection metadata accepted");
    fixture.protect(0, PAGE_READWRITE);
  }
}
void separate_allocations() {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const auto granularity = std::size_t(info.dwAllocationGranularity);
  auto* address = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, granularity * 2, MEM_RESERVE, PAGE_NOACCESS));
  require(address != nullptr && VirtualFree(address, 0, MEM_RELEASE), "adjacency reservation failed");
  auto* first = VirtualAlloc(address, granularity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  auto* second = VirtualAlloc(address + granularity, granularity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  require(first == address && second == address + granularity, "adjacent allocation fixture failed");
  std::memset(address + granularity - 8, 0x45, 16);
  for (const auto fault : {LocalMemoryQueryTestFaults{}, LocalMemoryQueryTestFaults{true, false, false},
                           LocalMemoryQueryTestFaults{false, true, false}, LocalMemoryQueryTestFaults{false, false, true}}) {
    Faults inject(fault);
    require(!writable_private_span(reinterpret_cast<std::uintptr_t>(address + granularity - 8), 16),
            "two distinct RW allocations passed single-allocation guard");
  }
  require(VirtualFree(first, 0, MEM_RELEASE) && VirtualFree(second, 0, MEM_RELEASE), "adjacency cleanup failed");
}
void exact_reads() {
  Pages fixture;
  std::array<std::uint64_t, 2> output{1, 2};
  LocalMemoryMetrics metrics;
  ScopedLocalMemoryMetrics measure(metrics);
  require(read_local_flag_words(fixture.at(fixture.page - 8), output), "exact cross-page flags read failed");
  require(output[0] == 0x5959595959595959ull && output[1] == output[0] && metrics.read_calls == 1 && metrics.requested_bytes == 16,
          "exact flags read/accounting changed");
  fixture.protect(fixture.page, PAGE_NOACCESS);
  output = {1, 2};
  require(!read_local_flag_words(fixture.at(fixture.page - 8), output) && output == std::array<std::uint64_t, 2>{1, 2},
          "failed cross-page read exposed partial output");
  require(metrics.read_calls == 2 && metrics.requested_bytes == 32, "failed read did not consume exact metrics");
  fixture.protect(fixture.page, PAGE_READWRITE);
  require(!read_local_flag_words(0, output) && !read_local_flag_words(UINT64_MAX, output) && metrics.read_calls == 2,
          "invalid flags request reached RPM");
}
// write_local_private: its own fresh proof of exactly the span, then one
// timed store; nothing invalid or unproven reaches a query or a store.
void proven_writes() {
  Pages fixture;
  std::array<std::uint8_t, kLocalWriteLimit + 1> data{};
  for (std::size_t i = 0; i < data.size(); ++i)
    data[i] = static_cast<std::uint8_t>(0x80 + i);
  const auto untouched = [&](std::size_t from, std::size_t to) {
    for (auto i = from; i < to; ++i)
      if (fixture.data[i] != 0x59)
        return false;
    return true;
  };
  LocalMemoryMetrics metrics;
  ScopedLocalMemoryMetrics measure(metrics);
  require(!write_local_private(fixture.at(), data.data(), 0) && !write_local_private(fixture.at(), data.data(), kLocalWriteLimit + 1) &&
              !write_local_private(fixture.at(), nullptr, 8) && !write_local_private(0, data.data(), 8) &&
              !write_local_private(UINT64_MAX - 7, data.data(), 16),
          "an invalid write was accepted");
  require(metrics.query_calls == 0 && metrics.write_calls == 0 && untouched(0, fixture.page * 3),
          "an invalid write reached a query or a store");
  for (const auto& [offset, size] : {std::pair{std::size_t(16), std::size_t(24)}, std::pair{fixture.page - 8, std::size_t(24)},
                                     std::pair{fixture.page - 24, kLocalWriteLimit}, std::pair{std::size_t(48), std::size_t(8)}}) {
    metrics = {};
    require(write_local_private(fixture.at(offset), data.data(), size), "a proven write failed");
    require(
        std::memcmp(fixture.data + offset, data.data(), size) == 0 && untouched(0, offset) && untouched(offset + size, fixture.page * 3),
        "a proven write missed or escaped its exact span");
    require(metrics.write_calls == 1 && metrics.read_calls == 0 && metrics.query_fallback_calls == 0 &&
                metrics.query_allocation_calls == 1 && metrics.query_page_calls == 1,
            "a resident write did not take one fresh O(1) proof and one timed store");
    std::memset(fixture.data + offset, 0x59, size);
  }
  // A page that lost exact read-write access since an earlier write or proof:
  // the next write proves afresh and stores nothing.
  for (const DWORD protection :
       {DWORD(PAGE_READONLY), DWORD(PAGE_NOACCESS), DWORD(PAGE_EXECUTE_READWRITE), DWORD(PAGE_READWRITE | PAGE_GUARD)}) {
    auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, fixture.page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    require(page != nullptr, "write refusal allocation failed");
    std::memset(page, 0x59, fixture.page * 2);
    DWORD previous = 0;
    require(VirtualProtect(page + fixture.page, fixture.page, protection, &previous) != FALSE, "write refusal protection failed");
    metrics = {};
    require(!write_local_private(reinterpret_cast<std::uintptr_t>(page + fixture.page - 8), data.data(), 24) && metrics.write_calls == 0,
            "a write into a page without exact read-write access reached a store");
    MEMORY_BASIC_INFORMATION region{};
    require(VirtualQuery(page + fixture.page, &region, sizeof(region)) == sizeof(region) && region.Protect == protection,
            "a refused write consumed a guard page or changed protection");
    require(VirtualProtect(page + fixture.page, fixture.page, PAGE_READWRITE, &previous) != FALSE, "write refusal restore failed");
    for (std::size_t i = 0; i < fixture.page * 2; ++i)
      require(page[i] == 0x59, "a refused write changed memory");
    require(VirtualFree(page, 0, MEM_RELEASE) != FALSE, "write refusal cleanup failed");
  }
  // An inspection scope's proof of the page is never borrowed by a write.
  {
    LocalMemoryReader reader(16);
    std::uint64_t word = 0;
    ScopedLocalMemoryQueryCache old(LocalMemoryQueryMode::private_pages);
    require(reader.read(fixture.at(), &word, sizeof(word)), "read-only proof setup failed");
    fixture.protect(0, PAGE_READONLY);
    metrics = {};
    require(!write_local_private(fixture.at(), data.data(), 8) && metrics.write_calls == 0,
            "a write borrowed an active stale inspection proof");
    require(!old.finish(), "changed inspection metadata accepted");
    fixture.protect(0, PAGE_READWRITE);
  }
  require(untouched(0, fixture.page * 3), "a refused write changed the fixture");
}

// Runs between write_local_private's proof and its store: the page changes
// access after the proof, as another thread could change it.
struct Injection {
  enum class Kind { noaccess, readonly, guard, decommit } kind = Kind::noaccess;
  std::uint8_t* page = nullptr;
  std::size_t page_size = 0;
  unsigned calls = 0;
  bool injected = false;
  std::uint64_t address = 0;
  std::size_t size = 0;
  // Called inside the noexcept writer: reports instead of throwing.
  static void run(void* context, std::uint64_t address, std::size_t size) {
    auto& self = *static_cast<Injection*>(context);
    ++self.calls;
    self.address = address;
    self.size = size;
    DWORD previous = 0;
    switch (self.kind) {
      case Kind::noaccess:
        self.injected = VirtualProtect(self.page, self.page_size, PAGE_NOACCESS, &previous) != FALSE;
        break;
      case Kind::readonly:
        self.injected = VirtualProtect(self.page, self.page_size, PAGE_READONLY, &previous) != FALSE;
        break;
      case Kind::guard:
        self.injected = VirtualProtect(self.page, self.page_size, PAGE_READWRITE | PAGE_GUARD, &previous) != FALSE;
        break;
      case Kind::decommit:
        self.injected = VirtualFree(self.page, self.page_size, MEM_DECOMMIT) != FALSE;
        break;
    }
  }
};
struct WriteHook {
  WriteHook(Injection& injection, bool without_guarded_copy) {
    set_local_memory_write_test_hook(&Injection::run, &injection, without_guarded_copy);
  }
  ~WriteHook() { set_local_memory_write_test_hook(nullptr, nullptr); }
};

// Decommit, no access, read-only or a guard page between the proof and the
// store: false, no crash, nothing stored on the changed page and a consumed
// guard re-armed. Without the fault handler the store is WriteProcessMemory,
// which refuses the same changes (and consumes a guard page, as it always
// did). A page-crossing store may already have written the bytes before the
// changed page, as a partial WriteProcessMemory can.
void write_fault_injection() {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const std::size_t page = info.dwPageSize;
  std::array<std::uint8_t, 24> data{};
  data.fill(0x11);
  using Kind = Injection::Kind;
  for (const bool without_guarded_copy : {false, true}) {
    for (const auto kind : {Kind::noaccess, Kind::readonly, Kind::guard, Kind::decommit}) {
      for (const bool crossing : {false, true}) {
        auto* memory = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        require(memory != nullptr, "fault injection allocation failed");
        std::memset(memory, 0x59, page * 2);
        // The span either lies inside the changed page or crosses into it.
        const auto offset = crossing ? page - 8 : page + 16;
        Injection injection;
        injection.kind = kind;
        injection.page = memory + page;
        injection.page_size = page;
        LocalMemoryMetrics metrics;
        bool written = true;
        {
          WriteHook hook(injection, without_guarded_copy);
          ScopedLocalMemoryMetrics measure(metrics);
          written = write_local_private(reinterpret_cast<std::uintptr_t>(memory + offset), data.data(), data.size());
        }
        require(injection.injected, "the access change could not be injected");
        require(!written, "a store into a page changed after its proof reported success");
        require(
            injection.calls == 1 && injection.address == reinterpret_cast<std::uintptr_t>(memory + offset) && injection.size == data.size(),
            "the fault injection did not run once, between the proof and the store of the exact span");
        require(metrics.write_calls == 1, "the faulting store was not timed as a write");
        MEMORY_BASIC_INFORMATION region{};
        require(VirtualQuery(memory + page, &region, sizeof(region)) == sizeof(region), "fault injection query failed");
        if (kind == Kind::decommit) {
          require(region.State == MEM_RESERVE, "the decommitted page was committed again");
        } else {
          const DWORD injected = kind == Kind::noaccess   ? DWORD(PAGE_NOACCESS)
                                 : kind == Kind::readonly ? DWORD(PAGE_READONLY)
                                 : without_guarded_copy   ? DWORD(PAGE_READWRITE)
                                                          : DWORD(PAGE_READWRITE | PAGE_GUARD);
          const char* changed = kind == Kind::guard ? "the guarded store did not re-arm the consumed guard page"
                                                    : "the failed store changed the page's protection";
          require(region.State == MEM_COMMIT && region.Protect == injected, changed);
          DWORD previous = 0;
          require(VirtualProtect(memory + page, page, PAGE_READWRITE, &previous) != FALSE, "fault injection restore failed");
          for (std::size_t i = page; i < page * 2; ++i)
            require(memory[i] == 0x59, "the failed store changed the page whose access changed");
        }
        // The page before the changed one: untouched, except the bytes a
        // crossing store wrote before it faulted.
        for (std::size_t i = 0; i < (crossing ? page - 8 : page); ++i)
          require(memory[i] == 0x59, "the failed store wrote outside its span");
        require(VirtualFree(memory, 0, MEM_RELEASE) != FALSE, "fault injection cleanup failed");
      }
    }
  }
  // The hook is gone: the delivered path stores again.
  auto* memory = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  require(memory != nullptr && write_local_private(reinterpret_cast<std::uintptr_t>(memory + 8), data.data(), data.size()) &&
              std::memcmp(memory + 8, data.data(), data.size()) == 0,
          "a write after the fault injection failed");
  require(VirtualFree(memory, 0, MEM_RELEASE) != FALSE, "post-injection cleanup failed");
}

// read_local_bytes: one exact timed RPM of 1..kLocalBytesReadLimit bytes,
// output only when whole.
void exact_byte_reads() {
  Pages fixture;
  LocalMemoryMetrics metrics;
  ScopedLocalMemoryMetrics measure(metrics);
  std::array<std::uint8_t, kLocalBytesReadLimit + 1> output{};
  output.fill(1);
  require(read_local_bytes(fixture.at(fixture.page - 12), output.data(), kLocalBytesReadLimit) && output[0] == 0x59 &&
              output[kLocalBytesReadLimit - 1] == 0x59 && output[kLocalBytesReadLimit] == 1 && metrics.read_calls == 1 &&
              metrics.requested_bytes == kLocalBytesReadLimit && metrics.query_calls == 0,
          "an exact cross-page byte read failed or widened");
  output.fill(1);
  require(!read_local_bytes(fixture.at(), output.data(), kLocalBytesReadLimit + 1) && !read_local_bytes(fixture.at(), output.data(), 0) &&
              !read_local_bytes(fixture.at(), nullptr, 8) && !read_local_bytes(0, output.data(), 8) &&
              !read_local_bytes(UINT64_MAX - 3, output.data(), 8) && metrics.read_calls == 1,
          "an invalid byte read reached RPM");
  fixture.protect(fixture.page, PAGE_NOACCESS);
  require(!read_local_bytes(fixture.at(fixture.page - 12), output.data(), 24) && output[0] == 1 && metrics.read_calls == 2,
          "a failed byte read exposed partial output or went uncounted");
  fixture.protect(fixture.page, PAGE_READWRITE);
}
}  // namespace
int main() {
  try {
    invalid_arguments();
    fresh_and_fallback();
    separate_allocations();
    exact_reads();
    proven_writes();
    write_fault_injection();
    exact_byte_reads();
    std::printf("Writable private span tests passed: %u checks\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s (Windows error %lu)\n", error.what(), GetLastError());
    return 1;
  }
}
