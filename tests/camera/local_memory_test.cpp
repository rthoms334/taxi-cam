#include "../../src/camera/local_memory.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace {
using namespace taxi_camera::native_camera;

constexpr std::uint32_t kVerifiedImageTimestamp = 1787653788;
constexpr std::uint32_t kVerifiedImageSize = 235963904;
constexpr std::uint16_t kVerifiedImageSections = 14;

unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s (Windows error %lu)\n", message, GetLastError());
    std::abort();
  }
}

std::uint32_t u32(const std::uint8_t* bytes) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index)
    value |= std::uint32_t(bytes[index]) << (index * 8);
  return value;
}

struct Allocation {
  std::uint8_t* data = nullptr;
  std::size_t size = 0;
  explicit Allocation(std::size_t bytes, DWORD allocation = MEM_RESERVE | MEM_COMMIT) : size(bytes) {
    data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, allocation, PAGE_READWRITE));
    require(data != nullptr, "Could not allocate local memory fixture");
  }
  ~Allocation() { require(VirtualFree(data, 0, MEM_RELEASE) != 0, "Could not release local memory fixture"); }
  std::uint64_t address(std::size_t offset = 0) const { return reinterpret_cast<std::uintptr_t>(data + offset); }
};

void public_query_equivalence() {
  // Both public APIs document identical consecutive-region MBI semantics.
  // https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualqueryex
  const auto compare = [](const void* address) {
    MEMORY_BASIC_INFORMATION implicit{}, explicit_process{};
    const auto first = VirtualQuery(address, &implicit, sizeof(implicit));
    const auto second = VirtualQueryEx(GetCurrentProcess(), address, &explicit_process, sizeof(explicit_process));
    require(first == sizeof(implicit) && second == first, "Public query APIs returned different metadata lengths");
    require(implicit.BaseAddress == explicit_process.BaseAddress && implicit.AllocationBase == explicit_process.AllocationBase &&
                implicit.AllocationProtect == explicit_process.AllocationProtect && implicit.RegionSize == explicit_process.RegionSize &&
                implicit.State == explicit_process.State && implicit.Protect == explicit_process.Protect &&
                implicit.Type == explicit_process.Type,
            "Explicit current-process query changed allocation, extent, state, protection or type");
    return explicit_process;
  };
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const auto page = std::size_t(info.dwPageSize);
  Allocation allocation(page * 16);
  for (const DWORD protection : {DWORD(PAGE_READONLY), DWORD(PAGE_READWRITE), DWORD(PAGE_NOACCESS), DWORD(PAGE_EXECUTE),
                                 DWORD(PAGE_EXECUTE_READ), DWORD(PAGE_EXECUTE_READWRITE), DWORD(PAGE_READWRITE | PAGE_GUARD)}) {
    DWORD previous = 0;
    require(VirtualProtect(allocation.data, allocation.size, protection, &previous) != FALSE,
            "Could not set equivalence fixture protection");
    for (unsigned offset = 0; offset < 16; ++offset) {
      const auto region = compare(allocation.data + offset * page + 13);
      require(region.State == MEM_COMMIT && region.Type == MEM_PRIVATE && region.Protect == protection,
              "Public query altered requested protection or consumed a guard page");
    }
  }
  DWORD previous = 0;
  require(VirtualProtect(allocation.data, allocation.size, PAGE_READWRITE, &previous) != FALSE, "Could not restore equivalence fixture");
  require(VirtualProtect(allocation.data + 3 * page, page, PAGE_READONLY, &previous) != FALSE, "Could not create query split fixture");
  for (unsigned offset = 0; offset < 16; ++offset)
    compare(allocation.data + offset * page);
  require(VirtualFree(allocation.data + 8 * page, page, MEM_DECOMMIT) != FALSE, "Could not decommit equivalence fixture");
  require(compare(allocation.data + 8 * page).State == MEM_RESERVE, "Explicit query accepted decommitted page as committed");
  compare(allocation.data + 7 * page);
  compare(allocation.data + 9 * page);
  require(VirtualAlloc(allocation.data + 8 * page, page, MEM_COMMIT, PAGE_READWRITE) == allocation.data + 8 * page,
          "Could not recommit equivalence fixture");
  require(compare(allocation.data + 8 * page).State == MEM_COMMIT, "Explicit query did not observe recommit");
  {
    Allocation reservation(page * 3, MEM_RESERVE);
    require(compare(reservation.data + page).State == MEM_RESERVE, "Explicit query changed reserved region identity");
  }
  const auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(page), nullptr);
  require(mapping != nullptr, "Could not create public query mapping fixture");
  const auto mapped = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, page);
  require(mapped && compare(mapped).Type == MEM_MAPPED, "Explicit query lost mapped-file type");
  require(UnmapViewOfFile(mapped) != FALSE && CloseHandle(mapping) != FALSE, "Could not release public query mapping fixture");
  require(compare(GetModuleHandleW(nullptr)).Type == MEM_IMAGE, "Explicit query lost main-image type");
  auto* released = VirtualAlloc(nullptr, page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  require(released != nullptr && VirtualFree(released, 0, MEM_RELEASE) != FALSE, "Could not create released query fixture");
  require(compare(released).State == MEM_FREE, "Explicit query lost released-region state");
  MEMORY_BASIC_INFORMATION invalid{};
  const auto* inaccessible = reinterpret_cast<const void*>(std::numeric_limits<std::uintptr_t>::max());
  require(VirtualQuery(inaccessible, &invalid, sizeof(invalid)) == 0 &&
              VirtualQueryEx(GetCurrentProcess(), inaccessible, &invalid, sizeof(invalid)) == 0,
          "Explicit query accepted an out-of-range address");

  // Compare identical wrapper work on this process only. This does not model
  // MSFS's address map, thread scheduling or any installed instrumentation.
  Allocation benchmark(page * 16);
  LARGE_INTEGER frequency{}, start{}, middle{}, end{};
  constexpr unsigned repeats = 512;
  constexpr unsigned queries_per_stage = 28;
  std::uint64_t implicit_bytes = 0, explicit_bytes = 0;
  require(QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&start), "Could not start public wrapper benchmark");
  for (unsigned run = 0; run < repeats; ++run)
    for (unsigned pass = 0; pass < 2; ++pass)
      for (unsigned offset = 14; offset > 0; --offset) {
        MEMORY_BASIC_INFORMATION region{};
        require(VirtualQuery(benchmark.data + offset * page, &region, sizeof(region)) == sizeof(region), "Implicit query benchmark failed");
        implicit_bytes += region.RegionSize;
      }
  require(QueryPerformanceCounter(&middle), "Could not time implicit queries");
  for (unsigned run = 0; run < repeats; ++run)
    for (unsigned pass = 0; pass < 2; ++pass)
      for (unsigned offset = 14; offset > 0; --offset) {
        MEMORY_BASIC_INFORMATION region{};
        require(VirtualQueryEx(GetCurrentProcess(), benchmark.data + offset * page, &region, sizeof(region)) == sizeof(region),
                "Explicit current-process query benchmark failed");
        explicit_bytes += region.RegionSize;
      }
  require(QueryPerformanceCounter(&end) && implicit_bytes == explicit_bytes, "Public wrappers did not query equivalent extents");
  std::printf(
      "Own-process public query wrappers: queries_each=%u VirtualQuery_ms=%.6f VirtualQueryEx_ms=%.6f per28; live benefit unmeasured.\n",
      repeats * queries_per_stage, double(middle.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart / repeats,
      double(end.QuadPart - middle.QuadPart) * 1000 / frequency.QuadPart / repeats);
}
void measured_reads() {
  Allocation allocation(8192);
  LocalMemoryReader reader;
  LocalMemoryMetrics outer, inner;
  std::array<std::uint8_t, 64> output{};
  require(reader.read(allocation.address(), output.data(), 8), "unmeasured read failed");
  {
    ScopedLocalMemoryMetrics scope(outer);
    require(reader.read(allocation.address(), output.data(), 8), "measured read failed");
    require(outer.query_calls == 1 && outer.read_calls == 1 && outer.requested_bytes == 8, "read metrics count differs from real calls");
    {
      ScopedLocalMemoryMetrics nested(inner);
      require(reader.read(allocation.address(1), output.data(), 16), "nested measured read failed");
      require(inner.query_calls == 1 && inner.read_calls == 1 && inner.requested_bytes == 16, "nested metrics were not isolated");
    }
    require(reader.read(allocation.address(), output.data(), 8), "restored measured scope failed");
    require(outer.query_calls == 2 && outer.read_calls == 2 && outer.requested_bytes == 16, "outer metrics scope was not restored");
    require(!reader.read(0, output.data(), 8), "invalid pointer accepted");
    require(outer.query_calls == 2 && outer.read_calls == 2, "invalid read issued or counted an OS call");
    DWORD previous = 0;
    require(VirtualProtect(allocation.data + 4096, 4096, PAGE_NOACCESS, &previous) != 0, "metrics protection failed");
    require(!reader.read(allocation.address(4096), output.data(), 8), "inaccessible measured read accepted");
    require(outer.query_calls == 3 && outer.read_calls == 2 && outer.requested_bytes == 16, "failed query was counted as an RPM");
    require(VirtualProtect(allocation.data + 4096, 4096, previous, &previous) != 0, "metrics protection restore failed");
    LocalImageReader image(GetModuleHandleW(nullptr), 4096);
    require(image.read(0, output.data(), 2), "measured self-image read failed");
    require(outer.query_calls == 4 && outer.read_calls == 3 && outer.requested_bytes == 18, "image read was omitted from metrics");
  }
  require(reader.read(allocation.address(), output.data(), 8), "post-scope read failed");
  require(outer.query_calls == 4 && outer.read_calls == 3 && inner.query_calls == 1, "expired metrics scope was retained");
  require(outer.query_ticks > 0 && outer.read_ticks > 0, "successful timed OS calls had no elapsed ticks");
}

void cached_queries() {
  Allocation allocation(16384);
  for (std::size_t i = 0; i < allocation.size; ++i)
    allocation.data[i] = static_cast<std::uint8_t>(i ^ 0x39);
  LocalMemoryReader reader;
  std::array<std::uint8_t, 16> output{};
  LocalMemoryMetrics metrics;
  {
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache cache;
    // 98 initial fields and their complete second reads are all retained.
    for (unsigned pass = 0; pass < 2; ++pass)
      for (unsigned i = 0; i < 98; ++i) {
        require(reader.read(allocation.address(i * 16), output.data(), output.size()), "Cached field read failed");
        require(std::memcmp(output.data(), allocation.data + i * 16, output.size()) == 0, "Cache altered field contents");
      }
    require(metrics.query_calls == 1 && metrics.query_cache_hits == 195 && metrics.read_calls == 196 && metrics.requested_bytes == 3136 &&
                reader.attempted_bytes() == 3136,
            "Cache omitted an RPM/trace read or failed to reuse region metadata");
    require(cache.finish() && metrics.query_calls == 2 && metrics.query_cache_validation_failures == 0,
            "Cached region was not freshly revalidated at the inspection endpoint");
    require(!cache.finish(), "A finished scope was reused as a fresh proof");
    require(reader.read(allocation.address(), output.data(), 8) && metrics.query_calls == 3, "Finished scope retained its cached metadata");
  }
  require(metrics.query_calls == 3 && metrics.read_calls == 197, "Cache metrics lost exact native call counts");

  // Independent readers and main-image reads share only this stage's metadata.
  metrics = {};
  {
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache cache;
    LocalImageReader image(GetModuleHandleW(nullptr), 4096);
    LocalMemoryReader second;
    require(reader.read(allocation.address(), output.data(), 8) && second.read(allocation.address(8), output.data(), 8),
            "Independent object readers did not share one scoped region cache");
    require(image.query(0, 2).readable && image.read(0, output.data(), 2) && output[0] == 'M' && output[1] == 'Z',
            "Cached main-image query/read failed its exact allocation contract");
    require(!second.read(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)), output.data(), 8),
            "A cached image region bypassed the private-object type restriction");
    require(cache.finish() && metrics.query_calls == 4 && metrics.query_cache_hits == 3 && metrics.read_calls == 3,
            "Two distinct object/image regions were not independently checked twice");
  }

  // Nested scopes own separate proofs, and another thread has no active cache.
  metrics = {};
  {
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache outer;
    require(reader.read(allocation.address(), output.data(), 8), "Outer cache read failed");
    {
      ScopedLocalMemoryQueryCache inner;
      require(reader.read(allocation.address(), output.data(), 8) && inner.finish(), "Inner cache did not validate independently");
    }
    LocalMemoryMetrics thread_metrics;
    std::thread other([&] {
      ScopedLocalMemoryMetrics measured_other(thread_metrics);
      LocalMemoryReader independent;
      std::uint64_t value = 0;
      independent.read(allocation.address(), &value, sizeof(value));
      independent.read(allocation.address(), &value, sizeof(value));
    });
    other.join();
    require(thread_metrics.query_calls == 2 && thread_metrics.read_calls == 2 && thread_metrics.query_cache_hits == 0,
            "A different thread reused cached region metadata");
    require(
        reader.read(allocation.address(), output.data(), 8) && outer.finish() && metrics.query_calls == 4 && metrics.query_cache_hits == 1,
        "Nested scope did not restore its outer stage");
  }
  metrics = {};
  {
    ScopedLocalMemoryMetrics measured(metrics);
    {
      ScopedLocalMemoryQueryCache abandoned;
      require(reader.read(allocation.address(), output.data(), 8), "Abandoned cache read failed");
    }
    require(reader.read(allocation.address(), output.data(), 8) && metrics.query_calls == 2 && metrics.query_cache_hits == 0,
            "Destruction leaked an unvalidated cache into later operations");
  }
}

void fused_inspection_transactions() {
  Allocation allocation(16384);
  std::fill(allocation.data, allocation.data + allocation.size, 0x57);
  // Two pure graph stages retain independent readers and complete trace rereads.
  // They may share metadata only with the currently active outer transaction.
  const auto stage = [&](ScopedLocalMemoryQueryCache& transaction, std::size_t offset, bool change_field = false) {
    if (!transaction.is_current())
      return false;
    LocalMemoryReader reader;
    std::uint64_t first = 0, second = 0;
    if (!reader.read(allocation.address(offset), &first, sizeof(first)))
      return false;
    if (change_field)
      allocation.data[offset] ^= 1u;
    return reader.read(allocation.address(offset), &second, sizeof(second)) && first == second;
  };
  LocalMemoryMetrics separate, fused;
  {
    ScopedLocalMemoryMetrics measured(separate);
    for (const auto offset : {0u, 128u}) {
      ScopedLocalMemoryQueryCache transaction;
      require(stage(transaction, offset) && transaction.finish(), "Separate inspection failed");
    }
  }
  {
    ScopedLocalMemoryMetrics measured(fused);
    ScopedLocalMemoryQueryCache transaction;
    require(stage(transaction, 0) && stage(transaction, 128), "Fused stages did not share their active transaction");
    require(transaction.finish(), "Fused transaction was not revalidated before publication");
    require(!transaction.is_current() && !stage(transaction, 0), "A completed proof was borrowed across an operation boundary");
  }
  require(separate.query_calls == 4 && fused.query_calls == 2 && separate.read_calls == 4 && fused.read_calls == 4 &&
              separate.requested_bytes == 32 && fused.requested_bytes == 32,
          "Fusing adjacent stages failed to remove repeated queries or changed exact field/trace reads");
  {
    ScopedLocalMemoryQueryCache transaction;
    require(!stage(transaction, 0, true), "Shared metadata hid an identity change from the complete trace reread");
    require(transaction.finish(), "A byte-only fixture change incorrectly changed memory metadata");
  }
  {
    ScopedLocalMemoryQueryCache transaction;
    require(stage(transaction, 0), "Protection-change first stage failed");
    DWORD previous = 0;
    require(VirtualProtect(allocation.data, allocation.size, PAGE_READONLY, &previous) != FALSE, "Could not change stage protection");
    require(stage(transaction, 128), "Readable second stage unexpectedly failed");
    require(!transaction.finish(), "Fused stages accepted protection changes before their single endpoint check");
    require(VirtualProtect(allocation.data, allocation.size, previous, &previous) != FALSE, "Could not restore stage protection");
  }
  {
    ScopedLocalMemoryQueryCache outer;
    require(stage(outer, 0), "Outer transaction setup failed");
    {
      ScopedLocalMemoryQueryCache inner;
      require(!outer.is_current() && !stage(outer, 128), "A pure stage borrowed a hidden outer transaction");
      require(stage(inner, 128) && inner.finish(), "Independent nested transaction failed");
    }
    require(outer.is_current() && outer.finish(), "Nested scope lost the outer transaction");
  }
  {
    LocalMemoryMetrics after_operation;
    ScopedLocalMemoryMetrics measured(after_operation);
    // A private-operation boundary requires a new proof, even for the same field.
    ScopedLocalMemoryQueryCache transaction;
    require(stage(transaction, 0) && transaction.finish() && after_operation.query_calls == 2,
            "Post-operation inspection reused prior region metadata");
  }
}

void cache_protection_changes() {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const auto page = std::size_t(info.dwPageSize);
  Allocation allocation(page * 3);
  std::fill(allocation.data, allocation.data + allocation.size, 0x37);
  std::array<std::uint8_t, 16> output{};
  LocalMemoryReader reader;
  DWORD previous = 0;
  // A split elsewhere in the initially observed region must invalidate the
  // proof even when every actual field read still succeeds with identical data.
  for (const auto changed_page : {0u, 1u, 2u}) {
    ScopedLocalMemoryQueryCache cache;
    require(reader.read(allocation.address(), output.data(), 8), "Split fixture initial read failed");
    require(VirtualProtect(allocation.data + page * changed_page, page, PAGE_READONLY, &previous) != FALSE,
            "Could not introduce a readable region split");
    require(reader.read(allocation.address(page * 2), output.data(), 8), "A readable cached field was spuriously rejected");
    require(!cache.finish(), "Changed protection/extent escaped endpoint validation");
    require(VirtualProtect(allocation.data + page * changed_page, page, PAGE_READWRITE, &previous) != FALSE,
            "Could not restore split fixture");
  }
  for (const auto access : {PAGE_NOACCESS, PAGE_EXECUTE, PAGE_READWRITE | PAGE_GUARD}) {
    LocalMemoryMetrics metrics;
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache cache;
    require(reader.read(allocation.address(page), output.data(), 8), "Access-change initial read failed");
    require(VirtualProtect(allocation.data + page, page, access, &previous) != FALSE, "Could not change cached page access");
    output.fill(0xad);
    const bool copied = reader.read(allocation.address(page), output.data(), output.size());
    // Windows can let RPM read PAGE_EXECUTE on this architecture. Endpoint
    // verification must still refuse that stage, regardless of RPM success.
    require((copied && access == PAGE_EXECUTE) ||
                (!copied && std::all_of(output.begin(), output.end(), [](auto byte) { return byte == 0xad; })),
            "Cached metadata bypassed RPM access checks or exposed partial bytes");
    require(!cache.finish() && metrics.query_cache_validation_failures == 1 && metrics.read_calls == 2,
            "Failed RPM did not poison the stage or preserve call accounting");
    MEMORY_BASIC_INFORMATION current{};
    require(VirtualQuery(allocation.data + page, &current, sizeof(current)) == sizeof(current) && current.Protect == DWORD(access),
            "Cached RPM consumed a guard page or changed its protection");
    require(VirtualProtect(allocation.data + page, page, PAGE_READWRITE, &previous) != FALSE, "Could not restore access-change fixture");
  }
  {
    ScopedLocalMemoryQueryCache cache;
    require(reader.read(allocation.address(), output.data(), 8), "Decommit initial read failed");
    require(VirtualFree(allocation.data + page, page, MEM_DECOMMIT) != FALSE, "Could not decommit cached middle page");
    require(!cache.finish(), "Decommitted cached subregion passed endpoint validation");
    require(VirtualAlloc(allocation.data + page, page, MEM_COMMIT, PAGE_READWRITE) == allocation.data + page,
            "Could not recommit fixture page");
  }
  {
    ScopedLocalMemoryQueryCache cache;
    require(reader.read(allocation.address(), output.data(), 8), "Bulk cache setup failed");
    require(VirtualProtect(allocation.data + page, page, PAGE_NOACCESS, &previous) != FALSE, "Could not deny cached bulk middle page");
    std::array<std::uint8_t, 8192> bulk;
    bulk.fill(0xda);
    require(!reader.read(allocation.address(), bulk.data(), bulk.size()) &&
                std::all_of(bulk.begin(), bulk.end(), [](auto byte) { return byte == 0xda; }) && !cache.finish(),
            "Cached bulk read leaked an earlier region after a middle-page failure");
    require(VirtualProtect(allocation.data + page, page, PAGE_READWRITE, &previous) != FALSE, "Could not restore bulk middle page");
  }
  // Release is checked without touching the old allocation through a raw load.
  auto* released = VirtualAlloc(nullptr, page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  require(released != nullptr, "Could not allocate release fixture");
  {
    ScopedLocalMemoryQueryCache cache;
    require(reader.read(reinterpret_cast<std::uintptr_t>(released), output.data(), 8), "Release initial read failed");
    require(VirtualFree(released, 0, MEM_RELEASE) != FALSE && !cache.finish(), "Released allocation retained valid cached identity");
  }
  // An accessible replacement at the exact address still fails its original
  // private-allocation proof when the memory type changes to a mapped view.
  auto* replaced = VirtualAlloc(nullptr, page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  require(replaced != nullptr, "Could not allocate replacement fixture");
  const auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(page), nullptr);
  require(mapping != nullptr, "Could not create replacement mapping");
  {
    ScopedLocalMemoryQueryCache cache;
    require(reader.read(reinterpret_cast<std::uintptr_t>(replaced), output.data(), 8), "Replacement initial read failed");
    require(VirtualFree(replaced, 0, MEM_RELEASE) != FALSE, "Could not release replacement address");
    const auto mapped = MapViewOfFileEx(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, page, replaced);
    require(mapped == replaced, "Could not place mapping at the exact former private address");
    require(reader.read(reinterpret_cast<std::uintptr_t>(mapped), output.data(), 8) && !cache.finish(),
            "Readable replacement with a different allocation type passed validation");
    require(UnmapViewOfFile(mapped) != FALSE, "Could not release replacement mapping");
  }
  require(CloseHandle(mapping) != FALSE, "Could not close replacement mapping");
}

void cache_bounds() {
  std::vector<std::unique_ptr<Allocation>> allocations;
  for (unsigned i = 0; i <= ScopedLocalMemoryQueryCache::kRegionLimit; ++i)
    allocations.push_back(std::make_unique<Allocation>(4096));
  LocalMemoryReader reader;
  std::uint64_t output = 0;
  LocalMemoryMetrics metrics;
  {
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache cache;
    for (unsigned i = 0; i < ScopedLocalMemoryQueryCache::kRegionLimit; ++i)
      require(reader.read(allocations[i]->address(), &output, sizeof(output)), "Bounded cache refused an allowed distinct region");
    require(!reader.read(allocations.back()->address(), &output, sizeof(output)) &&
                metrics.query_calls == ScopedLocalMemoryQueryCache::kRegionLimit &&
                metrics.read_calls == ScopedLocalMemoryQueryCache::kRegionLimit,
            "Cache limit silently evicted metadata or issued an unbounded extra query");
    require(!reader.read(allocations.front()->address(), &output, sizeof(output)) && !cache.finish() &&
                metrics.query_cache_validation_failures == 1,
            "Cache exhaustion did not latch refusal for the whole stage");
  }
  {
    ScopedLocalMemoryQueryCache fresh;
    require(reader.read(allocations.back()->address(), &output, sizeof(output)) && fresh.finish(),
            "A fresh bounded scope inherited a prior scope's failure");
  }
}

void cache_profile() {
  Allocation allocation(4096);
  std::array<std::uint64_t, 98> uncached{}, cached{};
  LocalMemoryReader reader;
  LocalMemoryMetrics before, after;
  LARGE_INTEGER frequency{}, started{}, middle{}, finished{};
  require(QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&started), "Could not start cache profile");
  constexpr unsigned repeats = 128;
  {
    ScopedLocalMemoryMetrics metrics(before);
    for (unsigned run = 0; run < repeats; ++run) {
      reader.reset_budget();
      for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned i = 0; i < uncached.size(); ++i)
          require(reader.read(allocation.address(i * 8), &uncached[i], 8), "Uncached profile read failed");
    }
  }
  require(QueryPerformanceCounter(&middle), "Could not time uncached profile");
  {
    ScopedLocalMemoryMetrics metrics(after);
    for (unsigned run = 0; run < repeats; ++run) {
      ScopedLocalMemoryQueryCache cache;
      reader.reset_budget();
      for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned i = 0; i < cached.size(); ++i)
          require(reader.read(allocation.address(i * 8), &cached[i], 8), "Cached profile read failed");
      require(cache.finish(), "Cached profile endpoint did not agree");
    }
  }
  require(QueryPerformanceCounter(&finished) && cached == uncached && before.query_calls == 196 * repeats &&
              after.query_calls == 2 * repeats && before.read_calls == after.read_calls &&
              before.requested_bytes == after.requested_bytes && after.query_cache_hits == 195 * repeats,
          "Cache profile changed bytes/field rechecks or failed its bounded query reduction");
  const auto milliseconds = [&](LONGLONG ticks) { return double(ticks) * 1000.0 / double(frequency.QuadPart) / repeats; };
  std::printf("Own-memory stage: 196 exact reads unchanged, VirtualQuery 196 -> 2, %.3f -> %.3f ms incl endpoint check.\n",
              milliseconds(middle.QuadPart - started.QuadPart), milliseconds(finished.QuadPart - middle.QuadPart));
}

void descending_cache_profile() {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const auto page = std::size_t(info.dwPageSize);
  Allocation allocation(page * 16);
  std::fill(allocation.data, allocation.data + allocation.size, 0x39);
  LocalMemoryReader reader;
  LocalMemoryMetrics metrics;
  LARGE_INTEGER frequency{}, start{}, finish{};
  require(QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&start), "Descending benchmark clock unavailable");
  constexpr unsigned repeats = 128;
  {
    ScopedLocalMemoryMetrics measured(metrics);
    for (unsigned repeat = 0; repeat < repeats; ++repeat) {
      reader.reset_budget();
      ScopedLocalMemoryQueryCache cache;
      // Graph traversal may discover lower-address objects after higher ones.
      // Each requested field and its exact consistency reread remain present.
      for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned offset = 14; offset > 0; --offset) {
          std::uint64_t value = 0;
          require(reader.read(allocation.address(offset * page), &value, sizeof(value)) && value == 0x3939393939393939ull,
                  "Descending cached field or complete reread changed");
        }
      require(cache.finish(), "Descending cache endpoint proof failed");
    }
  }
  require(QueryPerformanceCounter(&finish), "Descending benchmark clock unavailable at endpoint");
  require(metrics.query_calls == 2 * repeats && metrics.read_calls == 28 * repeats && metrics.requested_bytes == 224 * repeats,
          "Canonical query failed to reduce descending queries or omitted exact fields");
  std::printf("Descending own-allocation graph: queries_per_stage=%llu exact_reads=28 bytes=224 stage_ms=%.6f query_ms=%.6f\n",
              static_cast<unsigned long long>(metrics.query_calls / repeats),
              double(finish.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart / repeats,
              double(metrics.query_ticks) * 1000 / frequency.QuadPart / repeats);
}
void canonical_query_ranges() {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const auto page = std::size_t(info.dwPageSize);
  Allocation allocation(65536);
  require(page == 4096 && allocation.address() % 65536 == 0, "Canonical query fixture needs this host's aligned 64 KiB allocation");
  std::fill(allocation.data, allocation.data + allocation.size, 0x39);
  LocalMemoryReader reader;
  for (const DWORD protection : {DWORD(PAGE_NOACCESS), DWORD(PAGE_READWRITE | PAGE_GUARD), DWORD(PAGE_READONLY)}) {
    DWORD previous = 0;
    require(VirtualProtect(allocation.data, page, protection, &previous) != FALSE, "Could not protect preceding query page");
    LocalMemoryMetrics metrics;
    {
      ScopedLocalMemoryMetrics measured(metrics);
      ScopedLocalMemoryQueryCache cache;
      std::uint64_t output = 0;
      require(reader.read(allocation.address(3 * page), &output, sizeof(output)) && output == 0x3939393939393939ull,
              "Earlier metadata query changed the actual field read");
      require(cache.finish() && metrics.query_calls == 3 && metrics.read_calls == 1 && metrics.requested_bytes == 8,
              "Preceding split did not use exact-address fallback and exact endpoint proof");
    }
    MEMORY_BASIC_INFORMATION prefix{};
    require(VirtualQuery(allocation.data, &prefix, sizeof(prefix)) == sizeof(prefix) && prefix.Protect == protection,
            "Earlier metadata query consumed a guard or changed preceding protection");
    require(VirtualProtect(allocation.data, page, PAGE_READWRITE, &previous) != FALSE, "Could not restore preceding query page");
  }
  require(VirtualFree(allocation.data, page, MEM_DECOMMIT) != FALSE, "Could not decommit preceding query page");
  {
    LocalMemoryMetrics metrics;
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache cache;
    std::uint64_t output = 0;
    require(reader.read(allocation.address(3 * page), &output, sizeof(output)) && cache.finish() && metrics.query_calls == 3 &&
                metrics.read_calls == 1 && output == 0x3939393939393939ull,
            "Reserved preceding page bypassed exact-address fallback");
  }
  require(VirtualAlloc(allocation.data, page, MEM_COMMIT, PAGE_READWRITE) == allocation.data, "Could not recommit preceding page");
  std::fill(allocation.data, allocation.data + page, 0x39);
  // Changes outside the actual field still invalidate the broader observation.
  for (const auto changed : {0u, 3u, 15u}) {
    ScopedLocalMemoryQueryCache cache;
    std::uint64_t output = 0;
    DWORD previous = 0;
    require(reader.read(allocation.address(8 * page), &output, sizeof(output)), "Canonical extent initial read failed");
    require(VirtualProtect(allocation.data + changed * page, page, PAGE_READONLY, &previous) != FALSE, "Could not split canonical extent");
    require(reader.read(allocation.address(8 * page), &output, sizeof(output)) && !cache.finish(),
            "Changed preceding/succeeding page escaped canonical endpoint proof");
    require(VirtualProtect(allocation.data + changed * page, page, PAGE_READWRITE, &previous) != FALSE,
            "Could not restore canonical extent");
  }
  // The fixed region cap still bounds both fallback queries and every endpoint.
  std::vector<std::unique_ptr<Allocation>> allocations;
  for (unsigned i = 0; i <= ScopedLocalMemoryQueryCache::kRegionLimit; ++i) {
    auto item = std::make_unique<Allocation>(65536);
    DWORD previous = 0;
    require(VirtualProtect(item->data, page, PAGE_NOACCESS, &previous) != FALSE, "Could not create bounded fallback fixture");
    allocations.push_back(std::move(item));
  }
  {
    LocalMemoryMetrics metrics;
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache cache;
    std::uint64_t output = 0;
    for (unsigned i = 0; i < ScopedLocalMemoryQueryCache::kRegionLimit; ++i)
      require(reader.read(allocations[i]->address(page), &output, sizeof(output)), "Allowed bounded fallback read failed");
    require(metrics.query_calls == 128 && cache.finish() && metrics.query_calls == 192 && metrics.read_calls == 64,
            "Fallback query plus endpoint exceeded or omitted fixed 192-call maximum");
  }
  {
    LocalMemoryMetrics metrics;
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache cache;
    std::uint64_t output = 0;
    for (unsigned i = 0; i < ScopedLocalMemoryQueryCache::kRegionLimit; ++i)
      require(reader.read(allocations[i]->address(page), &output, sizeof(output)), "Bounded fallback cap setup failed");
    require(!reader.read(allocations.back()->address(page), &output, sizeof(output)) && metrics.query_calls == 128 && !cache.finish(),
            "Region cap issued an extra canonical or fallback query");
  }
}
void private_memory_reads() {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const auto page = std::size_t(info.dwPageSize);
  Allocation allocation(page * 3);
  for (std::size_t index = 0; index < allocation.size; ++index)
    allocation.data[index] = static_cast<std::uint8_t>(index);
  LocalMemoryReader reader;
  std::array<std::uint8_t, 64> output{};
  std::uint32_t expected_bytes = 0;
  for (std::size_t size = 1; size <= 64; ++size) {
    output.fill(0xee);
    require(reader.read(allocation.address(1), output.data(), size), "A permitted local private field could not be read");
    expected_bytes += static_cast<std::uint32_t>(size);
    require(reader.attempted_bytes() == expected_bytes && reader.read_failures() == 0, "Successful local read accounting is incorrect");
    for (std::size_t index = 0; index < output.size(); ++index)
      require(output[index] == (index < size ? std::uint8_t(index + 1) : 0xee), "Local reader copied bytes outside the requested field");
  }
  // Both production interfaces dispatch to the same bounded reader/state.
  taxi_camera::engine_camera::MemoryReader& entry_reader = reader;
  taxi_camera::discovery::AircraftObjectReader& aircraft_reader = reader;
  require(entry_reader.read(allocation.address(), output.data(), 24) && aircraft_reader.read(allocation.address(), output.data(), 16),
          "Multiple inherited reader interfaces did not share the implementation");
  expected_bytes += 40;
  require(reader.attempted_bytes() == expected_bytes, "Interface dispatch bypassed the shared allowance");

  const auto before_invalid = reader.attempted_bytes();
  require(!reader.read(0, output.data(), 8) && !reader.read(allocation.address(), nullptr, 8) &&
              !reader.read(allocation.address(), output.data(), 0) &&
              !reader.read(allocation.address(), output.data(), kLocalObjectFieldLimit + 1) &&
              !reader.read(std::numeric_limits<std::uint64_t>::max() - 3, output.data(), 8) && reader.attempted_bytes() == before_invalid,
          "Invalid field request reached a query/read or exceeded the size cap");

  reader.reset_budget(24);
  require(reader.budget_limit() == 24 && reader.attempted_bytes() == 0 && reader.read_failures() == 0, "Explicit budget reset failed");
  require(reader.read(allocation.address(), output.data(), 24), "Exact shared allowance was not usable");
  require(!reader.read(allocation.address(), output.data(), 1) && reader.attempted_bytes() == 24,
          "Read beyond exhausted allowance was issued or automatically reset");
  reader.reset_budget(0);
  require(!reader.read(allocation.address(), output.data(), 1) && reader.attempted_bytes() == 0, "Zero allowance did not disable reads");
  reader.reset_budget(std::numeric_limits<std::uint32_t>::max());
  require(reader.budget_limit() == kLocalObjectReadLimit, "Object allowance was not clamped to its hard maximum");

  DWORD previous = 0;
  for (const auto protection : {PAGE_READONLY, PAGE_READWRITE, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE}) {
    require(VirtualProtect(allocation.data + page, page, protection, &previous) != 0, "Could not set readable fixture protection");
    reader.reset_budget();
    require(reader.read(allocation.address(page), output.data(), 16) && reader.read_failures() == 0,
            "Documented readable committed private page was rejected");
  }
  for (const auto protection : {PAGE_NOACCESS, PAGE_EXECUTE, PAGE_READWRITE | PAGE_GUARD}) {
    require(VirtualProtect(allocation.data + page, page, protection, &previous) != 0, "Could not set refused fixture protection");
    reader.reset_budget();
    output.fill(0xa7);
    require(!reader.read(allocation.address(page), output.data(), 16) && reader.attempted_bytes() == 16 && reader.read_failures() == 1,
            "No-access, execute-only or guarded private page was not refused and counted");
    require(std::all_of(output.begin(), output.end(), [](auto value) { return value == 0xa7; }),
            "Failed page read changed the caller's output");
    MEMORY_BASIC_INFORMATION region{};
    require(VirtualQuery(allocation.data + page, &region, sizeof(region)) == sizeof(region) && region.Protect == DWORD(protection),
            "Read attempt consumed a guard or changed page protection");
  }
  require(VirtualProtect(allocation.data + page, page, PAGE_READONLY, &previous) != 0, "Could not split fixture memory regions");
  reader.reset_budget();
  require(reader.read(allocation.address(page - 8), output.data(), 16) && reader.attempted_bytes() == 16 && reader.read_failures() == 0,
          "A complete readable field could not cross separately protected private regions");
  for (unsigned i = 0; i < 16; ++i)
    require(output[i] == std::uint8_t(page - 8 + i), "Scalar region-boundary read changed byte order");
  require(VirtualProtect(allocation.data + page, page, PAGE_READWRITE, &previous) != 0, "Could not restore fixture page");
  require(VirtualFree(allocation.data + page, page, MEM_DECOMMIT) != 0, "Could not decommit fixture page");
  reader.reset_budget();
  require(!reader.read(allocation.address(page), output.data(), 8) && reader.attempted_bytes() == 8 && reader.read_failures() == 1,
          "Reserved/decommitted storage was read as committed private data");
  reader.reset_budget();
  require(!reader.read(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)), output.data(), 8) && reader.read_failures() == 1,
          "Private-object reader accepted main-image storage");

  const auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(page), nullptr);
  require(mapping != nullptr, "Could not create mapped-data fixture");
  const auto mapped = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, page);
  require(mapped != nullptr, "Could not map data fixture");
  reader.reset_budget();
  require(!reader.read(reinterpret_cast<std::uintptr_t>(mapped), output.data(), 8) && reader.read_failures() == 1,
          "Private-object reader accepted MEM_MAPPED storage");
  require(UnmapViewOfFile(mapped) != 0 && CloseHandle(mapping) != 0, "Could not release mapped-data fixture");
}

void large_private_fields() {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const auto page = std::size_t(info.dwPageSize);
  Allocation allocation(kLocalObjectFieldLimit + page * 2);
  for (std::size_t i = 0; i < allocation.size; ++i)
    allocation.data[i] = static_cast<std::uint8_t>((i * 17) ^ 0x69);
  std::vector<std::uint8_t> output(kLocalObjectFieldLimit + 16, 0xaa);
  LocalMemoryReader reader;
  for (const auto size : {65u, 4095u, 4096u, 4097u, 16384u, kLocalObjectFieldLimit}) {
    reader.reset_budget();
    std::fill(output.begin(), output.end(), 0xaa);
    require(reader.read(allocation.address(3), output.data(), size) && reader.attempted_bytes() == size && reader.read_failures() == 0,
            "Exact large private field was refused or incorrectly accounted");
    for (std::size_t i = 0; i < output.size(); ++i)
      require(output[i] == (i < size ? allocation.data[i + 3] : 0xaa), "Large read altered bytes outside its exact output extent");
  }
  DWORD previous = 0;
  // Split the middle of a valid field into a distinct readable region. Each
  // region must be checked/read, without broadening the requested field.
  require(VirtualProtect(allocation.data + page * 3, page, PAGE_READONLY, &previous) != FALSE, "Could not split a large field");
  reader.reset_budget();
  require(reader.read(allocation.address(3), output.data(), kLocalObjectFieldLimit), "Large field could not span readable private regions");
  for (std::size_t i = 0; i < kLocalObjectFieldLimit; ++i)
    require(output[i] == allocation.data[i + 3], "A multi-region field was concatenated incorrectly");

  for (const auto access : {PAGE_NOACCESS, PAGE_EXECUTE, PAGE_READWRITE | PAGE_GUARD}) {
    require(VirtualProtect(allocation.data + page * 3, page, access, &previous) != FALSE,
            "Could not protect the large field's middle page");
    reader.reset_budget();
    std::fill(output.begin(), output.end(), 0xa7);
    require(!reader.read(allocation.address(3), output.data(), kLocalObjectFieldLimit) &&
                reader.attempted_bytes() == kLocalObjectFieldLimit && reader.read_failures() == 1,
            "An inaccessible middle page did not refuse the entire large read");
    require(std::all_of(output.begin(), output.end(), [](auto byte) { return byte == 0xa7; }),
            "Successful initial regions leaked into output after a later-region failure");
    MEMORY_BASIC_INFORMATION region{};
    require(VirtualQuery(allocation.data + page * 3, &region, sizeof(region)) == sizeof(region) && region.Protect == DWORD(access),
            "Large read consumed the middle page guard or changed its protection");
  }
  require(VirtualProtect(allocation.data + page * 3, page, PAGE_READWRITE, &previous) != FALSE, "Could not restore the middle page");
  require(VirtualFree(allocation.data + page * 3, page, MEM_DECOMMIT) != FALSE, "Could not decommit the middle page");
  reader.reset_budget();
  std::fill(output.begin(), output.end(), 0xa7);
  require(!reader.read(allocation.address(), output.data(), kLocalObjectFieldLimit) && reader.read_failures() == 1 &&
              std::all_of(output.begin(), output.end(), [](auto byte) { return byte == 0xa7; }),
          "A reserved middle page was accepted or exposed partially read bytes");
  reader.reset_budget(kLocalObjectFieldLimit - 1);
  require(!reader.read(allocation.address(), output.data(), kLocalObjectFieldLimit) && reader.attempted_bytes() == 0 &&
              reader.read_failures() == 0,
          "An oversized-for-budget bulk request reached memory access");
}

void bulk_read_profile() {
  Allocation allocation(kLocalObjectFieldLimit);
  for (std::size_t i = 0; i < allocation.size; ++i)
    allocation.data[i] = static_cast<std::uint8_t>(i ^ 0x5a);
  MEMORY_BASIC_INFORMATION region{};
  require(VirtualQuery(allocation.data, &region, sizeof(region)) == sizeof(region) && region.State == MEM_COMMIT &&
              region.Type == MEM_PRIVATE && region.RegionSize >= allocation.size,
          "Read-call comparison needs one contiguous readable private region");
  std::array<std::uint8_t, kLocalObjectFieldLimit> scalar{}, bulk{};
  LARGE_INTEGER frequency{}, start{}, middle{}, finish{};
  require(QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&start), "Could not start local read timing");
  LocalMemoryReader reader;
  constexpr unsigned repeats = 4;
  for (unsigned iteration = 0; iteration < repeats; ++iteration) {
    reader.reset_budget();
    for (unsigned pass = 0; pass < 2; ++pass)
      for (std::size_t offset = 0; offset < scalar.size(); offset += 8)
        require(reader.read(allocation.address(offset), scalar.data() + offset, 8), "Scalar comparison read failed");
    require(reader.attempted_bytes() == 2 * kLocalObjectFieldLimit, "Scalar comparison byte count changed");
  }
  require(QueryPerformanceCounter(&middle), "Could not time the scalar read comparison");
  for (unsigned iteration = 0; iteration < repeats; ++iteration) {
    reader.reset_budget();
    for (unsigned pass = 0; pass < 2; ++pass)
      require(reader.read(allocation.address(), bulk.data(), bulk.size()), "Bulk comparison read failed");
    require(reader.attempted_bytes() == 2 * kLocalObjectFieldLimit, "Bulk comparison byte count changed");
  }
  require(QueryPerformanceCounter(&finish) && scalar == bulk && std::memcmp(bulk.data(), allocation.data, bulk.size()) == 0,
          "Scalar and batched reads observed different bytes");
  const auto milliseconds = [&](LONGLONG ticks) { return double(ticks) * 1000.0 / double(frequency.QuadPart) / repeats; };
  std::printf("Own-memory 32768-byte field + reread: 8192 scalar read requests vs 2 batched, %.3f ms vs %.3f ms per pair.\n",
              milliseconds(middle.QuadPart - start.QuadPart), milliseconds(finish.QuadPart - middle.QuadPart));
  // Timing is diagnostic only. The deterministic regression is exact bytes and
  // two reads, each using one VQ/RPM pair for this verified single-region fixture.
}

void main_image_reads() {
  const auto module = GetModuleHandleW(nullptr);
  LocalImageReader reader(module, 0x80000000);
  std::array<std::uint8_t, 64> output{};
  require(reader.read(0, output.data(), 64) && output[0] == 'M' && output[1] == 'Z', "Current main image could not be read safely");
  const auto pe = u32(output.data() + 60);
  require(reader.read(pe + 24 + 56, output.data(), 4), "Self image-size field could not be read");
  const auto image_size = u32(output.data());
  LocalImageReader bounded(module, image_size);
  require(!bounded.query(image_size, 1).readable && bounded.query(image_size, 1).size == 0 &&
              !bounded.read(image_size - 1, output.data(), 2) && !bounded.read(0, nullptr, 1) && !bounded.read(0, output.data(), 0),
          "Image bounds or invalid destinations were accepted");
  const auto first = bounded.query(0, image_size);
  require(first.readable && first.size != 0 && first.size <= image_size, "Image query failed to clip to its first region");
  if (first.size < image_size && first.size >= 8) {
    const auto second = bounded.query(first.size, 8);
    if (second.readable && second.size == 8)
      require(bounded.read(first.size - 8, output.data(), 16), "Image reader failed a readable region-boundary loop");
  }
  // Even with a deliberately oversized bound, a page past the actual main
  // allocation must not become readable merely because it is mapped elsewhere.
  const auto past = reader.query(image_size, 8);
  require(!past.readable && !reader.read(image_size, output.data(), 8), "Image reader escaped the main module allocation");
  LocalImageReader tiny(module, 2);
  require(tiny.read(0, output.data(), 2) && tiny.query(0, 64).size == 2 && !tiny.read(1, output.data(), 2),
          "Caller-supplied image bound was not enforced");
  LocalImageReader zero(module, 0);
  LocalImageReader excessive(module, 0xffffffff);
  LocalImageReader null_module(nullptr, 4096);
  LocalImageReader other_module(GetModuleHandleW(L"kernel32.dll"), 4096);
  require(zero.query(0, 1).size == 0 && excessive.query(0, 1).size == 0 && null_module.query(0, 1).size == 0 &&
              other_module.query(0, 1).size == 0,
          "Invalid size or foreign module became an enabled image reader");
  Allocation private_image(4096);
  LocalImageReader impostor(reinterpret_cast<HMODULE>(private_image.data), 4096);
  require(!impostor.read(0, output.data(), 2), "Private allocation was accepted as the main executable");
  const auto self_result = parse_verified_main_image();
  require(self_result.valid_image && self_result.error.empty() && !self_result.sections.empty() && self_result.records.empty() &&
              self_result.scanned_bytes == 0 && self_result.metadata_bytes <= 8192,
          "Structural header inspection failed or scanned code in the test executable");
}

struct HeaderFixture final : taxi_camera::discovery::ImageReader {
  static constexpr std::uint32_t Pe = 128;
  static constexpr std::uint32_t Optional = Pe + 24;
  static constexpr std::uint32_t Sections = Optional + 240;
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(65536, 0);
  std::uint32_t read_bytes = 0;
  std::uint32_t query_calls = 0;
  std::uint32_t largest_end = 0;
  std::uint32_t fail_at = 0xffffffff;
  std::uint32_t query_mode = 0;
  std::uint32_t window_size = 65536;
  std::uint32_t available = 65536;

  void put(std::uint32_t offset, std::uint64_t value, unsigned size = 4) {
    for (unsigned index = 0; index < size; ++index)
      bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
  }
  HeaderFixture() {
    put(0, 0x5a4d, 2);
    put(60, Pe);
    put(Pe, 0x4550);
    put(Pe + 4, 0x8664, 2);
    put(Pe + 6, kVerifiedImageSections, 2);
    put(Pe + 8, kVerifiedImageTimestamp);
    put(Pe + 20, 240, 2);
    put(Optional, 0x20b, 2);
    put(Optional + 56, kVerifiedImageSize);
    put(Optional + 60, 4096);
    put(Optional + 64, 0x12345678);
    put(Optional + 70, 0x4160, 2);
    put(Optional + 108, 16);
    put(Optional + 112 + 3 * 8, 0x10000);
    put(Optional + 116 + 3 * 8, 12);
    put(Optional + 112 + 10 * 8, 0x20000);
    put(Optional + 116 + 10 * 8, 148);
    for (std::uint32_t index = 0; index < kVerifiedImageSections; ++index) {
      const auto offset = Sections + index * 40;
      std::memcpy(bytes.data() + offset, "SECTION!", 8);
      put(offset + 8, 4096);
      put(offset + 12, 0x10000 + index * 0x10000);
      put(offset + 16, 512);
      put(offset + 20, 0xffffff00);  // Unused on-disk file offset.
      put(offset + 36, 0x40000040);
    }
  }
  taxi_camera::discovery::ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    ++query_calls;
    require(rva < bytes.size() && maximum <= bytes.size() - rva, "Header query escaped the fixed image prefix");
    if (rva >= available)
      return {};
    if (query_mode == 1)
      return {0, true};
    if (query_mode == 2)
      return {maximum + 1, true};
    if (query_mode == 3)
      return {maximum, false};
    return {std::min({maximum, window_size, available - rva}), true};
  }
  bool read(std::uint32_t rva, void* destination, std::size_t size) override {
    require(rva < bytes.size() && size <= bytes.size() - rva && read_bytes + size <= 8192,
            "Header read escaped its prefix or byte allowance");
    read_bytes += static_cast<std::uint32_t>(size);
    largest_end = std::max(largest_end, rva + static_cast<std::uint32_t>(size));
    if (rva <= fail_at && fail_at - rva < size)
      return false;
    std::memcpy(destination, bytes.data() + rva, size);
    return true;
  }
  taxi_camera::discovery::Inventory run() {
    const auto result = parse_verified_image_headers(*this);
    require(result.metadata_bytes <= 8192 && result.metadata_bytes >= read_bytes && result.scanned_bytes == 0 && result.records.empty(),
            "Header parser scanned data or undercounted attempted metadata bytes");
    require(result.valid_image == result.error.empty(), "Header validity/error state disagrees");
    return result;
  }
};

void synthetic_headers() {
  for (const auto window : {1u, 7u, 64u, 65536u}) {
    HeaderFixture fixture;
    fixture.window_size = window;
    const auto result = fixture.run();
    require(result.valid_image && result.sections.size() == 14 && result.machine == 0x8664 && result.timestamp == kVerifiedImageTimestamp &&
                result.image_size == kVerifiedImageSize && result.metadata_bytes == 888 && result.read_failures == 0 &&
                result.exception_rva == 0x10000 && result.exception_size == 12 && result.load_config_rva == 0x20000 &&
                result.load_config_size == 148 && result.checksum == 0x12345678 && result.dll_characteristics == 0x4160 &&
                result.sections[0].name == "SECTION!" && result.sections[0].size == 4096 &&
                fixture.largest_end == HeaderFixture::Sections + 14 * 40,
            "Exact supported synthetic headers did not yield bounded loaded-image metadata");
  }
  for (const auto sections : {13u, 15u, 96u}) {
    HeaderFixture fixture;
    fixture.put(HeaderFixture::Pe + 8, kVerifiedImageTimestamp + 123);
    fixture.put(HeaderFixture::Optional + 56, kVerifiedImageSize + 4096);
    fixture.put(HeaderFixture::Pe + 6, sections, 2);
    fixture.put(HeaderFixture::Optional + 60, 8192);
    const auto result = fixture.run();
    require(result.valid_image && result.timestamp == kVerifiedImageTimestamp + 123 && result.image_size == kVerifiedImageSize + 4096 &&
                result.sections.size() == sections,
            "Changed build metadata with valid bounds was rejected");
  }
  {
    HeaderFixture fixture;
    fixture.put(HeaderFixture::Optional + 56, 0x80000001u);
    require(!fixture.run().valid_image, "Excessive image extent was accepted");
  }
  for (unsigned scenario = 0; scenario < 26; ++scenario) {
    HeaderFixture fixture;
    const auto optional = HeaderFixture::Optional;
    const auto sections = HeaderFixture::Sections;
    switch (scenario) {
      case 0:
        fixture.put(0, 0, 2);
        break;
      case 1:
        fixture.put(60, 0xffffffff);
        break;
      case 2:
        fixture.put(60, 65520);
        break;
      case 3:
        fixture.put(60, 1);
        break;
      case 4:
        fixture.put(HeaderFixture::Pe, 0);
        break;
      case 5:
        fixture.put(HeaderFixture::Pe + 4, 0x14c, 2);
        break;
      case 6:
        fixture.put(HeaderFixture::Pe + 6, 0, 2);
        break;
      case 7:
        fixture.put(HeaderFixture::Pe + 6, 97, 2);
        break;
      case 8:
        fixture.put(HeaderFixture::Pe + 20, 111, 2);
        break;
      case 9:
        fixture.put(HeaderFixture::Pe + 20, 4097, 2);
        break;
      case 10:
        fixture.put(optional, 0x10b, 2);
        break;
      case 11:
        fixture.put(optional + 56, 0);
        break;
      case 12:
        fixture.put(optional + 60, 0);
        break;
      case 13:
        fixture.put(optional + 60, 65537);
        break;
      case 14:
        fixture.put(optional + 60, sections + 14 * 40 - 1);
        break;
      case 15:
        fixture.put(optional + 108, 17);
        break;
      case 16:
        fixture.put(sections + 12, 4095);
        break;
      case 17:
        fixture.put(sections + 8, 0xffffffff);
        break;
      case 18:
        fixture.put(sections + 40 + 12, 0x10008);
        break;
      case 19:
        fixture.put(sections + 12, kVerifiedImageSize - 1);
        break;
      case 20:
        fixture.put(optional + 112 + 3 * 8, 0xffffffff);
        break;
      case 21:
        fixture.put(optional + 116 + 3 * 8, 0);
        break;
      case 22:
        fixture.put(optional + 112 + 10 * 8, 0);
        break;
      case 23:
        fixture.put(optional + 116 + 10 * 8, 4097);
        break;
      case 24:
        fixture.put(optional + 112 + 3 * 8, 100);
        break;
      case 25:
        fixture.put(optional + 108, 0);
        for (unsigned index = 0; index < 14; ++index) {
          fixture.put(sections + index * 40 + 8, 0);
          fixture.put(sections + index * 40 + 16, 0);
        }
        break;
    }
    require(!fixture.run().valid_image, "Malformed or stale synthetic headers were accepted");
  }
  for (const auto failure : {0u, HeaderFixture::Pe, HeaderFixture::Optional, HeaderFixture::Sections}) {
    HeaderFixture fixture;
    fixture.fail_at = failure;
    const auto result = fixture.run();
    require(!result.valid_image && result.read_failures == 1, "Header read failure was not counted/refused");
  }
  for (const auto mode : {1u, 2u, 3u}) {
    HeaderFixture fixture;
    fixture.query_mode = mode;
    const auto result = fixture.run();
    require(!result.valid_image && result.read_failures == 1 && fixture.read_bytes == 0,
            "Zero, oversized or unreadable query window reached a header read");
  }
  {
    HeaderFixture fixture;
    fixture.available = HeaderFixture::Sections + 12;
    require(!fixture.run().valid_image, "Truncated section-header table was accepted");
  }
  {
    HeaderFixture fixture;
    fixture.put(HeaderFixture::Optional + 108, 0);
    const auto result = fixture.run();
    require(result.valid_image && result.exception_rva == 0 && result.load_config_rva == 0,
            "Absent optional directories caused unrequested metadata reads");
  }
  {
    HeaderFixture fixture;
    fixture.put(HeaderFixture::Sections + 8, 0);
    const auto result = fixture.run();
    require(result.valid_image && result.sections[0].size == 512, "Zero virtual size did not use the declared loaded-size fallback");
  }
}

}  // namespace

int main() {
  public_query_equivalence();
  measured_reads();
  cached_queries();
  fused_inspection_transactions();
  cache_protection_changes();
  cache_bounds();
  cache_profile();
  descending_cache_profile();
  canonical_query_ranges();
  private_memory_reads();
  large_private_fields();
  bulk_read_profile();
  main_image_reads();
  synthetic_headers();
  std::printf("PASS: %u local memory and header checks. Own allocations/self image and synthetic headers only.\n", checks);
  return 0;
}
