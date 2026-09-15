#include "local_memory.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace taxi_camera::native_camera {
namespace {

constexpr std::uint32_t kMaximumImageSize = 0x80000000;
constexpr std::uint32_t kHeaderExtent = 65536;
constexpr std::uint32_t kHeaderReadBudget = 8192;

thread_local LocalMemoryMetrics* active_metrics = nullptr;
thread_local ScopedLocalMemoryQueryCache* active_query_cache = nullptr;

std::uint64_t performance_tick() noexcept {
  LARGE_INTEGER counter{};
  return QueryPerformanceCounter(&counter) && counter.QuadPart > 0 ? static_cast<std::uint64_t>(counter.QuadPart) : 0;
}

std::uint64_t elapsed_ticks(std::uint64_t start) noexcept {
  const auto end = performance_tick();
  return start != 0 && end >= start ? end - start : 0;
}

SIZE_T query_memory_uncached(const void* address, MEMORY_BASIC_INFORMATION& region) noexcept {
  auto* const metrics = active_metrics;
  const auto start = metrics ? performance_tick() : 0;
  // Use the documented explicit-process entry point with only the current
  // process pseudo-handle. The MBI contract and all caller validation remain
  // unchanged; no handle/PID or lower-level syscall path is configurable here.
  const auto result = VirtualQueryEx(GetCurrentProcess(), address, &region, sizeof(region));
  if (metrics) {
    ++metrics->query_calls;
    metrics->query_ticks += elapsed_ticks(start);
  }
  return result;
}

SIZE_T query_memory(const void* address, MEMORY_BASIC_INFORMATION& region) noexcept {
  return active_query_cache ? active_query_cache->query(address, region) : query_memory_uncached(address, region);
}

bool read_memory(const void* address, void* destination, SIZE_T size, SIZE_T& copied) noexcept {
  auto* const metrics = active_metrics;
  const auto start = metrics ? performance_tick() : 0;
  const bool result = ReadProcessMemory(GetCurrentProcess(), address, destination, size, &copied) != FALSE;
  if (metrics) {
    ++metrics->read_calls;
    metrics->requested_bytes += size;
    metrics->read_ticks += elapsed_ticks(start);
  }
  if ((!result || copied != size) && active_query_cache)
    active_query_cache->fail();
  return result;
}

// https://learn.microsoft.com/en-us/windows/win32/memory/memory-protection-constants
bool readable(DWORD protection) noexcept {
  if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
    return false;
  const auto basic = protection & 0xff;
  return basic == PAGE_READONLY || basic == PAGE_READWRITE || basic == PAGE_WRITECOPY || basic == PAGE_EXECUTE_READ ||
         basic == PAGE_EXECUTE_READWRITE || basic == PAGE_EXECUTE_WRITECOPY;
}

bool region_contains(const MEMORY_BASIC_INFORMATION& region, std::uintptr_t address, std::size_t size) noexcept {
  const auto start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
  return region.RegionSize != 0 && start <= address && region.RegionSize <= std::numeric_limits<std::uintptr_t>::max() - start &&
         address - start < region.RegionSize && size <= region.RegionSize - (address - start);
}

bool private_bytes(std::uintptr_t address, std::uint8_t* temporary, std::size_t size) noexcept {
  std::size_t offset = 0;
  while (offset < size) {
    MEMORY_BASIC_INFORMATION region{};
    const auto current = address + offset;
    if (query_memory(reinterpret_cast<const void*>(current), region) != sizeof(region) || region.State != MEM_COMMIT ||
        region.Type != MEM_PRIVATE || !readable(region.Protect) || !region_contains(region, current, 1))
      return false;
    const auto available = region.RegionSize - (current - reinterpret_cast<std::uintptr_t>(region.BaseAddress));
    const auto chunk = std::min(size - offset, available);
    SIZE_T copied = 0;
    if (!read_memory(reinterpret_cast<const void*>(current), temporary + offset, chunk, copied) || copied != chunk)
      return false;
    offset += chunk;
  }
  return true;
}

// Keep the large temporary off the frequent scalar-read stack frame. No output
// is published if any part fails, and no byte outside the field is requested.
__declspec(noinline) bool large_private_field(std::uintptr_t address, void* destination, std::size_t size) noexcept {
  std::array<std::uint8_t, kLocalObjectFieldLimit> bytes;
  if (!private_bytes(address, bytes.data(), size))
    return false;
  std::memcpy(destination, bytes.data(), size);
  return true;
}

std::uint16_t u16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(bytes[0] | (std::uint16_t(bytes[1]) << 8));
}

std::uint32_t u32(const std::uint8_t* bytes) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index)
    value |= std::uint32_t(bytes[index]) << (index * 8);
  return value;
}

bool within(std::uint32_t start, std::uint32_t size, std::uint32_t bound) {
  return start <= bound && size <= bound - start;
}

class HeaderReader {
 public:
  HeaderReader(discovery::ImageReader& reader, discovery::Inventory& result) : reader_(reader), result_(result) {}
  bool read(std::uint32_t rva, void* destination, std::uint32_t size) {
    if (size == 0 || !within(rva, size, kHeaderExtent) || size > kHeaderReadBudget - result_.metadata_bytes) {
      result_.error = "A header request exceeds the fixed extent or metadata allowance.";
      return false;
    }
    result_.metadata_bytes += size;
    auto* output = static_cast<std::uint8_t*>(destination);
    std::uint32_t offset = 0;
    while (offset < size) {
      const auto window = reader_.query(rva + offset, size - offset);
      if (!window.readable || window.size == 0 || window.size > size - offset ||
          !reader_.read(rva + offset, output + offset, window.size)) {
        ++result_.read_failures;
        result_.error = "A required main-image header field could not be read exactly.";
        return false;
      }
      offset += window.size;
    }
    return true;
  }

 private:
  discovery::ImageReader& reader_;
  discovery::Inventory& result_;
};

bool directory_valid(const discovery::Inventory& image, std::uint32_t rva, std::uint32_t size) {
  if (rva == 0 || size == 0)
    return rva == 0 && size == 0;
  return within(rva, size, image.image_size) && std::any_of(image.sections.begin(), image.sections.end(), [&](const auto& section) {
           return rva >= section.rva && within(rva - section.rva, size, section.size);
         });
}

}  // namespace

ScopedLocalMemoryMetrics::ScopedLocalMemoryMetrics(LocalMemoryMetrics& metrics) noexcept : previous_(active_metrics) {
  active_metrics = &metrics;
}

ScopedLocalMemoryMetrics::~ScopedLocalMemoryMetrics() {
  active_metrics = previous_;
}

ScopedLocalMemoryQueryCache::ScopedLocalMemoryQueryCache() noexcept : previous_(active_query_cache) {
  active_query_cache = this;
}

ScopedLocalMemoryQueryCache::~ScopedLocalMemoryQueryCache() {
  if (active_)
    active_query_cache = previous_;
}

SIZE_T ScopedLocalMemoryQueryCache::query(const void* address, MEMORY_BASIC_INFORMATION& region) noexcept {
  if (!active_ || failed_)
    return 0;
  const auto value = reinterpret_cast<std::uintptr_t>(address);
  for (std::size_t i = 0; i < count_; ++i) {
    if (region_contains(regions_[i], value, 1)) {
      region = regions_[i];
      if (active_metrics)
        ++active_metrics->query_cache_hits;
      return sizeof(region);
    }
  }
  if (count_ == regions_.size()) {
    failed_ = true;
    return 0;
  }
  // VirtualQuery reports only the suffix beginning at its queried page. Probe
  // the containing 64 KiB window first so descending graph fields can share
  // one observation. This queries metadata only: no extra bytes are read. The
  // result is usable only when it contains the actual field; a preceding guard,
  // reservation or protection split falls back to the exact requested address.
  // There is no merging, eviction or reuse beyond this transaction. finish() freshly
  // compares every saved region, including any newly observed prefix pages.
  constexpr std::uintptr_t window_size = 65536;
  const auto window_start = value - value % window_size;
  if (query_memory_uncached(reinterpret_cast<const void*>(window_start), region) != sizeof(region) || !region_contains(region, value, 1)) {
    if (window_start == value || query_memory_uncached(address, region) != sizeof(region) || !region_contains(region, value, 1)) {
      failed_ = true;
      return 0;
    }
  }
  regions_[count_++] = region;
  return sizeof(region);
}

bool ScopedLocalMemoryQueryCache::is_current() const noexcept {
  return active_ && !failed_ && active_query_cache == this;
}

bool ScopedLocalMemoryQueryCache::finish() noexcept {
  if (!active_ || active_query_cache != this)
    return false;
  for (std::size_t i = 0; i < count_ && !failed_; ++i) {
    const auto& initial = regions_[i];
    MEMORY_BASIC_INFORMATION current{};
    if (query_memory_uncached(initial.BaseAddress, current) != sizeof(current) || current.BaseAddress != initial.BaseAddress ||
        current.AllocationBase != initial.AllocationBase || current.AllocationProtect != initial.AllocationProtect ||
        current.RegionSize != initial.RegionSize || current.State != initial.State || current.Protect != initial.Protect ||
        current.Type != initial.Type)
      failed_ = true;
  }
  if (failed_ && active_metrics)
    ++active_metrics->query_cache_validation_failures;
  active_query_cache = previous_;
  active_ = false;
  return !failed_;
}

LocalMemoryReader::LocalMemoryReader(std::uint32_t limit) noexcept {
  reset_budget(limit);
}

void LocalMemoryReader::reset_budget(std::uint32_t limit) noexcept {
  limit_ = std::min(limit, kLocalObjectReadLimit);
  attempted_ = 0;
  failures_ = 0;
}

bool LocalMemoryReader::read(std::uint64_t address, void* destination, std::size_t size) noexcept {
  if (destination == nullptr || address == 0 || size == 0 || size > kLocalObjectFieldLimit ||
      address > std::numeric_limits<std::uintptr_t>::max() - size || size > limit_ - attempted_)
    return false;
  attempted_ += static_cast<std::uint32_t>(size);
  std::array<std::uint8_t, 64> bytes;
  // Region queries remain snapshots; RPM rechecks access instead of directly
  // dereferencing an engine allocation. Large fields use their own temporary.
  const bool success = size > bytes.size() ? large_private_field(static_cast<std::uintptr_t>(address), destination, size)
                                           : private_bytes(static_cast<std::uintptr_t>(address), bytes.data(), size);
  if (!success) {
    ++failures_;
    return false;
  }
  if (size <= bytes.size())
    std::memcpy(destination, bytes.data(), size);
  return true;
}

LocalImageReader::LocalImageReader(HMODULE main_module, std::uint32_t image_size) noexcept {
  const auto base = reinterpret_cast<std::uintptr_t>(main_module);
  if (main_module == nullptr || main_module != GetModuleHandleW(nullptr) || image_size == 0 || image_size > kMaximumImageSize ||
      image_size > std::numeric_limits<std::uintptr_t>::max() - base)
    return;
  module_ = main_module;
  base_ = base;
  limit_ = image_size;
}

discovery::ReadWindow LocalImageReader::query(std::uint32_t rva, std::uint32_t maximum) noexcept {
  if (rva >= limit_ || maximum == 0)
    return {};
  maximum = std::min(maximum, limit_ - rva);
  const auto address = base_ + rva;
  MEMORY_BASIC_INFORMATION region{};
  if (query_memory(reinterpret_cast<const void*>(address), region) != sizeof(region) || !region_contains(region, address, 1))
    return {};
  const auto available = region.RegionSize - (address - reinterpret_cast<std::uintptr_t>(region.BaseAddress));
  const auto size = static_cast<std::uint32_t>(std::min<std::size_t>(maximum, available));
  return {size, region.State == MEM_COMMIT && region.Type == MEM_IMAGE && region.AllocationBase == module_ && readable(region.Protect)};
}

bool LocalImageReader::read(std::uint32_t rva, void* destination, std::size_t size) noexcept {
  if (destination == nullptr || size == 0 || rva >= limit_ || size > limit_ - rva)
    return false;
  auto* output = static_cast<std::uint8_t*>(destination);
  std::uint32_t offset = 0;
  while (offset < size) {
    const auto window = query(rva + offset, static_cast<std::uint32_t>(size - offset));
    if (!window.readable || window.size == 0 || window.size > size - offset)
      return false;
    SIZE_T copied = 0;
    if (!read_memory(reinterpret_cast<const void*>(base_ + rva + offset), output + offset, window.size, copied) || copied != window.size)
      return false;
    offset += window.size;
  }
  return true;
}

discovery::Inventory parse_verified_image_headers(discovery::ImageReader& reader) {
  discovery::Inventory result;
  HeaderReader source(reader, result);
  std::array<std::uint8_t, 64> dos{};
  if (!source.read(0, dos.data(), dos.size()))
    return result;
  const auto pe = u32(dos.data() + 60);
  if (u16(dos.data()) != 0x5a4d || pe < 64 || !within(pe, 24, kHeaderExtent)) {
    result.error = "The DOS signature or PE header offset is invalid.";
    return result;
  }
  std::array<std::uint8_t, 24> coff{};
  if (!source.read(pe, coff.data(), coff.size()))
    return result;
  result.machine = u16(coff.data() + 4);
  result.section_count = u16(coff.data() + 6);
  result.timestamp = u32(coff.data() + 8);
  const auto optional_size = u16(coff.data() + 20);
  if (u32(coff.data()) != 0x4550 || result.machine != 0x8664 || result.section_count == 0 || result.section_count > 96 ||
      optional_size < 112 || optional_size > 4096) {
    result.error = "The main PE header is not bounded AMD64 executable metadata.";
    return result;
  }
  std::array<std::uint8_t, 4096> optional{};
  if (!source.read(pe + 24, optional.data(), optional_size))
    return result;
  result.image_size = u32(optional.data() + 56);
  result.checksum = u32(optional.data() + 64);
  result.dll_characteristics = u16(optional.data() + 70);
  const auto headers_size = u32(optional.data() + 60);
  const auto directory_count = u32(optional.data() + 108);
  const auto section_table = pe + 24 + optional_size;
  const std::uint32_t section_bytes = result.section_count * 40;
  if (u16(optional.data()) != 0x20b || result.image_size == 0 || result.image_size > kMaximumImageSize || headers_size == 0 ||
      headers_size > result.image_size || headers_size > kHeaderExtent || !within(section_table, section_bytes, headers_size) ||
      directory_count > (optional_size - 112u) / 8u) {
    result.error = "The optional header, image extent or section-table bounds are invalid.";
    return result;
  }
  if (directory_count > 3) {
    result.exception_rva = u32(optional.data() + 112 + 3 * 8);
    result.exception_size = u32(optional.data() + 116 + 3 * 8);
  }
  if (directory_count > 10) {
    result.load_config_rva = u32(optional.data() + 112 + 10 * 8);
    result.load_config_size = u32(optional.data() + 116 + 10 * 8);
  }
  std::array<std::uint8_t, 96 * 40> table{};
  if (!source.read(section_table, table.data(), section_bytes))
    return result;
  for (std::uint32_t index = 0; index < result.section_count; ++index) {
    const auto* entry = table.data() + index * 40;
    std::string name;
    for (unsigned character = 0; character < 8 && entry[character] != 0; ++character)
      name.push_back(entry[character] >= 32 && entry[character] <= 126 ? static_cast<char>(entry[character]) : '?');
    const auto virtual_size = u32(entry + 8);
    const auto size = virtual_size != 0 ? virtual_size : u32(entry + 16);
    const auto rva = u32(entry + 12);
    if (!within(rva, size, result.image_size) || (size != 0 && rva < headers_size)) {
      result.error = "A declared loaded section overlaps headers or exceeds the image bounds.";
      return result;
    }
    result.sections.push_back({name, rva, size, u32(entry + 36)});
  }
  std::sort(result.sections.begin(), result.sections.end(), [](const auto& left, const auto& right) { return left.rva < right.rva; });
  if (std::none_of(result.sections.begin(), result.sections.end(), [](const auto& section) { return section.size != 0; })) {
    result.error = "The image has no nonempty loaded sections.";
    return result;
  }
  std::uint32_t previous_end = headers_size;
  for (const auto& section : result.sections) {
    if (section.size == 0)
      continue;
    if (section.rva < previous_end) {
      result.error = "The declared loaded sections overlap.";
      return result;
    }
    previous_end = section.rva + section.size;
  }
  if (!directory_valid(result, result.exception_rva, result.exception_size) ||
      !directory_valid(result, result.load_config_rva, result.load_config_size)) {
    result.error = "An exception or load-config directory is malformed or outside the declared sections.";
    return result;
  }
  result.valid_image = true;
  return result;
}

discovery::Inventory parse_verified_main_image() {
  LocalImageReader reader(GetModuleHandleW(nullptr), kHeaderExtent);
  return parse_verified_image_headers(reader);
}

}  // namespace taxi_camera::native_camera
