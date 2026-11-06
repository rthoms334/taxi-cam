#include "../../tools/diagnostics/reference_inventory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {
using namespace taxi_camera::discovery;

constexpr std::uint32_t kText = 0x1000;
constexpr std::uint32_t kTarget = 0x1100;
constexpr std::uint32_t kLiteral = 0x300;
constexpr std::uint32_t kTable = 0x400;
constexpr std::uint32_t kChunk = 32 * 1024;

void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct Patch {
  std::uint32_t rva;
  std::vector<std::uint8_t> bytes;
};

// Sparse synthetic loaded image: even the 128-MiB scan-limit case allocates
// only small patches. No process API, image file or real instruction decoder.
struct SyntheticReader final : ImageReader {
  std::uint32_t image_size = 0x20000;
  std::vector<Patch> patches;
  std::uint32_t function_read_rva = 0;
  std::vector<Patch> function_read_patches;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> holes;
  std::uint64_t read_calls = 0;
  std::uint64_t furthest_code_read = 0;
  std::uint32_t largest_code_read = 0;

  ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    if (rva >= image_size)
      return {};
    return {std::min(maximum, image_size - rva), true};
  }

  bool read(std::uint32_t rva, void* destination, std::size_t size) override {
    ++read_calls;
    if (rva > image_size || size > image_size - rva)
      return false;
    const auto end = static_cast<std::uint64_t>(rva) + size;
    for (const auto& hole : holes) {
      if (rva < hole.second && end > hole.first)
        return false;
    }
    if (rva >= kText) {
      furthest_code_read = std::max(furthest_code_read, end);
      largest_code_read = std::max(largest_code_read, static_cast<std::uint32_t>(size));
    }
    auto* output = static_cast<std::uint8_t*>(destination);
    std::fill_n(output, size, 0x90);
    for (const auto* applied : {&patches, &function_read_patches}) {
      if (applied == &function_read_patches && rva != function_read_rva)
        continue;
      for (const auto& patch : *applied) {
        const auto first = std::max<std::uint64_t>(rva, patch.rva);
        const auto last = std::min(end, patch.rva + patch.bytes.size());
        if (first < last)
          std::memcpy(output + first - rva, patch.bytes.data() + first - patch.rva, last - first);
      }
    }
    return true;
  }

  void bytes(std::uint32_t rva, std::vector<std::uint8_t> value) { patches.push_back({rva, std::move(value)}); }

  void dword(std::uint32_t rva, std::uint32_t value) {
    bytes(rva, {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value >> 16),
                static_cast<std::uint8_t>(value >> 24)});
  }

  void call(std::uint32_t rva, std::uint32_t target) {
    bytes(rva, {0xe8});
    dword(rva + 1, target - (rva + 5));
  }
};

struct Fixture {
  SyntheticReader reader;
  Inventory image;
  std::vector<LiteralTarget> literals{{"build identity", kLiteral, "CameraToTexture fixture"}};
  std::vector<CodeTarget> targets{{"explicit leaf", kTarget}};

  explicit Fixture(std::uint32_t text_size = 0x10000) {
    image.valid_image = true;
    image.machine = 0x8664;
    image.image_size = kText + text_size;
    reader.image_size = image.image_size;
    image.sections = {{".rdata", 0x200, 0xe00, 0x40000040}, {".text", kText, text_size, 0x60000020}};
    image.exception_rva = kTable;
    auto text = std::vector<std::uint8_t>(literals[0].expected_text.begin(), literals[0].expected_text.end());
    text.push_back(0);
    reader.bytes(kLiteral, std::move(text));
  }

  void function(std::uint32_t begin, std::uint32_t end) {
    const auto at = image.exception_rva + image.exception_size;
    reader.dword(at, begin);
    reader.dword(at + 4, end);
    reader.dword(at + 8, 0x280);
    image.exception_size += 12;
  }
};

class SyntheticDecoder final : public InstructionDecoder {
 public:
  bool wrong_call_size = false;

  std::size_t decode(std::uint8_t* bytes, std::size_t available, std::uint32_t, std::string& instruction) override {
    if (available == 0)
      return 0;
    if (bytes[0] == 0x90 || bytes[0] == 0xc3) {
      instruction = bytes[0] == 0x90 ? "nop" : "ret";
      return 1;
    }
    if (bytes[0] == 0xe8 && available >= (wrong_call_size ? 7u : 5u)) {
      instruction = "call rel32";
      return wrong_call_size ? 7 : 5;
    }
    if (available >= 10 && bytes[0] == 0x48 && bytes[1] == 0xb8) {
      instruction = "movabs rax, imm64";
      return 10;
    }
    if (available >= 7 && bytes[0] == 0x48 && bytes[1] == 0x8d) {
      instruction = "lea rcx, [rip+rel32]";
      return 7;
    }
    return 0;
  }
};

ReferenceInventory scan(Fixture& fixture, SyntheticDecoder& decoder) {
  return find_direct_call_references(fixture.reader, fixture.image, decoder, fixture.literals, fixture.targets);
}

void references_and_boundaries() {
  SyntheticDecoder decoder;
  Fixture fixture;
  fixture.function(0x1200, 0x1207);
  fixture.reader.call(0x1201, kTarget);
  fixture.reader.bytes(0x1206, {0xc3});
  fixture.function(0x1300, 0x1306);
  fixture.reader.call(0x1300, 0x1500);
  fixture.reader.bytes(0x1305, {0xc3});
  fixture.reader.call(0x1400, 0x1600);  // Explicitly unrequested destination.
  fixture.targets.push_back({"forward leaf", 0x1500});
  auto result = scan(fixture, decoder);
  require(result.valid_targets && result.error.empty() && result.references.size() == 2 && result.candidate_count == 2,
          "Signed rel32 matching, multiple targets or exact target filtering failed");
  const auto& first = result.references[0];
  require(first.target_label == "explicit leaf" && first.target_rva == kTarget && first.instruction_rva == 0x1201 &&
              first.function_begin_rva == 0x1200 && first.function_end_rva == 0x1207 && first.context.size() == 3 &&
              first.context[1].direct_call_target_rva == kTarget,
          "A direct call's function bounds, label or context target metadata was lost");
  require(result.references[1].target_label == "forward leaf" && result.references[1].instruction_rva == 0x1300,
          "A positive displacement to a leaf without .pdata was rejected");

  Fixture immediate;
  immediate.function(0x1200, 0x120b);
  immediate.reader.bytes(0x1200, {0x48, 0xb8});
  immediate.reader.call(0x1202, kTarget);  // E8 bytes are part of movabs's immediate.
  immediate.reader.bytes(0x120a, {0xc3});
  result = scan(immediate, decoder);
  require(result.candidate_count == 1 && result.failed_boundary_checks == 1 && result.references.empty(),
          "An E8 sequence inside an immediate was accepted as a call instruction");

  Fixture changed_after_scan;
  changed_after_scan.function(0x1200, 0x1207);
  changed_after_scan.reader.call(0x1201, kTarget);
  changed_after_scan.reader.bytes(0x1206, {0xc3});
  changed_after_scan.reader.function_read_rva = 0x1200;
  changed_after_scan.reader.function_read_patches = {{0x1202, {0x10}}};
  result = scan(changed_after_scan, decoder);
  require(result.candidate_count == 1 && result.failed_boundary_checks == 1 && result.references.empty(),
          "A call whose bytes changed after the scan read was accepted from the stale scan bytes");

  Fixture wrong_size;
  wrong_size.function(0x1200, 0x1210);
  wrong_size.reader.call(0x1200, kTarget);
  decoder.wrong_call_size = true;
  result = scan(wrong_size, decoder);
  require(result.references.empty() && result.failed_boundary_checks == 1, "A call decoded with a size other than five was accepted");
  decoder.wrong_call_size = false;

  Fixture crossing;
  const auto call = kText + kChunk - 2;
  crossing.function(call - 3, call + 6);
  crossing.reader.call(call, kTarget);
  crossing.reader.bytes(call + 5, {0xc3});
  result = scan(crossing, decoder);
  require(result.references.size() == 1 && result.references[0].instruction_rva == call && result.candidate_count == 1,
          "A call crossing the scan chunk boundary was missed or duplicated");

  Fixture truncated;
  truncated.function(0x1200, 0x1203);
  truncated.reader.call(0x1200, kTarget);
  result = scan(truncated, decoder);
  require(result.references.empty() && result.failed_boundary_checks == 1,
          "A call extending beyond its runtime-function range was accepted");

  Fixture lea;
  lea.function(0x1200, 0x1208);
  lea.reader.bytes(0x1200, {0x48, 0x8d, 0x0d});
  lea.reader.dword(0x1203, kLiteral - 0x1207);
  lea.reader.bytes(0x1207, {0xc3});
  result = find_literal_references(lea.reader, lea.image, decoder, lea.literals);
  require(result.references.size() == 1 && result.references[0].instruction_rva == 0x1200,
          "Sharing the scanner broke the existing seven-byte LEA reference path");
}

void rejected_inputs_and_reads() {
  SyntheticDecoder decoder;
  for (const auto target : {0u, kLiteral, 0x11000u, 0xffffffffu}) {
    Fixture fixture;
    fixture.function(0x1200, 0x1206);
    fixture.targets[0].rva = target;
    const auto result = scan(fixture, decoder);
    require(!result.valid_targets && !result.error.empty() && result.code_bytes == 0,
            "A zero, non-executable or out-of-image target was accepted");
  }
  for (const auto flags : {0xe0000020u, 0x62000020u, 0x20000020u}) {
    Fixture fixture;
    fixture.function(0x1200, 0x1206);
    fixture.image.sections[1].flags = flags;
    require(!scan(fixture, decoder).valid_targets, "A writable, discardable or unreadable code target was accepted");
  }
  for (const auto count : {0u, 9u}) {
    Fixture fixture;
    fixture.targets.resize(count, {"excess", kTarget});
    const auto result = scan(fixture, decoder);
    require(!result.valid_targets && result.code_bytes == 0 && fixture.reader.read_calls == 0,
            "An empty or oversized target list caused memory reads");
  }
  Fixture duplicate;
  duplicate.targets.push_back(duplicate.targets[0]);
  require(!scan(duplicate, decoder).valid_targets, "Duplicate targets were accepted");
  Fixture stale;
  stale.reader.bytes(kLiteral, {'X'});
  require(!scan(stale, decoder).valid_targets, "A stale build literal was accepted");
  Fixture invalid;
  invalid.image.valid_image = false;
  require(!scan(invalid, decoder).valid_targets && invalid.reader.read_calls == 0, "An invalid image was read");

  Fixture missing_table;
  auto result = scan(missing_table, decoder);
  require(result.valid_targets && !result.error.empty() && result.code_bytes == 0, "Missing .pdata was accepted");
  Fixture bad_table;
  bad_table.function(0x1200, 0x1206);
  bad_table.function(0x1204, 0x1210);
  result = scan(bad_table, decoder);
  require(!result.error.empty() && result.code_bytes == 0, "Overlapping runtime-function bounds were accepted");
  Fixture unreadable_table;
  unreadable_table.function(0x1200, 0x1206);
  unreadable_table.reader.holes = {{kTable, kTable + 12}};
  result = scan(unreadable_table, decoder);
  require(result.read_failures == 1 && !result.error.empty() && result.code_bytes == 0, "Unreadable .pdata was not reported");
  Fixture unbounded;
  unbounded.function(0x1200, 0x1201);
  unbounded.reader.call(0x1300, kTarget);
  result = scan(unbounded, decoder);
  require(result.references.empty() && result.missing_function_bounds == 1 && result.decoded_bytes == 0,
          "A candidate without runtime-function bounds was decoded");
  Fixture oversized(0x20000);
  oversized.function(0x1200, 0x1200 + 65537);
  oversized.reader.call(0x1200, kTarget);
  result = scan(oversized, decoder);
  require(result.references.empty() && result.failed_boundary_checks == 1 && result.decoded_bytes == 0 &&
              oversized.reader.largest_code_read <= kChunk + 4,
          "The 64-KiB per-function boundary-validation limit was exceeded");
  Fixture unreadable_code;
  unreadable_code.function(0x1200, 0x1206);
  unreadable_code.reader.call(0x1200, kTarget);
  unreadable_code.reader.holes = {{0x1200, 0x1206}};
  result = scan(unreadable_code, decoder);
  require(result.read_failures == 1 && result.references.empty(), "A failed code scan read was not reported");
}

void hard_limits() {
  SyntheticDecoder decoder;
  Fixture references;
  references.function(0x1200, 0x1200 + 33 * 5);
  for (std::uint32_t i = 0; i < 33; ++i)
    references.reader.call(0x1200 + i * 5, kTarget);
  auto result = scan(references, decoder);
  require(result.reference_limit_reached && result.references.size() == 32 && result.candidate_count == 32,
          "The 32-reference limit was exceeded or not reported");

  Fixture candidates;
  candidates.function(kText, kText + 1);
  for (std::uint32_t i = 0; i < 1025; ++i)
    candidates.reader.call(0x1200 + i * 5, kTarget);
  result = scan(candidates, decoder);
  require(result.reference_limit_reached && result.candidate_count == 1024 && result.missing_function_bounds == 1024 &&
              result.references.empty() && result.decoded_bytes == 0,
          "The 1024-candidate limit was exceeded or not reported");

  constexpr std::uint32_t code_limit = 128 * 1024 * 1024;
  Fixture code(code_limit + 1);
  code.function(kText, kText + 1);
  result = scan(code, decoder);
  require(result.code_limit_reached && result.code_bytes == code_limit && result.decoded_bytes == 0 &&
              code.reader.furthest_code_read <= static_cast<std::uint64_t>(kText) + code_limit + 4,
          "The 128-MiB scan limit or bounded call-overlap read failed");

  constexpr std::uint32_t function_size = 64 * 1024;
  Fixture decode(129 * function_size);
  for (std::uint32_t i = 0; i < 129; ++i) {
    const auto begin = kText + i * function_size;
    const auto end = begin + function_size;
    decode.function(begin, end);
    decode.reader.bytes(end - 10, {0x48, 0xb8});
    decode.reader.call(end - 8, kTarget);  // False positive after a long decoded prefix.
  }
  result = scan(decode, decoder);
  require(result.decode_limit_reached && result.decoded_bytes == 8 * 1024 * 1024 && result.references.empty() &&
              result.candidate_count == 129 && result.failed_boundary_checks == 129,
          "The aggregate eight-MiB boundary-decoding limit was exceeded or not reported");

  // Leave only four decode-budget bytes at a real five-byte call. The decoder
  // must not receive a fifth byte and the incomplete call must not be emitted.
  Fixture partial_instruction(129 * function_size);
  for (std::uint32_t i = 0; i < 128; ++i) {
    const auto begin = kText + i * (function_size - 1);
    const auto end = begin + function_size - 1;
    partial_instruction.function(begin, end);
    partial_instruction.reader.bytes(end - 10, {0x48, 0xb8});
    partial_instruction.reader.call(end - 8, kTarget);
  }
  const auto last = kText + 128 * (function_size - 1);
  partial_instruction.function(last, last + 130);
  partial_instruction.reader.call(last + 124, kTarget);
  partial_instruction.reader.bytes(last + 129, {0xc3});
  result = scan(partial_instruction, decoder);
  require(result.decode_limit_reached && result.decoded_bytes == 8 * 1024 * 1024 - 4 && result.references.empty() &&
              result.candidate_count == 129,
          "A multi-byte instruction exceeded the remaining aggregate decode budget");
}
}  // namespace

int main() {
  try {
    references_and_boundaries();
    rejected_inputs_and_reads();
    hard_limits();
    std::puts("PASS: direct-call targets, decoded boundaries, immediate false positives, chunk overlap, refused reads and hard limits.");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
