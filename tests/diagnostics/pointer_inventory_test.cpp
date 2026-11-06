#include "../../tools/diagnostics/pointer_inventory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
using namespace taxi_camera::discovery;
static_assert(kMaximumPointerTableReadBytes == 264 && kPrecedingWordIndex == -1);

constexpr std::uint64_t Base = 0x180000000;
constexpr std::uint32_t Table = 0x2100;

struct SyntheticReader final : ImageReader {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x5000);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> holes;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> reads;
  std::uint32_t maximum_window = 64;
  std::uint32_t fail_rva = std::numeric_limits<std::uint32_t>::max();
  bool stalled = false;
  bool oversized_window = false;

  ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    if (stalled || rva >= bytes.size()) {
      return {};
    }
    if (oversized_window) {
      return {maximum + 1, true};
    }
    auto size = std::min({maximum, maximum_window, static_cast<std::uint32_t>(bytes.size()) - rva});
    for (const auto& hole : holes) {
      if (rva >= hole.first && rva < hole.second) {
        return {std::min(size, hole.second - rva), false};
      }
      if (rva < hole.first) {
        size = std::min(size, hole.first - rva);
      }
    }
    return {size, true};
  }

  bool read(std::uint32_t rva, void* destination, std::size_t size) override {
    reads.emplace_back(rva, static_cast<std::uint32_t>(size));
    if (rva > bytes.size() || size > bytes.size() - rva) {
      return false;
    }
    if (rva == fail_rva) {
      // A failed adapter read can partially populate the buffer; none of these
      // partial bytes may become a classified output entry.
      std::memset(destination, 0xff, size / 2);
      return false;
    }
    for (const auto& hole : holes) {
      if (rva < hole.second && std::uint64_t(rva) + size > hole.first) {
        return false;
      }
    }
    std::memcpy(destination, bytes.data() + rva, size);
    return true;
  }

  void word(std::uint32_t rva, std::uint64_t value) {
    for (std::uint32_t index = 0; index < 8; ++index) {
      bytes.at(rva + index) = static_cast<std::uint8_t>(value >> (index * 8));
    }
  }
};

Inventory metadata() {
  Inventory image;
  image.valid_image = true;
  image.machine = 0x8664;
  image.image_size = 0x5000;
  image.sections = {{".text", 0x1000, 0x800, 0x60000020}, {".rdata", 0x2000, 0x1000, 0x40000040}, {".data", 0x3000, 0x800, 0xc0000040},
                    {".wx", 0x3800, 0x200, 0xe0000020},   {".xonly", 0x3a00, 0x200, 0x20000020},  {".discard", 0x3c00, 0x200, 0x42000040}};
  image.section_count = static_cast<std::uint16_t>(image.sections.size());
  return image;
}

SyntheticReader fixture() {
  SyntheticReader reader;
  reader.word(Table - 8, Base + 0x2500);
  reader.word(Table, Base + 0x1100);
  reader.word(Table + 8, Base + 0x1200);
  reader.word(Table + 16, Base + 0x2508);
  reader.word(Table + 24, Base + 0x1300);
  return reader;
}

std::uint32_t checks = 0;

void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void check_budget(const SyntheticReader& reader, const PointerInventory& result) {
  std::uint32_t bytes = 0;
  for (const auto& read : reader.reads) {
    require(read.second == 8, "Reader received a non-word request");
    require(read.first >= 0x2000 && std::uint64_t(read.first) + read.second <= 0x3000, "Target code or non-static data was read");
    bytes += read.second;
  }
  require(bytes == result.read_bytes && bytes <= 264, "Exact read accounting or hard byte cap failed");
}

void classifications() {
  const auto image = metadata();
  auto reader = fixture();
  const auto result = inspect_pointer_table(reader, image, Base, Table);
  require(
      result.valid_table && result.error.empty() && result.entries.size() == 3 && result.stopped_on_non_code && !result.entry_limit_reached,
      "Code run did not stop at first data pointer");
  require(result.preceding_available && result.preceding.index == -1 && result.preceding.source_rva == Table - 8 &&
              result.preceding.target_rva == 0x2500 && result.preceding.category == PointerCategory::image_data,
          "Preceding metadata normalization failed");
  require(result.entries[0].index == 0 && result.entries[0].source_rva == Table && result.entries[0].target_rva == 0x1100 &&
              result.entries[0].category == PointerCategory::image_code && result.entries[1].index == 1 &&
              result.entries[1].target_rva == 0x1200 && result.entries[2].category == PointerCategory::image_data,
          "Pointer indices/RVAs/classification failed");
  check_budget(reader, result);

  const std::pair<std::uint64_t, PointerCategory> cases[] = {
      {0, PointerCategory::null_pointer},
      {Base - 1, PointerCategory::external},
      {Base + image.image_size, PointerCategory::external},
      {std::numeric_limits<std::uint64_t>::max(), PointerCategory::external},
      {Base, PointerCategory::image_data},
      {Base + image.image_size - 1, PointerCategory::image_data},
      {Base + 0x1800, PointerCategory::image_data},  // First byte past .text.
      {Base + 0x1900, PointerCategory::image_data},  // Section gap.
      {Base + 0x3100, PointerCategory::image_data},  // Writable data.
      {Base + 0x3800, PointerCategory::image_data},  // Writable executable storage.
      {Base + 0x3a00, PointerCategory::image_data},  // Execute-only section.
      {Base + 0x3c00, PointerCategory::image_data},  // Discardable data.
  };
  for (const auto& test : cases) {
    reader = fixture();
    reader.word(Table, test.first);
    const auto classified = inspect_pointer_table(reader, image, Base, Table, 24, false);
    require(classified.entries.size() == 1 && classified.entries[0].category == test.second && classified.stopped_on_non_code,
            "Non-code termination/classification failed");
    require(!classified.preceding_available, "Disabled preceding-word inspection occurred");
    const bool in_image = test.second == PointerCategory::image_data;
    require(classified.entries[0].target_rva == (in_image ? static_cast<std::uint32_t>(test.first - Base) : 0),
            "External/null pointer value escaped into metadata");
    check_budget(reader, classified);
  }
  for (const auto category :
       {PointerCategory::null_pointer, PointerCategory::external, PointerCategory::image_data, PointerCategory::image_code}) {
    require(std::strcmp(pointer_category_name(category), "unknown") != 0, "Category serialization name missing");
  }
}

void read_failures() {
  const auto image = metadata();
  auto reader = fixture();
  reader.maximum_window = 1;
  auto result = inspect_pointer_table(reader, image, Base, Table);
  require(result.entries.size() == 3 && result.read_failures == 0, "Readable word across small query windows failed");
  check_budget(reader, result);

  reader = fixture();
  reader.holes = {{Table + 11, Table + 12}};
  result = inspect_pointer_table(reader, image, Base, Table);
  require(result.entries.size() == 1 && result.read_failures == 1 && !result.error.empty() && result.read_bytes == 16,
          "Partially inaccessible table word was read");
  check_budget(reader, result);

  reader = fixture();
  reader.fail_rva = Table + 8;
  result = inspect_pointer_table(reader, image, Base, Table);
  require(result.entries.size() == 1 && result.read_failures == 1 && result.read_bytes == 24 && !result.error.empty(),
          "Failed exact read leaked partial pointer bytes");
  check_budget(reader, result);

  reader = fixture();
  reader.holes = {{Table - 8, Table}};
  result = inspect_pointer_table(reader, image, Base, Table);
  require(!result.preceding_available && result.entries.size() == 3 && result.read_failures == 1,
          "Unavailable optional preceding word prevented valid table inspection");
  check_budget(reader, result);

  reader = fixture();
  reader.holes = {{0x1100, 0x1101}};
  result = inspect_pointer_table(reader, image, Base, Table);
  require(result.entries.size() == 1 && result.entries[0].category == PointerCategory::image_data && result.read_failures == 0,
          "Unreadable executable destination classified as code");
  check_budget(reader, result);

  for (const bool oversized : {false, true}) {
    reader = fixture();
    reader.stalled = !oversized;
    reader.oversized_window = oversized;
    result = inspect_pointer_table(reader, image, Base, Table, 24, false);
    require(result.entries.empty() && result.read_failures == 1 && reader.reads.empty() && !result.error.empty(),
            "Malformed query window did not fail closed");
  }
}

void bounds() {
  const auto image = metadata();
  for (const auto rva : {0u, 0x1100u, 0x1fffu, 0x2ff9u, 0x3100u, 0x3800u, 0x3a00u, 0x3c00u, 0x5000u, 0xfffffff8u}) {
    auto reader = fixture();
    const auto result = inspect_pointer_table(reader, image, Base, rva);
    require(!result.valid_table && !result.error.empty() && result.entries.empty() && reader.reads.empty(),
            "Invalid/executable/writable/overflowing table location was read");
  }

  auto reader = fixture();
  reader.word(0x2ff8, Base + 0x1100);
  auto result = inspect_pointer_table(reader, image, Base, 0x2ff8, 24, false);
  require(result.valid_table && result.entries.size() == 1 && !result.error.empty() && !result.entry_limit_reached,
          "Table scan crossed its containing static section");
  check_budget(reader, result);

  reader = fixture();
  reader.word(0x2000, 0);
  result = inspect_pointer_table(reader, image, Base, 0x2000);
  require(!result.preceding_available && result.entries.size() == 1 && result.read_bytes == 8,
          "Preceding word crossed the static section start");

  for (unsigned int test = 0; test < 6; ++test) {
    auto invalid = image;
    if (test == 0)
      invalid.valid_image = false;
    if (test == 1)
      invalid.machine = 0x14c;
    if (test == 2)
      invalid.image_size = 0;
    if (test == 3)
      invalid.sections[1].size = 0xffffffff;
    if (test == 4)
      invalid.sections[1].rva = 0x1100;
    if (test == 5)
      invalid.sections.clear();
    reader = fixture();
    result = inspect_pointer_table(reader, invalid, Base, Table);
    require(!result.valid_table && !result.error.empty() && reader.reads.empty(), "Malformed image metadata was accepted");
  }
  reader = fixture();
  result = inspect_pointer_table(reader, image, std::numeric_limits<std::uint64_t>::max() - image.image_size + 1, Table);
  require(!result.valid_table && reader.reads.empty(), "Image-base addition overflow was accepted");

  reader = fixture();
  const auto high_base = std::numeric_limits<std::uint64_t>::max() - image.image_size;
  reader.word(Table, high_base + 0x1100);
  result = inspect_pointer_table(reader, image, high_base, Table, 1, false);
  require(result.valid_table && result.entries.size() == 1 && result.entries[0].target_rva == 0x1100,
          "Valid high-base normalization failed");
}

void caps() {
  const auto image = metadata();
  for (const auto cap : {1u, 24u, 32u, 33u, 0xffffffffu}) {
    auto reader = fixture();
    for (std::uint32_t index = 0; index < 40; ++index) {
      reader.word(Table + index * 8, Base + 0x1100 + index);
    }
    const auto result = inspect_pointer_table(reader, image, Base, Table, cap);
    require(result.entries.size() == std::min(cap, 32u) && result.entry_limit_reached && !result.stopped_on_non_code,
            "Entry limit was not clamped or scan exceeded it");
    require(result.read_bytes == (std::min(cap, 32u) + 1) * 8, "Entry cap read accounting failed");
    check_budget(reader, result);
  }
  auto reader = fixture();
  auto result = inspect_pointer_table(reader, image, Base, Table, 0);
  require(!result.valid_table && result.entries.empty() && reader.reads.empty(), "Zero cap still performed reads");
  for (std::uint32_t index = 0; index < 40; ++index) {
    reader.word(Table + index * 8, Base + 0x1100);
  }
  result = inspect_pointer_table(reader, image, Base, Table);
  require(result.entries.size() == 24 && result.read_bytes == 200 && result.entry_limit_reached, "Default entry limit changed");
}

}  // namespace

int main() {
  try {
    classifications();
    read_failures();
    bounds();
    caps();
    std::printf("PASS: %u bounded static pointer-table checks; metadata only, no target bytes or absolute addresses returned.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Pointer inventory test failed: %s\n", error.what());
    return 1;
  }
}
