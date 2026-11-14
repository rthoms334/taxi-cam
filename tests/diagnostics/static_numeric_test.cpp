#include "../../tools/diagnostics/static_numeric.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
using namespace taxi_camera::discovery;
static_assert(kMaximumNumericReadBytes == 256 && kLargestNumericBatchBytes == 192);

struct SyntheticReader final : ImageReader {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x5000);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> reads;
  std::uint32_t fail_rva = 0xffffffff;
  std::uint32_t hole_rva = 0xffffffff;
  std::uint32_t maximum_window = 24;
  bool oversized = false;
  bool stalled = false;

  ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    if (stalled || rva >= bytes.size()) {
      return {};
    }
    if (oversized) {
      return {maximum + 1, true};
    }
    auto size = std::min({maximum, maximum_window, static_cast<std::uint32_t>(bytes.size()) - rva});
    if (rva == hole_rva) {
      return {size, false};
    }
    if (rva < hole_rva) {
      size = std::min(size, hole_rva - rva);
    }
    return {size, true};
  }

  bool read(std::uint32_t rva, void* destination, std::size_t size) override {
    reads.emplace_back(rva, static_cast<std::uint32_t>(size));
    if (rva > bytes.size() || size > bytes.size() - rva) {
      return false;
    }
    if (rva == fail_rva) {
      std::memset(destination, 0xff, size / 2);
      return false;
    }
    std::memcpy(destination, bytes.data() + rva, size);
    return true;
  }

  void word(std::uint32_t rva, std::uint64_t value, std::uint32_t width) {
    for (std::uint32_t index = 0; index < width; ++index) {
      bytes.at(rva + index) = static_cast<std::uint8_t>(value >> (index * 8));
    }
  }
};

Inventory metadata() {
  Inventory image;
  image.valid_image = true;
  image.machine = 0x8664;
  image.image_size = 0x5000;
  image.sections = {{".text", 0x1000, 0x1000, 0x60000020},
                    {".rdata", 0x2000, 0x1000, 0x40000040},
                    {".data", 0x3000, 0x1000, 0xc0000040},
                    {".discard", 0x4000, 0x1000, 0x42000040}};
  image.section_count = static_cast<std::uint16_t>(image.sections.size());
  return image;
}

SyntheticReader fixture() {
  SyntheticReader reader;
  reader.word(0x2100, 0x3ff0000000000000ull, 8);
  reader.word(0x2108, 0xc004000000000000ull, 8);
  reader.word(0x2110, 0x3ff0000000000001ull, 8);
  reader.word(0x2200, 0x3f800000, 4);
  reader.word(0x2204, 0xc0200000, 4);
  reader.word(0x2208, 0x3f800001, 4);
  return reader;
}

std::uint32_t checks = 0;

void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void budget(const SyntheticReader& reader, const StaticNumericInventory& result) {
  std::uint32_t bytes = 0;
  for (const auto& read : reader.reads) {
    require(read.first >= 0x2000 && std::uint64_t(read.first) + read.second <= 0x3000 && read.second != 0 && read.second <= 24,
            "A read escaped the permitted numeric section or size");
    bytes += read.second;
  }
  std::uint32_t record_bytes = 0;
  for (const auto& record : result.records) {
    require(record.bytes <= 24, "A per-request allowance was exceeded");
    require(record.error.empty() || record.values.empty(), "Failed request emitted partial numeric values");
    record_bytes += record.bytes;
  }
  require(bytes <= result.read_bytes && record_bytes == result.read_bytes && result.read_bytes <= 192 && result.read_bytes <= 256,
          "Attempted-byte accounting or cap failed");
}

void numeric_values() {
  const auto image = metadata();
  for (const auto window : {1u, 3u, 7u, 24u}) {
    auto reader = fixture();
    reader.maximum_window = window;
    const auto result =
        inspect_static_numeric(reader, image, {{0x2100, StaticNumericKind::float64, 3}, {0x2200, StaticNumericKind::float32, 3}});
    require(result.valid && result.error.empty() && result.read_failures == 0 && result.read_bytes == 36 && result.records.size() == 2,
            "Finite numeric batch failed");
    const auto& doubles = result.records[0];
    const auto& floats = result.records[1];
    require(doubles.rva == 0x2100 && doubles.kind == StaticNumericKind::float64 && doubles.count == 3 && doubles.bytes == 24 &&
                doubles.values.size() == 3 && doubles.values[0] == 1.0 && doubles.values[1] == -2.5 &&
                std::bit_cast<std::uint64_t>(doubles.values[2]) == 0x3ff0000000000001ull,
            "Double precision or little-endian scalar order was lost");
    require(floats.bytes == 12 && floats.values.size() == 3 && floats.values[0] == 1.0 && floats.values[1] == -2.5 &&
                floats.values[2] == static_cast<double>(std::bit_cast<float>(0x3f800001u)),
            "Float32 was not converted exactly to double");
    budget(reader, result);
  }

  const std::uint64_t finite_doubles[] = {
      0, 0x8000000000000000ull, 1, 0x8000000000000001ull, 0x0010000000000000ull, 0x7fefffffffffffffull, 0xffefffffffffffffull};
  for (const auto bits : finite_doubles) {
    auto reader = fixture();
    reader.word(0x2100, bits, 8);
    const auto result = inspect_static_numeric(reader, image, {{0x2100, StaticNumericKind::float64, 1}});
    require(result.valid && std::bit_cast<std::uint64_t>(result.records[0].values[0]) == bits,
            "Finite double, subnormal, signed zero or extreme lost precision");
    budget(reader, result);
  }
  const std::uint32_t finite_floats[] = {0, 0x80000000u, 1, 0x80000001u, 0x00800000u, 0x7f7fffffu, 0xff7fffffu};
  for (const auto bits : finite_floats) {
    auto reader = fixture();
    reader.word(0x2200, bits, 4);
    const auto result = inspect_static_numeric(reader, image, {{0x2200, StaticNumericKind::float32, 1}});
    const auto expected = static_cast<double>(std::bit_cast<float>(bits));
    require(result.valid && result.records[0].values[0] == expected && std::signbit(result.records[0].values[0]) == std::signbit(expected),
            "Finite float, subnormal, signed zero or extreme changed");
    budget(reader, result);
  }
}

void malformed_requests() {
  const auto image = metadata();
  for (const auto count : {0u, 4u, 0xffffffffu}) {
    auto reader = fixture();
    const auto result = inspect_static_numeric(reader, image, {{0x2100, StaticNumericKind::float64, count}});
    require(!result.valid && result.records.size() == 1 && result.records[0].values.empty() && reader.reads.empty(),
            "Malformed scalar count caused a read");
    budget(reader, result);
  }
  for (const auto count : {0u, 9u, 100u}) {
    auto reader = fixture();
    const std::vector<StaticNumericRequest> requests(count, {0x2100, StaticNumericKind::float64, 3});
    const auto result = inspect_static_numeric(reader, image, requests);
    require(!result.valid && result.records.empty() && reader.reads.empty(), "Malformed batch count caused a read");
    budget(reader, result);
  }
  auto reader = fixture();
  auto result = inspect_static_numeric(reader, image, {{0x2100, static_cast<StaticNumericKind>(99), 1}});
  require(!result.valid && result.records.size() == 1 && reader.reads.empty(), "Invalid numeric kind caused a read");
  budget(reader, result);

  reader = fixture();
  result = inspect_static_numeric(reader, image, std::vector<StaticNumericRequest>(8, {0x2100, StaticNumericKind::float64, 3}));
  require(result.valid && result.records.size() == 8 && result.read_bytes == 192, "Maximum permitted batch failed");
  budget(reader, result);

  reader = fixture();
  result = inspect_static_numeric(
      reader, image,
      {{0x2100, StaticNumericKind::float64, 1}, {0x2200, StaticNumericKind::float32, 4}, {0x2200, StaticNumericKind::float32, 1}});
  require(!result.valid && result.records.size() == 3 && result.records[0].values.size() == 1 && result.records[1].values.empty() &&
              result.records[2].values.size() == 1 && result.read_bytes == 12,
          "Invalid peer discarded successful requests or consumed a read");
  budget(reader, result);
}

void section_bounds() {
  const auto image = metadata();
  for (const auto rva : {0u, 0x1000u, 0x1ffcu, 0x2ff9u, 0x3000u, 0x4000u, 0x4ffcu, 0x5000u, 0xfffffffcu}) {
    auto reader = fixture();
    const auto result = inspect_static_numeric(reader, image, {{rva, StaticNumericKind::float64, 1}});
    require(!result.valid && reader.reads.empty() && result.read_failures == 0, "Invalid section range caused a read");
    budget(reader, result);
  }
  auto reader = fixture();
  auto result = inspect_static_numeric(reader, image, {{0x2ff8, StaticNumericKind::float64, 1}});
  require(result.valid && result.records[0].values[0] == 0.0 && result.read_bytes == 8, "Exact section boundary was refused");
  budget(reader, result);

  reader = fixture();
  auto split = image;
  split.sections[1].size = 0x108;
  split.sections.push_back({".other", 0x2108, 0xef8, 0x40000040});
  result = inspect_static_numeric(reader, split, {{0x2100, StaticNumericKind::float64, 2}});
  require(!result.valid && reader.reads.empty(), "One numeric request crossed two individually readable sections");
  budget(reader, result);

  for (const auto flags : {0x00000040u, 0x60000040u, 0xc0000040u, 0x42000040u}) {
    reader = fixture();
    auto changed = image;
    changed.sections[1].flags = flags;
    result = inspect_static_numeric(reader, changed, {{0x2100, StaticNumericKind::float64, 1}});
    require(!result.valid && reader.reads.empty(), "Excluded section characteristics were accepted");
    budget(reader, result);
  }
  for (unsigned int variant = 0; variant < 6; ++variant) {
    reader = fixture();
    auto changed = image;
    switch (variant) {
      case 0:
        changed.valid_image = false;
        break;
      case 1:
        changed.machine = 0x14c;
        break;
      case 2:
        changed.sections.clear();
        break;
      case 3:
        changed.sections[1].size = 0xffffffff;
        break;
      case 4:
        changed.sections[1].rva = 0x1fff;
        break;
      case 5:
        changed.image_size = 0;
        break;
    }
    result = inspect_static_numeric(reader, changed, {{0x2100, StaticNumericKind::float64, 1}});
    require(!result.valid && result.records.empty() && reader.reads.empty(), "Invalid image metadata caused a read");
    budget(reader, result);
  }
}

void non_finite() {
  const auto image = metadata();
  const std::uint64_t bad_doubles[] = {0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull, 0x7ff0000000000001ull};
  for (const auto bits : bad_doubles) {
    for (std::uint32_t index = 0; index < 3; ++index) {
      auto reader = fixture();
      reader.word(0x2100 + index * 8, bits, 8);
      const auto result =
          inspect_static_numeric(reader, image, {{0x2100, StaticNumericKind::float64, 3}, {0x2200, StaticNumericKind::float32, 1}});
      require(!result.valid && result.read_failures == 0 && result.records[0].values.empty() && result.records[1].values.size() == 1 &&
                  result.records[0].error == "A requested scalar is non-finite.",
              "Non-finite double emitted partial values or lost a good peer");
      budget(reader, result);
    }
  }
  const std::uint32_t bad_floats[] = {0x7f800000u, 0xff800000u, 0x7fc00000u, 0x7f800001u};
  for (const auto bits : bad_floats) {
    auto reader = fixture();
    reader.word(0x2204, bits, 4);
    const auto result = inspect_static_numeric(reader, image, {{0x2200, StaticNumericKind::float32, 3}});
    require(!result.valid && result.records[0].values.empty() && result.read_failures == 0, "Non-finite float32 was accepted");
    budget(reader, result);
  }
}

void inaccessible_bytes() {
  const auto image = metadata();
  for (std::uint32_t index = 0; index < 24; ++index) {
    for (const auto hole : {false, true}) {
      auto reader = fixture();
      reader.maximum_window = 1;
      (hole ? reader.hole_rva : reader.fail_rva) = 0x2100 + index;
      const auto result =
          inspect_static_numeric(reader, image, {{0x2100, StaticNumericKind::float64, 3}, {0x2200, StaticNumericKind::float32, 1}});
      require(!result.valid && result.read_failures == 1 && result.records[0].values.empty() && result.records[1].values.size() == 1 &&
                  result.read_bytes == 28,
              "Failed byte read lost accounting, exposed partial evidence or discarded peers");
      budget(reader, result);
    }
  }
  for (const auto oversized : {false, true}) {
    auto reader = fixture();
    reader.oversized = oversized;
    reader.stalled = !oversized;
    const auto result = inspect_static_numeric(reader, image, {{0x2100, StaticNumericKind::float64, 3}});
    require(!result.valid && reader.reads.empty() && result.read_failures == 1 && result.read_bytes == 24,
            "An invalid query reached a read or lost its attempted allowance");
    budget(reader, result);
  }
}

}  // namespace

int main() {
  try {
    numeric_values();
    malformed_requests();
    section_bounds();
    non_finite();
    inaccessible_bytes();
    std::printf("PASS: %u bounded static numeric checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
