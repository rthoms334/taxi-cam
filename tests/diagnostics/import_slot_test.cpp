#include "../../tools/diagnostics/import_slot.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
using namespace taxi_camera::discovery;

constexpr std::uint32_t Optional = 0x98;
constexpr std::uint32_t Directory = 0x2100;
constexpr std::uint32_t Lookup = 0x3000;
constexpr std::uint32_t Module = 0x4000;
constexpr std::uint32_t Name = 0x4200;
constexpr std::uint32_t Iat = 0x10000;
constexpr std::uint32_t IatSize = 0x10000;

struct SyntheticReader final : ImageReader {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x20000);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> reads;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> queries;
  std::uint32_t maximum_window = 512;
  std::uint32_t fail_rva = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t hole_rva = std::numeric_limits<std::uint32_t>::max();
  bool oversized = false;
  bool stalled = false;

  ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    queries.emplace_back(rva, maximum);
    if (stalled || rva >= bytes.size()) {
      return {};
    }
    if (oversized) {
      return {maximum + 1, true};
    }
    const auto extent = std::min({maximum, maximum_window, static_cast<std::uint32_t>(bytes.size()) - rva});
    if (rva == hole_rva) {
      return {extent, false};
    }
    return {hole_rva > rva ? std::min(extent, hole_rva - rva) : extent, true};
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

  void word(std::uint32_t rva, std::uint64_t value, unsigned int size = 4) {
    for (unsigned int index = 0; index < size; ++index) {
      bytes.at(rva + index) = static_cast<std::uint8_t>(value >> (index * 8));
    }
  }

  void literal(std::uint32_t rva, const std::string& value) {
    std::copy(value.begin(), value.end(), bytes.begin() + rva);
    bytes.at(rva + value.size()) = 0;
  }

  void descriptor(std::uint32_t rva, std::uint32_t lookup, std::uint32_t first = Iat, std::uint32_t module = Module) {
    word(rva, lookup);
    word(rva + 4, 0);
    word(rva + 8, 0);
    word(rva + 12, module);
    word(rva + 16, first);
  }
};

Inventory metadata() {
  Inventory image;
  image.valid_image = true;
  image.machine = 0x8664;
  image.section_count = 3;
  image.timestamp = 123456;
  image.image_size = 0x20000;
  image.checksum = 789;
  image.sections = {{".text", 0x1000, 0x1000, 0x60000020}, {".idata", 0x2000, 0xe000, 0x40000040}, {".iat", Iat, IatSize, 0xc0000040}};
  return image;
}

SyntheticReader fixture() {
  const auto image = metadata();
  SyntheticReader reader;
  reader.word(0, 0x5a4d, 2);
  reader.word(60, 0x80);
  reader.word(0x80, 0x4550);
  reader.word(0x84, image.machine, 2);
  reader.word(0x86, image.section_count, 2);
  reader.word(0x88, image.timestamp);
  reader.word(0x94, 240, 2);
  reader.word(Optional, 0x20b, 2);
  reader.word(Optional + 56, image.image_size);
  reader.word(Optional + 60, 0x400);
  reader.word(Optional + 64, image.checksum);
  reader.word(Optional + 108, 16);
  reader.word(Optional + 120, Directory);
  reader.word(Optional + 124, 40);
  reader.word(Optional + 208, Iat);
  reader.word(Optional + 212, IatSize);
  reader.descriptor(Directory, Lookup);
  reader.word(Lookup, Name, 8);
  reader.literal(Module, "VCRUNTIME140.dll");
  reader.word(Name, 123, 2);
  reader.literal(Name + 2, "__RTDynamicCast");
  // These resolved values must never enter a read buffer.
  for (std::uint32_t offset = 0; offset < IatSize; offset += 8) {
    reader.word(Iat + offset, 0x7fff012345678900ull + offset, 8);
  }
  return reader;
}

std::uint32_t checks = 0;

void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void budget(const SyntheticReader& reader, const ImportSlotInventory& result, bool exact_accounting = true) {
  std::uint32_t bytes = 0;
  for (const auto& read : reader.reads) {
    require(read.second > 0 && read.first < reader.bytes.size() && read.second <= reader.bytes.size() - read.first,
            "Read escaped the loaded image");
    require(read.first + read.second <= Iat || read.first >= Iat + IatSize, "Resolved IAT contents were read");
    require(std::uint64_t(read.first) + read.second <= result.iat_slot_rva || read.first >= std::uint64_t(result.iat_slot_rva) + 8,
            "The explicit requested slot was read through a metadata alias");
    bytes += read.second;
  }
  require(bytes <= result.read_bytes && result.read_bytes <= 131072, "Attempted-read accounting or hard byte cap failed");
  require(!exact_accounting || bytes == result.read_bytes, "Successful query/read accounting differed");
  require(result.descriptors_read <= 256 && result.thunks_read <= 256 * 2048, "Descriptor or thunk cap exceeded");
  for (const auto& query : reader.queries) {
    require(std::uint64_t(query.first) + query.second <= result.iat_slot_rva || query.first >= std::uint64_t(result.iat_slot_rva) + 8,
            "The explicit requested slot reached query through a metadata alias");
  }
}

void named_import() {
  const auto image = metadata();
  auto reader = fixture();
  const auto result = inspect_import_slot(reader, image, Iat);
  require(result.valid && result.available && result.status == "named_import" && result.error.empty(), "Named import was not found");
  require(result.module == "VCRUNTIME140.dll" && result.symbol == "__RTDynamicCast", "Module or symbol mismatch");
  require(result.descriptor_rva == Directory && result.lookup_thunk_rva == Lookup && result.iat_slot_rva == Iat &&
              result.hint_name_rva == Name && !result.by_ordinal && result.ordinal == 0,
          "Normalized metadata mismatch");
  budget(reader, result);

  for (const auto window : {1u, 3u, 7u, 16u}) {
    reader = fixture();
    reader.maximum_window = window;
    const auto split = inspect_import_slot(reader, image, Iat);
    require(split.valid && split.available && split.symbol == result.symbol, "Readable fragmented windows failed");
    budget(reader, split);
  }

  reader = fixture();
  reader.word(Lookup, 0x8000000000000002ull, 8);
  reader.word(Lookup + 8, Name, 8);
  const auto second = inspect_import_slot(reader, image, Iat + 8);
  require(second.available && second.thunks_read == 2 && second.lookup_thunk_rva == Lookup + 8, "Exact slot index was not respected");
  budget(reader, second);

  reader = fixture();
  reader.word(Optional + 124, 60);
  reader.descriptor(Directory + 20, Lookup + 16, Iat + 16);
  reader.word(Lookup + 16, Name, 8);
  const auto next = inspect_import_slot(reader, image, Iat + 16);
  require(next.available && next.descriptor_rva == Directory + 20 && next.descriptors_read == 2 && next.thunks_read == 3,
          "A preceding descriptor's terminating thunk did not bound its slots");
  budget(reader, next);
}

bool any_read_overlaps(const SyntheticReader& reader, std::uint32_t begin, std::uint32_t size) {
  return std::any_of(reader.reads.begin(), reader.reads.end(), [&](const auto& read) {
    return std::uint64_t(read.first) < std::uint64_t(begin) + size && begin < std::uint64_t(read.first) + read.second;
  });
}

void only_the_requested_name_is_read() {
  const auto image = metadata();
  auto reader = fixture();
  auto result = inspect_import_slot(reader, image, Iat);
  require(result.available && !any_read_overlaps(reader, Name, kImportNameHintBytes), "The import-name hint was read");

  constexpr std::uint32_t unrelated_name = Name + 0x80;
  reader = fixture();
  reader.word(Lookup, unrelated_name, 8);
  reader.word(unrelated_name, 7, 2);
  reader.literal(unrelated_name + 2, "UnrelatedImport");
  reader.word(Lookup + 8, Name, 8);
  result = inspect_import_slot(reader, image, Iat + 8);
  require(
      result.available && result.symbol == "__RTDynamicCast" && result.thunks_read == 2 && !any_read_overlaps(reader, unrelated_name, 0x20),
      "A preceding lookup entry's hint/name string was read");
  budget(reader, result);
}

void unavailable_cases() {
  const auto image = metadata();
  auto reader = fixture();
  reader.word(Lookup, 0x8000000000000123ull, 8);
  auto result = inspect_import_slot(reader, image, Iat);
  require(result.valid && !result.available && result.status == "ordinal_only" && result.by_ordinal && result.ordinal == 0x123 &&
              result.module == "VCRUNTIME140.dll" && result.symbol.empty() && result.hint_name_rva == 0,
          "Ordinal import was mistaken for a name");
  budget(reader, result);

  reader = fixture();
  reader.word(Directory, 0);
  result = inspect_import_slot(reader, image, Iat);
  require(result.valid && !result.available && result.status == "lookup_table_absent" && result.thunks_read == 0,
          "Missing OFT was not explicitly unavailable");
  budget(reader, result);

  reader = fixture();
  reader.word(Optional + 208, 0);
  reader.word(Optional + 212, 0);
  result = inspect_import_slot(reader, image, Iat);
  require(result.valid && !result.available && result.status == "iat_extent_unavailable" && result.descriptors_read == 0,
          "Missing IAT extent was not refused before table reads");
  budget(reader, result);

  reader = fixture();
  reader.word(Optional + 108, 2);
  result = inspect_import_slot(reader, image, Iat);
  require(result.valid && result.status == "iat_extent_unavailable", "Absent IAT data-directory entry was not handled");
  budget(reader, result);

  reader = fixture();
  reader.word(Optional + 120, 0);
  reader.word(Optional + 124, 0);
  result = inspect_import_slot(reader, image, Iat);
  require(result.valid && !result.available && result.status == "no_import_directory", "No imports was not explicit");
  budget(reader, result);

  for (const auto slot : {Iat - 8, Iat + 1, Iat + 8}) {
    reader = fixture();
    result = inspect_import_slot(reader, image, slot);
    require(result.valid && !result.available && result.symbol.empty(), "Absent or unaligned slot produced a symbol");
    budget(reader, result);
  }

  reader = fixture();
  result = inspect_import_slot(reader, image, Iat + 2048 * 8);
  require(result.valid && !result.available && result.status == "thunk_limit_reached" && result.thunks_read == 0,
          "Out-of-cap slot did not remain unavailable");
  budget(reader, result);
}

void malformed_headers() {
  const auto image = metadata();
  const std::pair<std::uint32_t, std::uint64_t> edits[] = {
      {0, 0},
      {60, 0xffffffff},
      {60, Iat},
      {60, 0x1000},
      {60, 0x1000 - 20},
      {60, 0},
      {0x80, 0},
      {0x84, 0x14c},
      {0x86, 2},
      {0x88, 654321},
      {0x94, 111},
      {0x94, 4097},
      {Optional, 0x10b},
      {Optional + 56, 0xffffffff},
      {Optional + 60, 0},
      {Optional + 60, 0x1ff},
      {Optional + 60, 0x2001},
      {Optional + 64, 0},
      {Optional + 108, 17},
      {Optional + 120, 0xffffffff},
      {Optional + 124, 19},
      {Optional + 124, 0xffffffff},
      {Optional + 120, Iat},
      {Optional + 208, 0xffffffff},
      {Optional + 212, 7},
      {Optional + 212, 0xffffffff},
      {Optional + 208, 0x1000},
  };
  for (const auto& edit : edits) {
    auto reader = fixture();
    const bool half = edit.first == 0 || edit.first == 0x84 || edit.first == 0x86 || edit.first == 0x94 || edit.first == Optional;
    reader.word(edit.first, edit.second, half ? 2 : 4);
    const auto result = inspect_import_slot(reader, image, Iat);
    require(!result.valid && !result.available && !result.error.empty(), "Malformed PE header was accepted");
    budget(reader, result);
  }
  for (const auto slot : {0xfffffffcu, image.image_size - 7, image.image_size}) {
    auto reader = fixture();
    const auto result = inspect_import_slot(reader, image, slot);
    require(!result.valid && reader.reads.empty(), "Invalid requested slot was read");
    budget(reader, result);
  }
  for (unsigned int variant = 0; variant < 6; ++variant) {
    auto bad = image;
    switch (variant) {
      case 0:
        bad.valid_image = false;
        break;
      case 1:
        bad.machine = 0x14c;
        break;
      case 2:
        bad.sections[1].rva = 0x1800;
        break;
      case 3:
        bad.sections[1].size = 0xffffffff;
        break;
      case 4:
        bad.sections.clear();
        break;
      case 5:
        bad.image_size = 0xffffffff;
        break;
    }
    auto reader = fixture();
    const auto result = inspect_import_slot(reader, bad, Iat);
    require(!result.valid && reader.reads.empty(), "Invalid input inventory caused reads");
    budget(reader, result);
  }
}

void malformed_tables() {
  const auto image = metadata();
  const std::pair<std::uint32_t, std::uint64_t> edits[] = {
      {Directory, Iat},
      {Directory, Iat - 4},
      {Directory, 0xfffffffcu},
      {Directory, 0x1000},
      {Directory + 12, 0},
      {Directory + 12, Iat},
      {Directory + 12, 0x1000},
      {Directory + 16, Iat + 1},
      {Directory + 16, 0},
      {Directory + 16, 0x80},
      {Lookup, 0x80000000ull},
      {Lookup, 0x8000000000010000ull},
      {Lookup, 0x7fff123456789abcull},
      {Lookup, 0x7fffffff},
      {Lookup, Iat},
      {Lookup, 0x1000},
      {Lookup, std::uint64_t(Iat - 1)},
      {Lookup, 0xfffffff8u},
  };
  for (const auto& edit : edits) {
    auto reader = fixture();
    reader.word(edit.first, edit.second, edit.first == Lookup ? 8 : 4);
    const auto result = inspect_import_slot(reader, image, Iat);
    require(!result.valid && !result.available && !result.error.empty(), "Malformed import metadata was accepted");
    budget(reader, result);
  }
  // A zero lookup thunk is a sentinel, not a pointer to an empty name.
  auto reader = fixture();
  reader.word(Lookup, 0, 8);
  auto result = inspect_import_slot(reader, image, Iat);
  require(result.valid && result.status == "slot_not_found" && result.module.empty(), "Null lookup sentinel was followed");
  budget(reader, result);

  // A missing directory terminator is malformed when no matching slot exists.
  reader = fixture();
  reader.word(Optional + 124, 20);
  result = inspect_import_slot(reader, image, Iat + 8);
  require(!result.valid && result.descriptors_read == 1, "Truncated directory without sentinel was accepted");
  budget(reader, result);
}

void strings_and_sections() {
  const auto image = metadata();
  for (const auto rva : {Module, Name + 2}) {
    for (const auto byte : {0u, 10u, 31u, 127u, 255u}) {
      auto reader = fixture();
      reader.word(rva, byte, 1);
      const auto result = inspect_import_slot(reader, image, Iat);
      require(!result.valid && !result.available && (rva == Module ? result.module.empty() : result.symbol.empty()),
              "Empty or invalid ASCII name was emitted");
      budget(reader, result);
    }
    for (const auto length : {127u, 128u}) {
      auto reader = fixture();
      reader.literal(rva, std::string(length, 'a'));
      const auto result = inspect_import_slot(reader, image, Iat);
      require(result.available == (length == 127), "Terminated 128-byte name limit was not enforced");
      budget(reader, result);
    }
  }

  auto reader = fixture();
  auto split = image;
  split.sections[1].size = Name + 8 - split.sections[1].rva;
  split.sections.push_back({".next", Name + 8, Iat - (Name + 8), 0x40000040});
  split.section_count = 4;
  reader.word(0x86, split.section_count, 2);
  const auto boundary = inspect_import_slot(reader, split, Iat);
  require(!boundary.valid && boundary.symbol.empty(), "A name crossed into another declared section");
  budget(reader, boundary);

  for (const auto flags : {0xc0000040u, 0x42000040u, 0xc2000040u, 0x60000040u, 0x00000040u}) {
    reader = fixture();
    auto changed = image;
    changed.sections[1].flags = flags;
    const auto result = inspect_import_slot(reader, changed, Iat);
    const bool allowed = (flags & 0x40000000u) && !(flags & 0x20000000u);
    require(result.available == allowed, "Import data section permission policy differed");
    budget(reader, result);
  }
}

void read_failures() {
  const auto image = metadata();
  for (const auto rva : {0u, 0x80u, Optional, Optional + 120, Optional + 208, Directory, Lookup, Module, Name + 2, Name + 5}) {
    for (const auto hole : {false, true}) {
      auto reader = fixture();
      (hole ? reader.hole_rva : reader.fail_rva) = rva;
      const auto result = inspect_import_slot(reader, image, Iat);
      require(!result.valid && !result.available && result.read_failures == 1 && result.symbol.empty(),
              "Failed metadata read emitted a symbol or lost the failure");
      budget(reader, result, !hole);
    }
  }
  for (const auto oversized : {false, true}) {
    auto reader = fixture();
    reader.oversized = oversized;
    reader.stalled = !oversized;
    const auto result = inspect_import_slot(reader, image, Iat);
    require(!result.valid && result.read_failures == 1 && reader.reads.empty(), "Invalid query window reached read()");
    budget(reader, result, false);
  }
}

void requested_header_aliases() {
  const auto image = metadata();
  const std::pair<std::uint32_t, std::uint32_t> header_reads[] = {
      {0, 64}, {0x80, 24}, {Optional, 112}, {Optional + 120, 8}, {Optional + 208, 8}};
  for (const auto& field : header_reads) {
    // Include a seven-byte overlap before each field and every byte of the
    // field itself, including its last byte. No query may overlap the slot.
    const auto first = field.first >= 7 ? field.first - 7 : 0;
    for (auto slot = first; slot < field.first + field.second; ++slot) {
      auto reader = fixture();
      const auto result = inspect_import_slot(reader, image, slot);
      require(!result.valid && !result.available && result.status == "invalid_metadata" && result.read_failures == 0 &&
                  result.error == "A metadata request overlaps the excluded requested IAT slot.",
              "A requested slot alias in a PE header was not rejected before query/read");
      budget(reader, result);
    }
  }
}

void hard_caps() {
  const auto image = metadata();
  auto reader = fixture();
  reader.word(Optional + 124, 257 * 20);
  for (std::uint32_t index = 0; index < 257; ++index) {
    reader.descriptor(Directory + index * 20, Lookup, Iat + 8);
  }
  auto result = inspect_import_slot(reader, image, Iat);
  require(result.valid && !result.available && result.status == "descriptor_limit_reached" && result.descriptors_read == 256 &&
              result.thunks_read == 0,
          "Descriptor hard cap was exceeded");
  budget(reader, result);

  // A real slot beyond the old cap remains inspectable, including the last
  // allowed descriptor. One beyond the new cap must never be inspected.
  for (const auto descriptor_index : {64u, 255u, 256u}) {
    reader = fixture();
    reader.word(Optional + 124, 258 * 20);
    for (std::uint32_t index = 0; index < descriptor_index; ++index) {
      reader.descriptor(Directory + index * 20, 0x8000, Iat + 8);
    }
    reader.descriptor(Directory + descriptor_index * 20, 0x8000);
    reader.word(0x8000, Name, 8);
    result = inspect_import_slot(reader, image, Iat);
    require(result.available == (descriptor_index < 256) && result.descriptors_read == std::min(descriptor_index + 1, 256u),
            "Extended descriptor boundary lookup failed");
    require(descriptor_index >= 256 || result.descriptor_rva == Directory + descriptor_index * 20,
            "Extended descriptor selected the wrong table entry");
    budget(reader, result);
  }

  reader = fixture();
  constexpr std::uint32_t long_lookup = 0x5000;
  for (std::uint32_t index = 0; index < 2048; ++index) {
    reader.word(long_lookup + index * 8, Name, 8);
  }
  reader.descriptor(Directory, long_lookup);
  result = inspect_import_slot(reader, image, Iat + 2047 * 8);
  require(result.available && result.thunks_read == 2048 && result.lookup_thunk_rva == long_lookup + 2047 * 8,
          "The last permitted thunk could not be identified");
  budget(reader, result);

  reader = fixture();
  reader.word(Optional + 124, 10 * 20);
  for (std::uint32_t index = 0; index < 2047; ++index) {
    reader.word(long_lookup + index * 8, Name, 8);
  }
  for (std::uint32_t index = 0; index < 9; ++index) {
    reader.descriptor(Directory + index * 20, long_lookup);
  }
  result = inspect_import_slot(reader, image, Iat + 2047 * 8);
  require(!result.valid && !result.available && result.status == "byte_limit_reached" && result.read_bytes > 131064 &&
              result.read_bytes <= 131072 && result.descriptors_read <= 8,
          "Aggregate metadata byte limit did not stop repeated lookup tables");
  budget(reader, result);
}

}  // namespace

int main() {
  try {
    named_import();
    only_the_requested_name_is_read();
    unavailable_cases();
    malformed_headers();
    malformed_tables();
    strings_and_sections();
    read_failures();
    requested_header_aliases();
    hard_caps();
    std::printf("PASS: %u exact import-slot metadata checks; no IAT pointer reads.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
