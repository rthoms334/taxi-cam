#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>

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
  std::uint64_t read_calls = 0;
  std::uint64_t requested_bytes = 0;
  std::uint64_t query_ticks = 0;
  std::uint64_t read_ticks = 0;
  std::uint64_t query_cache_hits = 0;
  std::uint64_t query_cache_validation_failures = 0;
};

class ScopedLocalMemoryMetrics {
 public:
  explicit ScopedLocalMemoryMetrics(LocalMemoryMetrics& metrics) noexcept;
  ~ScopedLocalMemoryMetrics();
  ScopedLocalMemoryMetrics(const ScopedLocalMemoryMetrics&) = delete;
  ScopedLocalMemoryMetrics& operator=(const ScopedLocalMemoryMetrics&) = delete;

 private:
  LocalMemoryMetrics* previous_ = nullptr;
};

// One read-only inspection stage only; no private calls may run inside it. The
// first query caches region metadata, never contents. A bounded earlier-page
// metadata probe is accepted only if it covers the requested address; otherwise
// query that exact address. At most two initial queries plus one endpoint query
// per saved region (192 calls total); no metadata merging. Exact RPM reads and
// the caller's complete trace rereads still run. finish() freshly queries every
// cached region and compares its allocation, extent, state, type and protection.
// Results must remain local until finish() succeeds. A failure poisons the scope;
// there is no eviction or retry, and destruction discards all cached metadata.
// This is not an atomic lifetime proof: an allocation/protection ABA between
// checks remains possible. RPM enforces access if protection changes midstage.
class ScopedLocalMemoryQueryCache {
 public:
  static constexpr std::size_t kRegionLimit = 64;
  ScopedLocalMemoryQueryCache() noexcept;
  ~ScopedLocalMemoryQueryCache();
  ScopedLocalMemoryQueryCache(const ScopedLocalMemoryQueryCache&) = delete;
  ScopedLocalMemoryQueryCache& operator=(const ScopedLocalMemoryQueryCache&) = delete;
  bool finish() noexcept;
  // A borrowed pure substage may join only the currently active transaction.
  bool is_current() const noexcept;
  // Reader implementation only. A failed RPM also poisons this stage.
  SIZE_T query(const void* address, MEMORY_BASIC_INFORMATION& region) noexcept;
  void fail() noexcept { failed_ = true; }

 private:
  ScopedLocalMemoryQueryCache* previous_ = nullptr;
  std::array<MEMORY_BASIC_INFORMATION, kRegionLimit> regions_{};
  std::size_t count_ = 0;
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
// identity proof. query clips to that bound and one VirtualQuery region;
// read can span readable regions but requires exact MEM_IMAGE allocation
// identity on every part. Logical PE-section restrictions belong to the caller.
class LocalImageReader final : public discovery::ImageReader {
 public:
  LocalImageReader(HMODULE main_module, std::uint32_t image_size) noexcept;
  discovery::ReadWindow query(std::uint32_t rva, std::uint32_t maximum) noexcept override;
  bool read(std::uint32_t rva, void* destination, std::size_t size) noexcept override;

 private:
  HMODULE module_ = nullptr;
  std::uintptr_t base_ = 0;
  std::uint32_t limit_ = 0;
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
