#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <string>

#include "aircraft_inventory.hpp"
#include "memory_reader.hpp"

namespace taxi_camera::native_camera {

inline constexpr std::uint32_t kLocalObjectReadLimit = 131072;
inline constexpr std::uint32_t kLocalObjectFieldLimit = 32768;

// Optional, thread-local CPU diagnostics. These count real OS queries/reads,
// including failed calls and image reads; ticks use QueryPerformanceCounter.
// No timing calls are added when no scope is active. Budgets remain separate.
struct LocalMemoryMetrics {
  std::uint64_t query_calls = 0;
  std::uint64_t query_allocation_calls = 0;
  std::uint64_t query_page_calls = 0;
  std::uint64_t query_fallback_calls = 0;
  std::uint64_t read_calls = 0;
  std::uint64_t requested_bytes = 0;
  std::uint64_t query_ticks = 0;
  // query_ticks by kind (allocation, working-set page, region fallback) and
  // each kind's slowest single call, which names the kernel call that stalls.
  std::uint64_t query_allocation_ticks = 0, query_page_ticks = 0, query_fallback_ticks = 0;
  std::uint64_t query_allocation_max_ticks = 0, query_page_max_ticks = 0, query_fallback_max_ticks = 0;
  std::uint64_t read_ticks = 0;
  // write_local_private stores (guarded copy or WriteProcessMemory), failed
  // ones included, and their ticks. Their proofs count as queries.
  std::uint64_t write_calls = 0;
  std::uint64_t write_ticks = 0;
  std::uint64_t query_cache_hits = 0;
  std::uint64_t query_cache_validation_failures = 0;
  // Page proofs taken from an active LocalMemoryProofCarry instead of a query.
  std::uint64_t carried_proofs = 0;
};

// The innermost active scope on this thread, or null. Read-only view for
// callers that attribute counts to their own sub-stages.
const LocalMemoryMetrics* active_local_memory_metrics() noexcept;

class ScopedLocalMemoryMetrics {
 public:
  explicit ScopedLocalMemoryMetrics(LocalMemoryMetrics& metrics) noexcept;
  ~ScopedLocalMemoryMetrics();
  ScopedLocalMemoryMetrics(const ScopedLocalMemoryMetrics&) = delete;
  ScopedLocalMemoryMetrics& operator=(const ScopedLocalMemoryMetrics&) = delete;

 private:
  LocalMemoryMetrics* previous_ = nullptr;
};

// First failure only, captured from existing queries without any extra reads or
// allocation. Values describe one differing metadata field, never object data.
struct LocalMemoryQueryFailure {
  const char* stage = "none";
  const char* field = "none";
  std::uintptr_t address = 0;
  std::size_t region_index = 0;
  std::uint64_t expected = 0, observed = 0;
  DWORD system_error = 0;
};

std::string describe_local_memory_query_failure(const LocalMemoryQueryFailure& failure);

enum class LocalMemoryQueryMode { full_regions, private_pages };
enum class LocalImageQueryMode { full_regions, pages };

// Fresh, current-process view-field helpers. The span is bounded to
// kLocalWriteLimit bytes (at most two pages) in one committed private
// allocation, with exactly PAGE_READWRITE throughout. No proof is retained
// across writes/native calls. The reads copy only after an exact RPM of at
// most kLocalBytesReadLimit bytes; their caller must establish ownership and
// writability.
inline constexpr std::size_t kLocalWriteLimit = 64;
inline constexpr std::size_t kLocalBytesReadLimit = 24;
bool writable_private_span(std::uint64_t address, std::size_t size) noexcept;
bool read_local_flag_words(std::uint64_t address, std::array<std::uint64_t, 2>& flags) noexcept;
bool read_local_bytes(std::uint64_t address, void* destination, std::size_t size) noexcept;
// Stores 1..kLocalWriteLimit bytes after its own fresh writable_private_span
// proof of exactly that span, with nothing between the proof and the store.
// The store is one guarded copy: a page decommitted, protected or guarded
// after the proof faults inside it and the copy reports failure, re-arming a
// consumed guard page; bytes before a faulting page may already be written,
// as with a partial WriteProcessMemory. Without the fault handler the store is
// WriteProcessMemory, never an unguarded copy. False for any refusal or
// failure; callers keep treating an attempt as a mutation and reread.
bool write_local_private(std::uint64_t address, const void* data, std::size_t size) noexcept;

#ifdef TAXI_LOCAL_MEMORY_TESTING
// Compiled only into focused fixtures, never the delivered bridge.
struct LocalMemoryQueryTestFaults {
  bool allocation_unavailable = false;
  bool pages_unavailable = false;
  bool pages_nonresident = false;
};
void set_local_memory_query_test_faults(LocalMemoryQueryTestFaults faults) noexcept;
// Runs inside write_local_private on this thread, after its proof and before
// its store; without_guarded_copy forces the WriteProcessMemory store. A null
// hook with false restores the delivered path.
using LocalMemoryWriteTestHook = void (*)(void* context, std::uint64_t address, std::size_t size);
void set_local_memory_write_test_hook(LocalMemoryWriteTestHook hook, void* context, bool without_guarded_copy = false) noexcept;
#endif

// Page and allocation proofs carried between private-page scopes on one
// thread, only while a ScopedLocalMemoryProofCarry is active. The probe uses it
// for the scheduled frames of an established camera pair (pulses opening, held
// open or closing): the first frame proves every page as usual and the
// following frames take those proofs instead of querying the kernel again,
// until the owner clears it (at most every 250 ms, and on lifecycle work). A carried proof skips both the initial query and the endpoint
// check; reads still go through ReadProcessMemory, which refuses a page that
// was freed or protected since. A failed scope or any write clears it.
struct LocalMemoryProofCarry {
  static constexpr std::size_t kPageLimit = 256;
  struct Page {
    std::uintptr_t base = 0, allocation_base = 0;
    std::size_t allocation_size = 0;
    DWORD protection = 0, allocation_protection = 0, type = 0;
  };
  std::array<Page, kPageLimit> pages{};
  std::size_t count = 0;
  void clear() noexcept { count = 0; }
  const Page* find(std::uintptr_t base) const noexcept;
  void remember(const Page& page) noexcept;
};

// Makes carry the active proof carry on this thread for its lifetime.
class ScopedLocalMemoryProofCarry {
 public:
  explicit ScopedLocalMemoryProofCarry(LocalMemoryProofCarry& carry) noexcept;
  ~ScopedLocalMemoryProofCarry();
  ScopedLocalMemoryProofCarry(const ScopedLocalMemoryProofCarry&) = delete;
  ScopedLocalMemoryProofCarry& operator=(const ScopedLocalMemoryProofCarry&) = delete;

 private:
  LocalMemoryProofCarry* previous_ = nullptr;
};

// One read-only inspection stage only; no private calls may run inside it. The
// full_regions mode caches region metadata, never contents. An earlier-page
// metadata probe is accepted only if it covers the requested address; otherwise
// query that exact address. This backend uses at most two initial queries plus
// one endpoint query per saved region (192 calls); no metadata merging. The
// alignment bounds the prefix, not VirtualQueryEx's internal suffix scan.
// Exact RPM reads and
// the caller's complete trace rereads still run. finish() checks every saved
// allocation, extent, state, type and protection against fresh region metadata.
// A fresh wider query can prove overlapping suffixes within that endpoint only;
// uncovered regions get independent queries and conflicting observations fail.
// Results must remain local until finish() succeeds. A failure poisons the scope;
// there is no eviction or retry, and destruction discards all cached metadata.
// This is not an atomic lifetime proof: an allocation/protection ABA between
// checks remains possible. RPM enforces access if protection changes midstage.
class ScopedLocalMemoryQueryCache {
 public:
  static constexpr std::size_t kRegionLimit = 64;
  static constexpr std::size_t kPageLimit = 128;
  static constexpr std::size_t kAllocationLimit = 64;
  // Pages queried alongside a miss: earlier scopes' pages on this thread in
  // the same allocation. Only their addresses carry over between scopes.
  static constexpr std::size_t kCandidateLimit = 64;
  // private_pages keeps fresh allocation identity/extent and the protection of
  // every requested private page, rather than the unrelated homogeneous suffix.
  // A cold private page is read with RPM first and then proven. Pages still
  // out of the working set (including cold image pages), failed page queries
  // and capacity overflow use the full-region backend.
  // Explicit image-page readers share these bounded proofs with distinct image
  // allocation identity. Direct MBI/default image queries stay exact.
  explicit ScopedLocalMemoryQueryCache(LocalMemoryQueryMode mode = LocalMemoryQueryMode::full_regions) noexcept;
  ~ScopedLocalMemoryQueryCache();
  ScopedLocalMemoryQueryCache(const ScopedLocalMemoryQueryCache&) = delete;
  ScopedLocalMemoryQueryCache& operator=(const ScopedLocalMemoryQueryCache&) = delete;
  bool finish() noexcept;
  // A borrowed pure substage may join only the currently active transaction.
  bool is_current() const noexcept;
  // Reader implementation only. A failed RPM also poisons this stage.
  SIZE_T query(const void* address, MEMORY_BASIC_INFORMATION& region) noexcept;
  bool uses_private_pages() const noexcept { return mode_ == LocalMemoryQueryMode::private_pages; }
  // With cold, a page of a proven ordinary private allocation that is outside
  // the working set sets *cold and returns false without a proof or refusal:
  // the caller reads it with RPM (which faults it in and never consumes a
  // guard page) and validates again before using the bytes.
  bool validate_private_range(std::uintptr_t address, std::size_t size, bool* cold = nullptr) noexcept;
  bool validate_image_range(std::uintptr_t address, std::size_t size, std::uintptr_t module) noexcept;
  void fail(const void* address, SIZE_T requested, SIZE_T copied, DWORD error) noexcept;
  const LocalMemoryQueryFailure& failure() const noexcept { return failure_; }

 private:
  struct AllocationProof {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    DWORD protection = 0;
    DWORD type = MEM_PRIVATE;
  };
  struct PageProof {
    std::uintptr_t base = 0;
    DWORD protection = 0;
    std::size_t allocation = 0;
  };
  bool finish_pages() noexcept;
  bool query_page_batch(std::uintptr_t base, std::size_t allocation, std::uintptr_t& requested_flags) noexcept;
  bool validate_page_range(std::uintptr_t address, std::size_t size, DWORD type, std::uintptr_t allocation, bool* cold = nullptr) noexcept;
  bool refuse(const char* stage,
              const char* field,
              std::uintptr_t address,
              std::size_t index,
              std::uint64_t expected,
              std::uint64_t observed,
              DWORD error = ERROR_SUCCESS) noexcept;
  ScopedLocalMemoryQueryCache* previous_ = nullptr;
  LocalMemoryQueryMode mode_;
  std::array<AllocationProof, kAllocationLimit> allocations_{};
  std::array<PageProof, kPageLimit> pages_{};
  // Proofs taken from the active LocalMemoryProofCarry: finish() does not
  // query them again. A freshly proven allocation clears its flag.
  std::array<bool, kAllocationLimit> allocation_carried_{};
  std::array<bool, kPageLimit> page_carried_{};
  // Fresh proofs from a batch query, used only once a read requests the page
  // (then it moves to pages_ and its endpoint is checked by finish()).
  std::array<PageProof, kCandidateLimit> candidates_{};
  std::size_t allocation_count_ = 0;
  std::size_t page_count_ = 0;
  std::size_t candidate_count_ = 0;
  std::array<MEMORY_BASIC_INFORMATION, kRegionLimit> regions_{};
  std::size_t count_ = 0;
  LocalMemoryQueryFailure failure_{};
  bool failed_ = false;
  bool active_ = true;
};

// Current process only; no handles/PIDs to other processes are accepted. Every
// 1..32768-byte request must fit committed readable MEM_PRIVATE regions. Page
// queries never consume guard pages or change protection. An optional stage
// cache requires successful endpoint validation before consuming read results.
// Failed query/RPM
// attempts consume their requested bytes; invalid requests/budget refusals do
// not issue a query/read. Output is copied only after an exact successful RPM.
class LocalMemoryReader final : public engine_camera::MemoryReader, public discovery::AircraftObjectReader {
 public:
  explicit LocalMemoryReader(std::uint32_t limit = kLocalObjectReadLimit) noexcept;
  bool read(std::uint64_t address, void* destination, std::size_t size) noexcept override;
  // Caller-controlled stage boundary only. Clamp to the fixed upper bound;
  // zero disables reads. Not thread-safe and never resets itself on failure.
  void reset_budget(std::uint32_t limit = kLocalObjectReadLimit) noexcept;
  std::uint32_t attempted_bytes() const noexcept { return attempted_; }
  std::uint32_t read_failures() const noexcept { return failures_; }
  std::uint32_t budget_limit() const noexcept { return limit_; }

 private:
  std::uint32_t limit_ = 0;
  std::uint32_t attempted_ = 0;
  std::uint32_t failures_ = 0;
};

// Explicit main executable only. The supplied size is a read bound, not an
// identity proof. Default query clips to that bound and one VirtualQuery region;
// read can span readable regions but requires exact MEM_IMAGE allocation
// identity on every part. Logical PE-section restrictions belong to the caller.
// Explicit pages mode clips to requested pages, checking fresh allocation/type
// and current page access. A private_pages scope retains those proofs through
// finish(); outside a scope every query is fresh. Full-region scopes and startup
// readers keep the existing query extents and scanner request-count budgets.
class LocalImageReader final : public discovery::ImageReader {
 public:
  LocalImageReader(HMODULE main_module, std::uint32_t image_size, LocalImageQueryMode mode = LocalImageQueryMode::full_regions) noexcept;
  discovery::ReadWindow query(std::uint32_t rva, std::uint32_t maximum) noexcept override;
  bool read(std::uint32_t rva, void* destination, std::size_t size) noexcept override;

 private:
  HMODULE module_ = nullptr;
  std::uintptr_t base_ = 0;
  std::uint32_t limit_ = 0;
  LocalImageQueryMode mode_ = LocalImageQueryMode::full_regions;
};

// Testable header-only core: at most 8192 requested metadata bytes, all within
// the first 65536 image bytes. Validates AMD64 PE32+, at most 96 sections,
// an image extent of at most 2 GiB, directory bounds and nonoverlapping sections.
// Timestamp, checksum, image size and section count are observed metadata,
// never a build allowlist. Reads no section body, exports or code.
// This is structural validation, not process identity or callable code proof.
discovery::Inventory parse_verified_image_headers(discovery::ImageReader& reader);

// Supplies GetModuleHandleW(nullptr) to the bounded core. No module enumeration,
// disk access, process-name search, private calls, hook install or scans. The
// native runtime separately owns executable-name and instruction verification.
discovery::Inventory parse_verified_main_image();

}  // namespace taxi_camera::native_camera
