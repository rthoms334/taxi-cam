#include "../../tools/diagnostics/static_literals.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

namespace {
using namespace taxi_camera::discovery;

struct SyntheticReader final : ImageReader {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x5000);
  std::vector<std::uint32_t> reads;
  std::uint32_t queries = 0;
  std::uint32_t unreadable = 0xffffffff;
  std::uint32_t failed_read = 0xffffffff;
  bool stalled = false;
  bool oversized = false;

  ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    ++queries;
    if (stalled || rva >= bytes.size())
      return {};
    if (oversized)
      return {maximum + 1, true};
    return {maximum, rva != unreadable};
  }

  bool read(std::uint32_t rva, void* output, std::size_t size) override {
    if (size != 1)
      throw std::runtime_error("Reader must stop byte-exactly at NUL");
    reads.push_back(rva);
    if (rva >= bytes.size() || rva == unreadable)
      return false;
    if (rva == failed_read) {
      *static_cast<std::uint8_t*>(output) = 'X';
      return false;
    }
    *static_cast<std::uint8_t*>(output) = bytes[rva];
    return true;
  }

  void text(std::uint32_t rva, const std::string& value) {
    std::copy(value.begin(), value.end(), bytes.begin() + rva);
    bytes.at(rva + value.size()) = 0;
  }
};

Inventory metadata() {
  Inventory image;
  image.valid_image = true;
  image.machine = 0x8664;
  image.image_size = 0x5000;
  image.sections = {{".rdata", 0x1000, 0x1000, 0x40000040},
                    {".text", 0x2000, 0x800, 0x60000020},
                    {".data", 0x3000, 0x800, 0xc0000040},
                    {".discard", 0x4000, 0x400, 0x42000040}};
  return image;
}

std::uint32_t checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

void accounting(const SyntheticReader& reader, const StaticLiteralInventory& result) {
  require(result.read_bytes == reader.reads.size() && result.read_bytes <= 1024, "Aggregate read-attempt accounting failed");
  std::uint32_t total = 0;
  for (const auto& record : result.records) {
    require(record.bytes <= 128, "Per-record byte cap exceeded");
    require(record.text.size() <= 127, "Returned string exceeds the terminated-byte limit");
    require(record.error.empty() || record.text.empty(), "A failed literal exposed its unterminated prefix");
    total += record.bytes;
  }
  require(total == result.read_bytes, "Per-record byte accounting differs from aggregate attempts");
  for (const auto rva : reader.reads) {
    require(rva >= 0x1000 && rva < 0x2000, "A read escaped the declared static-data section");
  }
}

void normal_and_partial() {
  const auto image = metadata();
  SyntheticReader reader;
  reader.text(0x1100, "Camera Setup");
  reader.text(0x1200, " ~");
  reader.bytes[0x1300] = 0;
  auto result = inspect_static_literals(reader, image, {0x1100, 0x1200, 0x1300});
  require(result.valid && result.error.empty() && result.read_failures == 0 && result.records.size() == 3, "Valid literals rejected");
  require(result.records[0].rva == 0x1100 && result.records[0].text == "Camera Setup" && result.records[0].bytes == 13 &&
              result.records[1].text == " ~" && result.records[2].text.empty() && result.records[2].bytes == 1,
          "Printable ASCII, empty literal or source RVA incorrect");
  require(std::find(reader.reads.begin(), reader.reads.end(), 0x110d) == reader.reads.end(), "Read continued past the terminating NUL");
  accounting(reader, result);

  reader.reads.clear();
  result = inspect_static_literals(reader, image, {0x1100, 0xffffffff, 0x1200});
  require(!result.valid && !result.error.empty() && result.records.size() == 3 && result.records[0].text == "Camera Setup" &&
              !result.records[1].error.empty() && result.records[1].bytes == 0 && result.records[2].text == " ~",
          "One failed request erased successful peers or prevented later requests");
  accounting(reader, result);
}

void section_bounds() {
  const auto image = metadata();
  for (const auto rva : {0u, 0xfffu, 0x2000u, 0x2800u, 0x3000u, 0x4000u, 0x5000u, 0xffffffffu}) {
    SyntheticReader reader;
    const auto result = inspect_static_literals(reader, image, {rva});
    require(!result.valid && result.records.size() == 1 && !result.records[0].error.empty() && reader.reads.empty() && reader.queries == 0,
            "Invalid/executable/writable/discardable/outside source was accessed");
  }
  SyntheticReader reader;
  auto result = inspect_static_literals(reader, image, {0x1fff});
  require(result.valid && result.records[0].bytes == 1, "NUL at last static-section byte rejected");
  reader.bytes[0x1fff] = 'A';
  reader.reads.clear();
  result = inspect_static_literals(reader, image, {0x1fff});
  require(!result.valid && result.records[0].bytes == 1 && result.records[0].text.empty(), "Unterminated string crossed section end");
  accounting(reader, result);

  for (std::uint32_t test = 0; test < 6; ++test) {
    auto invalid = image;
    if (test == 0)
      invalid.valid_image = false;
    if (test == 1)
      invalid.machine = 0x14c;
    if (test == 2)
      invalid.image_size = 0;
    if (test == 3)
      invalid.sections[0].size = 0xffffffff;
    if (test == 4)
      invalid.sections[1].rva = 0x1100;
    if (test == 5)
      invalid.sections.clear();
    SyntheticReader source;
    result = inspect_static_literals(source, invalid, {0x1100});
    require(!result.valid && !result.error.empty() && result.records.empty() && source.queries == 0, "Malformed image metadata accepted");
  }
  auto unreadable_section = image;
  unreadable_section.sections[0].flags = 0;
  SyntheticReader source;
  result = inspect_static_literals(source, unreadable_section, {0x1100});
  require(!result.valid && source.queries == 0, "Non-readable declared section was queried");
}

void failure_cases() {
  const auto image = metadata();
  for (const auto byte : {1u, 9u, 10u, 13u, 31u, 127u, 128u, 255u}) {
    SyntheticReader reader;
    reader.text(0x1100, "abcdef");
    reader.bytes[0x1103] = static_cast<std::uint8_t>(byte);
    const auto result = inspect_static_literals(reader, image, {0x1100});
    require(!result.valid && result.records[0].text.empty() && result.records[0].bytes == 4 && result.read_failures == 0,
            "Control or non-ASCII input was returned as a string");
    accounting(reader, result);
  }
  for (std::uint32_t test = 0; test < 4; ++test) {
    SyntheticReader reader;
    reader.text(0x1100, "abcdef");
    if (test == 0)
      reader.unreadable = 0x1103;
    if (test == 1)
      reader.failed_read = 0x1103;
    if (test == 2)
      reader.stalled = true;
    if (test == 3)
      reader.oversized = true;
    const auto result = inspect_static_literals(reader, image, {0x1100});
    require(!result.valid && result.read_failures == 1 && result.records[0].text.empty(), "Query/read failure did not fail closed");
    require(result.read_bytes == (test == 0 ? 3u : test == 1 ? 4u : 0u), "Failed read-attempt accounting incorrect");
    accounting(reader, result);
  }
}

void limits() {
  const auto image = metadata();
  for (const auto count : {0u, 9u, 100u}) {
    SyntheticReader reader;
    const auto result = inspect_static_literals(reader, image, std::vector<std::uint32_t>(count, 0x1100));
    require(!result.valid && result.records.empty() && reader.queries == 0 && !result.error.empty(),
            "Invalid request batch accessed memory");
  }
  for (const bool terminated : {false, true}) {
    SyntheticReader reader;
    reader.text(0x1100, std::string(terminated ? 127 : 128, 'A'));
    const auto result = inspect_static_literals(reader, image, std::vector<std::uint32_t>(8, 0x1100));
    require(result.valid == terminated && result.records.size() == 8 && result.read_bytes == 1024,
            "Per-request termination or aggregate byte cap failed");
    require(result.records[0].text.size() == (terminated ? 127u : 0u), "Maximum-sized string handling failed");
    accounting(reader, result);
    require(std::find(reader.reads.begin(), reader.reads.end(), 0x1180) == reader.reads.end(), "Attempted a 129th byte");
  }
}

}  // namespace

int main() {
  try {
    normal_and_partial();
    section_bounds();
    failure_cases();
    limits();
    std::printf("PASS: %u bounded literal checks, with writable-data refusal.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Static literal test failed: %s\n", error.what());
    return 1;
  }
}
