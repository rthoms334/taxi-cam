#include "../../src/camera/local_memory.hpp"

#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace {
using namespace taxi_camera::native_camera;

unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s (Windows error %lu)\n", message, GetLastError());
    std::abort();
  }
}

std::size_t page_size() {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  require(info.dwPageSize != 0, "The system page size is unavailable");
  return info.dwPageSize;
}

struct Allocation {
  std::uint8_t* data = nullptr;
  std::size_t size = 0;
  explicit Allocation(std::size_t bytes, bool touch = true) : size(bytes) {
    data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    require(data != nullptr, "Could not allocate a private page fixture");
    if (touch)
      std::fill_n(data, size, 0x39);
  }
  ~Allocation() {
    if (data)
      require(VirtualFree(data, 0, MEM_RELEASE) != FALSE, "Could not release a private page fixture");
  }
  std::uint64_t address(std::size_t offset = 0) const { return reinterpret_cast<std::uintptr_t>(data + offset); }
  void release() {
    require(VirtualFree(data, 0, MEM_RELEASE) != FALSE, "Could not release an observed allocation");
    data = nullptr;
  }
};

struct LockedPages {
  void* address;
  std::size_t size;
  LockedPages(void* value, std::size_t bytes) : address(value), size(bytes) {
    require(VirtualLock(address, size) != FALSE, "Could not pin the small deterministic hot-page fixture");
  }
  ~LockedPages() { require(VirtualUnlock(address, size) != FALSE, "Could not unpin a hot-page fixture"); }
};

PSAPI_WORKING_SET_EX_INFORMATION page_info(void* address) {
  PSAPI_WORKING_SET_EX_INFORMATION info{};
  info.VirtualAddress = address;
  require(K32QueryWorkingSetEx(GetCurrentProcess(), &info, sizeof(info)) != FALSE, "Could not inspect fixture page residency");
  return info;
}

void protect(Allocation& allocation, std::size_t offset, std::size_t size, DWORD protection) {
  DWORD previous = 0;
  require(VirtualProtect(allocation.data + offset, size, protection, &previous) != FALSE, "Could not change fixture page protection");
}

bool filled(const auto& bytes, std::uint8_t value) {
  return std::all_of(bytes.begin(), bytes.end(), [=](auto byte) { return byte == value; });
}

void accounting(const LocalMemoryMetrics& metrics) {
  require(metrics.query_calls == metrics.query_page_calls + metrics.query_allocation_calls + metrics.query_fallback_calls,
          "OS query families do not sum to the total query count");
}

void hot_fields_and_scope_boundaries() {
  const auto page = page_size();
  Allocation allocation(page * 3);
  LockedPages locked(allocation.data, allocation.size);
  LocalMemoryMetrics metrics;
  ScopedLocalMemoryMetrics measured(metrics);
  LocalMemoryReader reader(32);
  std::uint64_t first = 0, second = 0;
  {
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    require(reader.read(allocation.address(17), &first, sizeof(first)) && first == 0x3939393939393939ull,
            "A resident private field was not read exactly");
    const auto queries = metrics.query_calls;
    allocation.data[17] ^= 1;
    require(reader.read(allocation.address(17), &second, sizeof(second)) && second != first,
            "Metadata reuse incorrectly cached field contents or skipped the reread");
    require(metrics.query_calls == queries && metrics.read_calls == 2 && metrics.requested_bytes == 16,
            "A repeated field issued metadata queries or changed exact RPM accounting");
    require(metrics.query_page_calls != 0 && metrics.query_allocation_calls != 0 && metrics.query_fallback_calls == 0,
            "The resident field did not use bounded page and allocation metadata");
    require(cache.finish() && !cache.is_current(), "A hot private-page stage did not finish and detach");
    require(!cache.finish(), "A completed stage supplied another endpoint proof");
  }
  const auto previous_queries = metrics.query_calls;
  {
    ScopedLocalMemoryQueryCache fresh(LocalMemoryQueryMode::private_pages);
    require(reader.read(allocation.address(17), &second, sizeof(second)) && metrics.query_calls > previous_queries && fresh.finish(),
            "A later stage reused metadata from a finished private-page scope");
  }
  require(reader.attempted_bytes() == 24 && reader.read_failures() == 0 && metrics.read_calls == 3 && metrics.requested_bytes == 24,
          "Page metadata changed exact reader budgets or field read counts");
  accounting(metrics);
}

void access_types_and_cold_fallback() {
  const auto page = page_size();
  Allocation allocation(page);
  for (const DWORD access : {DWORD(PAGE_READONLY), DWORD(PAGE_READWRITE), DWORD(PAGE_EXECUTE_READ), DWORD(PAGE_EXECUTE_READWRITE)}) {
    protect(allocation, 0, page, access);
    LocalMemoryReader reader;
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    std::array<std::uint8_t, 16> output{};
    require(reader.read(allocation.address(7), output.data(), output.size()) && filled(output, 0x39) && cache.finish(),
            "A documented readable private-page protection was refused");
  }
  for (const DWORD access : {DWORD(PAGE_NOACCESS), DWORD(PAGE_EXECUTE), DWORD(PAGE_READWRITE | PAGE_GUARD)}) {
    protect(allocation, 0, page, access);
    LocalMemoryReader reader;
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    std::array<std::uint8_t, 16> output;
    output.fill(0xad);
    require(!reader.read(allocation.address(7), output.data(), output.size()) && filled(output, 0xad) &&
                reader.attempted_bytes() == output.size() && reader.read_failures() == 1,
            "An inaccessible, execute-only or guarded page was read or exposed output");
    MEMORY_BASIC_INFORMATION region{};
    require(VirtualQuery(allocation.data, &region, sizeof(region)) == sizeof(region) && region.Protect == access,
            "Page validation consumed a guard page or changed its protection");
  }
  protect(allocation, 0, page, PAGE_READWRITE);

  Allocation cold(page * 2, false);
  require(!page_info(cold.data).VirtualAttributes.Valid, "An untouched demand-zero fixture was unexpectedly resident");
  LocalMemoryMetrics metrics;
  {
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::array<std::uint8_t, 16> output;
    output.fill(0xad);
    require(reader.read(cold.address(), output.data(), output.size()) && filled(output, 0) && cache.finish(),
            "A cold demand-zero page failed its RPM-first page proof");
    // The exact RPM faults the page in; its proof then comes from page metadata,
    // never from VirtualQuery's region scan (about 1 us per MB of suffix).
    require(metrics.query_fallback_calls == 0 && metrics.query_page_calls >= 2 && metrics.read_calls == 1 &&
                metrics.requested_bytes == output.size(),
            "A cold page used the region-scan fallback or changed the exact field read");
  }
  accounting(metrics);
}

// A page trimmed from the working set after the program touched it is the
// MSFS case: the first read of an inspection finds it nonresident.
void trimmed_page_initial_read() {
  const auto page = page_size();
  Allocation allocation(page * 4);
  require(!VirtualUnlock(allocation.data + page, page) && GetLastError() == ERROR_NOT_LOCKED &&
              !page_info(allocation.data + page).VirtualAttributes.Valid,
          "The trimmed-page fixture stayed resident");
  LocalMemoryMetrics metrics;
  {
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::uint64_t value = 0;
    require(reader.read(allocation.address(page + 8), &value, sizeof(value)) && value == 0x3939393939393939ull,
            "A trimmed private page was not read exactly");
    const auto queries = metrics.query_calls;
    require(reader.read(allocation.address(page + 16), &value, sizeof(value)) && metrics.query_calls == queries,
            "A trimmed page's proof was not recorded for later fields");
    protect(allocation, page, page, PAGE_READONLY);
    require(!cache.finish(), "A trimmed page's recorded proof missed an endpoint protection change");
  }
  require(metrics.query_fallback_calls == 0 && metrics.read_calls == 2, "A trimmed page used the region-scan fallback");
  accounting(metrics);
  // A trimmed page that becomes inaccessible is refused by the RPM itself.
  protect(allocation, page, page, PAGE_READWRITE);
  require(!VirtualUnlock(allocation.data + page * 2, page) && GetLastError() == ERROR_NOT_LOCKED, "Second trim fixture failed");
  protect(allocation, page * 2, page, PAGE_NOACCESS);
  {
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::array<std::uint8_t, 8> output;
    output.fill(0xad);
    require(!reader.read(allocation.address(page * 2), output.data(), output.size()) && filled(output, 0xad) && !cache.finish(),
            "An inaccessible nonresident page was read or left the scope usable");
  }
}

// Proven reads copy directly. A page that disappears or changes after its
// proof must fail the read like RPM did, never crash, and never keep a guard
// page consumed. Another thread flips the page while this one reads.
void proven_copy_faults() {
  const auto page = page_size();
  Allocation allocation(page * 2);
  {
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::uint64_t value = 0;
    require(reader.read(allocation.address(page + 8), &value, sizeof(value)), "Proven-copy setup failed");
    require(VirtualFree(allocation.data + page, page, MEM_DECOMMIT) != FALSE, "Could not decommit a proven page");
    value = 0x1234;
    require(!reader.read(allocation.address(page + 8), &value, sizeof(value)) && value == 0x1234 &&
                std::strcmp(cache.failure().stage, "read") == 0 && !cache.finish(),
            "A direct copy from a decommitted page succeeded, exposed bytes or left the scope usable");
  }
  require(VirtualAlloc(allocation.data + page, page, MEM_COMMIT, PAGE_READWRITE) == allocation.data + page,
          "Could not recommit the proven-copy page");
  std::atomic<bool> stop{false};
  std::thread flipper([&] {
    while (!stop.load()) {
      VirtualFree(allocation.data + page, page, MEM_DECOMMIT);
      VirtualAlloc(allocation.data + page, page, MEM_COMMIT, PAGE_READWRITE);
      std::memset(allocation.data + page, 0x39, 64);
    }
  });
  unsigned succeeded = 0, failed = 0;
  for (unsigned round = 0; round < 20000; ++round) {
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::uint64_t first = 0, second = 0;
    const bool ok = reader.read(allocation.address(page + 8), &first, sizeof(first)) &&
                    reader.read(allocation.address(page + 8), &second, sizeof(second)) && cache.finish();
    if (ok) {
      require((first == 0x3939393939393939ull || first == 0) && (second == 0x3939393939393939ull || second == 0),
              "A racing direct copy returned bytes the page never held");
      ++succeeded;
    } else {
      ++failed;
    }
  }
  stop = true;
  flipper.join();
  require(succeeded + failed == 20000, "Racing direct copies were not all accounted for");
}

void rejected_allocation_types() {
  const auto page = page_size();
  const auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(page), nullptr);
  require(mapping != nullptr, "Could not create a mapped-memory fixture");
  for (const DWORD access : {DWORD(FILE_MAP_READ | FILE_MAP_WRITE), DWORD(FILE_MAP_COPY)}) {
    auto* mapped = static_cast<std::uint8_t*>(MapViewOfFile(mapping, access, 0, 0, page));
    require(mapped != nullptr, "Could not map a normal or copy-on-write fixture");
    mapped[0] = 0x39;  // The copy-on-write page is private in its working set, but remains MEM_MAPPED.
    std::array<std::uint8_t, 8> output;
    output.fill(0xad);
    {
      ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
      LocalMemoryReader reader;
      require(!reader.read(reinterpret_cast<std::uintptr_t>(mapped), output.data(), output.size()) && filled(output, 0xad),
              "Working-set privateness incorrectly authorized a mapped or copy-on-write allocation");
    }
    require(UnmapViewOfFile(mapped) != FALSE, "Could not unmap allocation-type fixture");
  }
  require(CloseHandle(mapping) != FALSE, "Could not close the allocation-type mapping");
  {
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::uint16_t output = 0xaaaa;
    require(!reader.read(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)), &output, sizeof(output)) && output == 0xaaaa,
            "The private reader accepted a main-image allocation");
  }
}

void fields_cross_pages_and_failure_output() {
  const auto page = page_size();
  Allocation allocation(page * 10);
  for (std::size_t index = 0; index < allocation.size; ++index)
    allocation.data[index] = static_cast<std::uint8_t>(index * 17 + 3);
  protect(allocation, page, page, PAGE_READONLY);
  LocalMemoryMetrics metrics;
  {
    LockedPages locked(allocation.data, allocation.size);
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::array<std::uint8_t, 32> small;
    small.fill(0xad);
    require(reader.read(allocation.address(page - 8), small.data(), 16) && std::memcmp(small.data(), allocation.data + page - 8, 16) == 0 &&
                std::all_of(small.begin() + 16, small.end(), [](auto byte) { return byte == 0xad; }),
            "A field crossing independently readable pages was copied incorrectly");
    std::array<std::uint8_t, kLocalObjectFieldLimit + 8> large;
    large.fill(0xad);
    require(reader.read(allocation.address(3), large.data(), kLocalObjectFieldLimit) &&
                std::memcmp(large.data(), allocation.data + 3, kLocalObjectFieldLimit) == 0 &&
                std::all_of(large.begin() + kLocalObjectFieldLimit, large.end(), [](auto byte) { return byte == 0xad; }) && cache.finish(),
            "A maximum-sized field escaped exact page coverage or its destination bound");
    require(metrics.read_calls == 2 && metrics.requested_bytes == 16 + kLocalObjectFieldLimit && metrics.query_fallback_calls == 0,
            "Resident page validation split exact RPM fields or used an unbounded fallback");
  }
  accounting(metrics);
  protect(allocation, page, page, PAGE_READWRITE);
  // Validate a successful prefix first, then deny the middle of the next field.
  // Neither prevalidation refusal nor failed RPM may expose a partial prefix.
  for (const DWORD access : {DWORD(PAGE_NOACCESS), DWORD(PAGE_READWRITE | PAGE_GUARD)}) {
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::uint64_t first = 0;
    require(reader.read(allocation.address(page - 8), &first, sizeof(first)), "Cross-page refusal setup failed");
    protect(allocation, page, page, access);
    std::array<std::uint8_t, 8192> output;
    output.fill(0xad);
    require(!reader.read(allocation.address(page - 8), output.data(), output.size()) && filled(output, 0xad),
            "A failed multi-page field exposed a partial destination");
    MEMORY_BASIC_INFORMATION region{};
    require(VirtualQuery(allocation.data + page, &region, sizeof(region)) == sizeof(region) && region.Protect == access,
            "Cross-page validation consumed a middle guard page");
    protect(allocation, page, page, PAGE_READWRITE);
  }
}

void endpoint_page_changes() {
  const auto page = page_size();
  for (const DWORD access : {DWORD(PAGE_READONLY), DWORD(PAGE_NOACCESS), DWORD(PAGE_EXECUTE), DWORD(PAGE_READWRITE | PAGE_GUARD)}) {
    Allocation allocation(page * 3);
    std::uint64_t output = 0;
    LocalMemoryMetrics metrics;
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    require(reader.read(allocation.address(page + 8), &output, sizeof(output)), "Endpoint protection setup failed");
    protect(allocation, page, page, access);
    require(!cache.finish() && metrics.query_cache_validation_failures == 1,
            "Changed protection of an observed page escaped endpoint validation");
    MEMORY_BASIC_INFORMATION region{};
    require(VirtualQuery(allocation.data + page, &region, sizeof(region)) == sizeof(region) && region.Protect == access,
            "Endpoint page validation consumed a guard or changed protection");
    accounting(metrics);
  }
  {
    Allocation allocation(page * 3);
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::uint64_t value = 0;
    require(reader.read(allocation.address(page + 8), &value, sizeof(value)), "Endpoint decommit setup failed");
    require(VirtualFree(allocation.data + page, page, MEM_DECOMMIT) != FALSE && !cache.finish(),
            "Decommit of an observed page escaped endpoint validation");
  }
  {
    Allocation allocation(page);
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::uint64_t value = 0;
    require(reader.read(allocation.address(), &value, sizeof(value)), "Endpoint release setup failed");
    allocation.release();
    require(!cache.finish(), "A released allocation retained its private-page proof");
  }
  {
    Allocation allocation(page * 3);
    LockedPages locked(allocation.data, page);
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::uint64_t first = 0, second = 0;
    require(reader.read(allocation.address(8), &first, sizeof(first)), "Unrelated-page setup failed");
    protect(allocation, page * 2, page, PAGE_READONLY);
    require(reader.read(allocation.address(8), &second, sizeof(second)) && second == first && cache.finish(),
            "An unread page's protection invalidated the deliberately page-bounded observation");
  }
}

void cached_rpm_failures_and_replacement_identity() {
  const auto page = page_size();
  for (const DWORD access : {DWORD(PAGE_NOACCESS), DWORD(PAGE_READWRITE | PAGE_GUARD)}) {
    Allocation allocation(page);
    LocalMemoryMetrics metrics;
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::array<std::uint8_t, 16> output;
    require(reader.read(allocation.address(), output.data(), 8), "Cached RPM failure setup failed");
    protect(allocation, 0, page, access);
    output.fill(0xad);
    require(!reader.read(allocation.address(), output.data(), output.size()) && filled(output, 0xad) && metrics.read_calls == 2 &&
                std::strcmp(cache.failure().stage, "read") == 0,
            "Cached metadata bypassed RPM access checks or exposed bytes from a failed exact read");
    const auto queries = metrics.query_calls;
    require(!reader.read(allocation.address(), output.data(), 8) && metrics.query_calls == queries && metrics.read_calls == 2 &&
                !cache.finish() && metrics.query_cache_validation_failures == 1,
            "A failed exact RPM did not poison its complete inspection scope");
    MEMORY_BASIC_INFORMATION region{};
    require(VirtualQuery(allocation.data, &region, sizeof(region)) == sizeof(region) && region.Protect == access,
            "Cached RPM consumed a guard or changed its protection");
    accounting(metrics);
  }
  const auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(page), nullptr);
  require(mapping != nullptr, "Could not create the allocation-replacement mapping");
  for (const bool mapped_replacement : {true, false}) {
    Allocation allocation(page);
    const auto original = allocation.address();
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::uint64_t value = 0;
    require(reader.read(original, &value, sizeof(value)), "Allocation-replacement setup failed");
    allocation.release();
    void* replacement = mapped_replacement
                            ? MapViewOfFileEx(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, page, reinterpret_cast<void*>(original))
                            : VirtualAlloc(reinterpret_cast<void*>(original), page, MEM_RESERVE | MEM_COMMIT, PAGE_READONLY);
    require(replacement == reinterpret_cast<void*>(original), "Could not replace the exact observed allocation address");
    if (!mapped_replacement) {
      DWORD previous = 0;
      require(VirtualProtect(replacement, page, PAGE_READWRITE, &previous) != FALSE, "Could not make replacement contents readable");
    }
    std::memset(replacement, 0x39, page);
    require(reader.read(original, &value, sizeof(value)) && value == 0x3939393939393939ull && !cache.finish(),
            "Identical bytes and page access concealed changed allocation type or initial allocation protection");
    require(mapped_replacement ? UnmapViewOfFile(replacement) != FALSE : VirtualFree(replacement, 0, MEM_RELEASE) != FALSE,
            "Could not release allocation-replacement fixture");
  }
  require(CloseHandle(mapping) != FALSE, "Could not close allocation-replacement mapping");
}

void nonresident_endpoint_fallback() {
  const auto page = page_size();
  Allocation allocation(page);
  LocalMemoryMetrics metrics;
  ScopedLocalMemoryMetrics measured(metrics);
  ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
  LocalMemoryReader reader;
  std::uint64_t value = 0;
  require(page_info(allocation.data).VirtualAttributes.Valid && reader.read(allocation.address(), &value, sizeof(value)) &&
              metrics.query_fallback_calls == 0,
          "The endpoint-residency fixture was not initially observed through the page backend");
  // An unlocked own-allocation page is removed from this test's working set.
  // https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualunlock
  require(
      !VirtualUnlock(allocation.data, page) && GetLastError() == ERROR_NOT_LOCKED && !page_info(allocation.data).VirtualAttributes.Valid,
      "The targeted fixture did not become nonresident before its endpoint check");
  require(cache.finish() && metrics.query_fallback_calls != 0 && metrics.read_calls == 1 && metrics.requested_bytes == sizeof(value),
          "A harmless residency change failed its endpoint fallback or caused an additional content read");
  accounting(metrics);
}

// A later scope on the same thread queries the pages an earlier one proved in
// the same allocation with its first miss, in one call. Only the addresses
// carry over: each proof is fresh in its own scope, and a batched page that is
// never read is not endpoint-checked.
void batched_page_hints() {
  const auto page = page_size();
  constexpr std::size_t pages = 12;
  Allocation allocation(page * (pages + 2));
  LockedPages locked(allocation.data, allocation.size);
  const auto walk = [&](std::size_t count) {
    LocalMemoryReader reader;
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < count; ++index)
      require(reader.read(allocation.address(index * page + 24), &value, sizeof(value)) && value == 0x3939393939393939ull,
              "A hinted page read returned the wrong bytes");
  };
  {
    ScopedLocalMemoryQueryCache first(LocalMemoryQueryMode::private_pages);
    walk(pages);
    require(first.finish(), "The hint-seeding scope did not finish");
  }
  LocalMemoryMetrics metrics;
  {
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache second(LocalMemoryQueryMode::private_pages);
    walk(pages);
    require(metrics.query_page_calls == 1 && metrics.query_fallback_calls == 0 && metrics.read_calls == pages,
            "A repeated walk did not prove its pages in one batched page query");
    require(second.finish(), "A batched scope refused unchanged pages");
  }
  accounting(metrics);
  {
    ScopedLocalMemoryQueryCache third(LocalMemoryQueryMode::private_pages);
    walk(pages / 2);
    // Batched but never read in this scope: its change must not fail the scope.
    protect(allocation, (pages - 1) * page, page, PAGE_READONLY);
    require(third.finish(), "An unread batched page was endpoint-checked");
  }
  protect(allocation, (pages - 1) * page, page, PAGE_READWRITE);
  {
    ScopedLocalMemoryQueryCache fourth(LocalMemoryQueryMode::private_pages);
    walk(pages);
    // Read after batching: a changed protection must fail the endpoint.
    protect(allocation, 2 * page, page, PAGE_READONLY);
    require(!fourth.finish(), "A batched page that was read escaped endpoint validation");
  }
  protect(allocation, 2 * page, page, PAGE_READWRITE);
}

// A batched page made inaccessible between the batch query and its read: the
// direct copy faults, the read fails and the scope is poisoned.
void batched_page_made_inaccessible() {
  const auto page = page_size();
  Allocation allocation(page * 4);
  LocalMemoryReader reader;
  std::uint64_t value = 0;
  {
    ScopedLocalMemoryQueryCache seed(LocalMemoryQueryMode::private_pages);
    require(reader.read(allocation.address(24), &value, sizeof(value)) &&
                reader.read(allocation.address(3 * page + 24), &value, sizeof(value)) && seed.finish(),
            "Batch seed scope failed");
  }
  ScopedLocalMemoryQueryCache scope(LocalMemoryQueryMode::private_pages);
  require(reader.read(allocation.address(24), &value, sizeof(value)), "Batch seed read failed");
  protect(allocation, 3 * page, page, PAGE_NOACCESS);
  value = 0x1234;
  require(!reader.read(allocation.address(3 * page + 24), &value, sizeof(value)) && value == 0x1234 && !scope.finish(),
          "A batched page made inaccessible before its read was read or accepted");
  protect(allocation, 3 * page, page, PAGE_READWRITE);
}

void exact_budgets_and_invalid_requests() {
  Allocation allocation(page_size());
  LocalMemoryMetrics metrics;
  ScopedLocalMemoryMetrics measured(metrics);
  ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
  LocalMemoryReader reader(16);
  std::array<std::uint8_t, 32> output;
  output.fill(0xad);
  require(!reader.read(0, output.data(), 8) && !reader.read(allocation.address(), nullptr, 8) &&
              !reader.read(allocation.address(), output.data(), 0) &&
              !reader.read(allocation.address(), output.data(), kLocalObjectFieldLimit + 1) &&
              !reader.read(std::numeric_limits<std::uint64_t>::max() - 3, output.data(), 8) && metrics.query_calls == 0 &&
              metrics.read_calls == 0 && reader.attempted_bytes() == 0 && filled(output, 0xad),
          "An invalid request issued queries, reads, consumed allowance or changed output");
  require(reader.read(allocation.address(), output.data(), 16), "Exact private-page allowance was not usable");
  const auto queries = metrics.query_calls;
  require(!reader.read(allocation.address(), output.data(), 1) && reader.attempted_bytes() == 16 && metrics.query_calls == queries &&
              metrics.read_calls == 1 && cache.finish(),
          "Budget exhaustion queried memory or changed the byte allowance");
  accounting(metrics);
}

void capacity_fallback_preserves_prior_proofs() {
  const auto page = page_size();
  for (const bool change_first : {false, true}) {
    Allocation allocation(page * (ScopedLocalMemoryQueryCache::kPageLimit + 1));
    LocalMemoryMetrics metrics;
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::uint64_t value = 0;
    for (std::size_t index = 0; index <= ScopedLocalMemoryQueryCache::kPageLimit; ++index)
      require(reader.read(allocation.address(index * page), &value, sizeof(value)) && value == 0x3939393939393939ull,
              "Page-cap overflow did not continue through bounded legacy metadata");
    require(metrics.query_fallback_calls != 0 && metrics.read_calls == ScopedLocalMemoryQueryCache::kPageLimit + 1,
            "The page cap was silently expanded or changed exact field reads");
    if (change_first)
      protect(allocation, 0, page, PAGE_READONLY);
    require(cache.finish() != change_first, "Page-cap fallback discarded an earlier proof or refused unchanged memory");
    accounting(metrics);
  }
  for (const bool change_first : {false, true}) {
    std::vector<std::unique_ptr<Allocation>> allocations;
    for (std::size_t index = 0; index <= ScopedLocalMemoryQueryCache::kAllocationLimit; ++index)
      allocations.push_back(std::make_unique<Allocation>(page));
    LocalMemoryMetrics metrics;
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::uint64_t value = 0;
    for (const auto& allocation : allocations)
      require(reader.read(allocation->address(), &value, sizeof(value)) && value == 0x3939393939393939ull,
              "Allocation-cap overflow did not retain bounded legacy fallback");
    require(metrics.query_fallback_calls != 0 && metrics.read_calls == allocations.size(),
            "The allocation cap was silently expanded or changed exact field reads");
    if (change_first)
      protect(*allocations.front(), 0, page, PAGE_READONLY);
    require(cache.finish() != change_first, "Allocation-cap fallback discarded a prior proof or refused unchanged memory");
    accounting(metrics);
  }
  {
    // Filling both stores still reaches the original hard legacy-region cap.
    // Neither fallback nor a cache hit may erase the failure and resume reads.
    constexpr auto total = ScopedLocalMemoryQueryCache::kAllocationLimit + ScopedLocalMemoryQueryCache::kRegionLimit;
    std::vector<std::unique_ptr<Allocation>> allocations;
    for (std::size_t index = 0; index <= total; ++index)
      allocations.push_back(std::make_unique<Allocation>(page));
    LocalMemoryMetrics metrics;
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache cache(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader reader;
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < total; ++index)
      require(reader.read(allocations[index]->address(), &value, sizeof(value)), "The combined bounded metadata capacity was unavailable");
    require(!reader.read(allocations.back()->address(), &value, sizeof(value)) &&
                !reader.read(allocations.front()->address(), &value, sizeof(value)) && !cache.finish() && metrics.read_calls == total,
            "Private-page fallback relaxed the legacy region cap or resumed a poisoned scope");
    accounting(metrics);
  }
}

void nested_thread_and_legacy_isolation() {
  Allocation allocation(page_size() * 3);
  LockedPages locked(allocation.data, page_size());
  LocalMemoryMetrics metrics;
  ScopedLocalMemoryMetrics measured(metrics);
  LocalMemoryReader reader;
  std::uint64_t output = 0;
  ScopedLocalMemoryQueryCache outer(LocalMemoryQueryMode::private_pages);
  require(reader.read(allocation.address(), &output, sizeof(output)) && outer.is_current(), "Outer page scope setup failed");
  {
    ScopedLocalMemoryQueryCache inner;
    const auto fallback_before = metrics.query_fallback_calls;
    require(!outer.is_current() && reader.read(allocation.address(), &output, sizeof(output)) &&
                metrics.query_fallback_calls > fallback_before && inner.finish(),
            "A nested default scope borrowed outer page metadata or changed its legacy mode");
  }
  require(outer.is_current(), "A nested scope did not restore its outer page transaction");
  LocalMemoryMetrics thread_metrics;
  bool thread_passed = false;
  std::thread other([&] {
    ScopedLocalMemoryMetrics thread_measured(thread_metrics);
    ScopedLocalMemoryQueryCache independent(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader thread_reader;
    std::uint64_t value = 0;
    thread_passed =
        thread_reader.read(allocation.address(), &value, sizeof(value)) && value == 0x3939393939393939ull && independent.finish();
  });
  other.join();
  require(
      thread_passed && thread_metrics.query_page_calls != 0 && thread_metrics.query_allocation_calls != 0 && thread_metrics.read_calls == 1,
      "Another thread reused the active thread's page observations");
  const auto queries = metrics.query_calls;
  require(reader.read(allocation.address(), &output, sizeof(output)) && metrics.query_calls == queries && outer.finish(),
          "Nested or thread-local operations lost the outer page cache");
  accounting(thread_metrics);
  accounting(metrics);
  {
    ScopedLocalMemoryQueryCache page_scope(LocalMemoryQueryMode::private_pages);
    MEMORY_BASIC_INFORMATION region{};
    require(page_scope.query(allocation.data, region) == sizeof(region) && region.RegionSize >= allocation.size,
            "Direct MBI query was silently clipped to private-page bounds");
    protect(allocation, page_size() * 2, page_size(), PAGE_READONLY);
    require(!page_scope.finish(), "A direct MBI observation lost its full-region endpoint contract");
  }
  {
    LocalMemoryMetrics image_metrics;
    ScopedLocalMemoryMetrics image_measured(image_metrics);
    ScopedLocalMemoryQueryCache page_scope(LocalMemoryQueryMode::private_pages);
    LocalImageReader image(GetModuleHandleW(nullptr), 4096);
    std::array<std::uint8_t, 2> signature{};
    require(image.read(0, signature.data(), signature.size()) && signature[0] == 'M' && signature[1] == 'Z' && page_scope.finish(),
            "Main-image validation stopped working inside a private-page inspection");
    require(image_metrics.query_fallback_calls != 0 && image_metrics.query_page_calls == 0 && image_metrics.query_allocation_calls == 0,
            "An executable-image read entered the private-page backend");
    accounting(image_metrics);
  }
}
}  // namespace

int main() {
  hot_fields_and_scope_boundaries();
  access_types_and_cold_fallback();
  rejected_allocation_types();
  fields_cross_pages_and_failure_output();
  endpoint_page_changes();
  cached_rpm_failures_and_replacement_identity();
  nonresident_endpoint_fallback();
  trimmed_page_initial_read();
  proven_copy_faults();
  batched_page_hints();
  batched_page_made_inaccessible();
  exact_budgets_and_invalid_requests();
  capacity_fallback_preserves_prior_proofs();
  nested_thread_and_legacy_isolation();
  std::printf("PASS: %u private-page metadata checks. Own allocations and self image only; no live performance claim.\n", checks);
}
