#include "../../tools/diagnostics/slot_prefix.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace {
using namespace taxi_camera::discovery;
static_assert(kMaximumSlotTargetReadBytes == 512 && kMaximumSlotProofReadBytes == 64);
constexpr std::uint64_t kBase = 0x140000000;
constexpr std::uint32_t kSlot = 0x3100;
constexpr std::uint32_t kEntry = 0x2000;
std::uint32_t checks = 0;

void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

struct Reader final : ImageReader {
  std::map<std::uint32_t, std::uint8_t> bytes;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> reads;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> queries;
  std::uint32_t fail_rva = 0;
  std::uint32_t hole_rva = 0;
  std::uint32_t query_chunk = 64;
  bool oversized_window = false;

  ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    queries.emplace_back(rva, maximum);
    if (oversized_window)
      return {maximum + 1, true};
    if (hole_rva >= rva && hole_rva - rva < maximum)
      return {maximum, false};
    return {std::min(maximum, query_chunk), true};
  }

  bool read(std::uint32_t rva, void* destination, std::size_t size) override {
    reads.emplace_back(rva, static_cast<std::uint32_t>(size));
    if (rva == fail_rva)
      return false;
    auto* output = static_cast<std::uint8_t*>(destination);
    for (std::uint32_t offset = 0; offset < size; ++offset) {
      const auto found = bytes.find(rva + offset);
      output[offset] = found == bytes.end() ? 0x90 : found->second;
    }
    return true;
  }

  void patch(std::uint32_t rva, const std::vector<std::uint8_t>& data) {
    for (std::uint32_t offset = 0; offset < data.size(); ++offset)
      bytes[rva + offset] = data[offset];
  }

  void pointer(std::uint32_t rva, std::uint64_t value) {
    for (std::uint32_t offset = 0; offset < 8; ++offset)
      bytes[rva + offset] = static_cast<std::uint8_t>(value >> (offset * 8));
  }

  void relative(std::uint32_t rva, std::uint32_t target, std::uint8_t opcode = 0xe8) {
    const auto displacement = target - (rva + 5);
    patch(rva, {opcode, static_cast<std::uint8_t>(displacement), static_cast<std::uint8_t>(displacement >> 8),
                static_cast<std::uint8_t>(displacement >> 16), static_cast<std::uint8_t>(displacement >> 24)});
  }
};

struct Decoder final : InstructionDecoder {
  std::vector<std::uint32_t> calls;
  std::uint32_t fail_rva = 0;
  std::size_t forced_size = 0;
  bool oversized_text = false;
  bool empty_text = false;

  std::size_t decode(std::uint8_t* bytes, std::size_t available, std::uint32_t rva, std::string& instruction) override {
    calls.push_back(rva);
    if (rva == fail_rva || available == 0)
      return 0;
    if (oversized_text) {
      instruction.assign(513, 'x');
      return 1;
    }
    if (empty_text)
      return 1;
    if (forced_size != 0) {
      instruction = "forced";
      return forced_size;
    }
    std::size_t size = 0;
    switch (bytes[0]) {
      case 0x90:
        instruction = "nop";
        size = 1;
        break;
      case 0xc3:
        instruction = "ret";
        size = 1;
        break;
      case 0xc2:
        instruction = "ret imm16";
        size = 3;
        break;
      case 0xb8:
        instruction = "mov eax, imm32";
        size = 5;
        break;
      case 0xe8:
        instruction = "call rel32";
        size = 5;
        break;
      case 0xe9:
        instruction = "jmp rel32";
        size = 5;
        break;
      case 0xeb:
        instruction = "jmp rel8";
        size = 2;
        break;
      case 0x48:
        if (available >= 2 && bytes[1] == 0xb8) {
          instruction = "mov rax, imm64";
          size = 10;
        }
        break;
    }
    return size <= available ? size : 0;
  }
};

struct Fixture {
  Reader reader;
  Decoder decoder;
  Inventory image;
  std::uint64_t base = kBase;
  std::vector<SlotPrefixRequest> requests{{kSlot, kEntry}};

  Fixture() {
    image.valid_image = true;
    image.machine = 0x8664;
    image.image_size = 0x5000;
    image.sections = {{".text", 0x1000, 0x2000, 0x60000020}, {".rdata", 0x3000, 0x1000, 0x40000040}};
    reader.pointer(kSlot, kBase + kEntry);
    reader.patch(kEntry, {0x90, 0xc3, 0xff});
  }

  SlotPrefixInventory run() {
    reader.reads.clear();
    reader.queries.clear();
    decoder.calls.clear();
    const auto result = inspect_slot_prefixes(reader, image, decoder, base, requests);
    require(result.read_bytes <= 512 && result.proof_bytes <= 64 && result.entries.size() <= 8, "Aggregate cap exceeded");
    std::uint32_t target_bytes = 0;
    std::uint32_t proof_bytes = 0;
    for (const auto& read : reader.reads) {
      const auto slot = std::find_if(requests.begin(), requests.end(), [&](const auto& request) { return request.slot_rva == read.first; });
      const auto entry =
          std::find_if(requests.begin(), requests.end(), [&](const auto& request) { return request.entry_rva == read.first; });
      if (slot != requests.end()) {
        require(read.second == 8, "Slot proof was not exactly eight bytes");
        proof_bytes += read.second;
      } else {
        require(
            entry != requests.end() && read.second > 0 && read.second <= 64 && std::uint64_t(read.first) + read.second <= image.image_size,
            "Reader followed an unrequested address or exceeded a target bound");
        target_bytes += read.second;
      }
    }
    for (const auto& query : reader.queries) {
      const auto bounded = std::any_of(requests.begin(), requests.end(), [&](const auto& request) {
        return (query.first >= request.slot_rva && std::uint64_t(query.first) + query.second <= std::uint64_t(request.slot_rva) + 8) ||
               (query.first >= request.entry_rva && std::uint64_t(query.first) + query.second <= std::uint64_t(request.entry_rva) + 64);
      });
      require(bounded, "A query followed an unrequested address or exceeded its bound");
    }
    require(result.read_bytes == target_bytes && result.proof_bytes == proof_bytes, "Attempted read-byte accounting is inaccurate");
    for (const auto& entry : result.entries)
      require(entry.decoded_bytes <= entry.attempted_bytes && entry.attempted_bytes <= 64 && entry.instructions.size() <= 64,
              "Decoded metadata exceeds its bounded read");
    return result;
  }
};

void reject_before_read(Fixture& test) {
  const auto result = test.run();
  require(!result.valid && !result.error.empty() && test.reader.reads.empty() && test.reader.queries.empty(),
          "Invalid static metadata was not refused before queries/reads");
}

void returns_and_boundaries() {
  Fixture test;
  auto result = test.run();
  require(result.valid && result.read_bytes == 64 && result.proof_bytes == 8 && result.read_failures == 0 &&
              result.entries[0].proof_slot_rva == kSlot && result.entries[0].entry_rva == kEntry && result.entries[0].ret_observed &&
              result.entries[0].stop_reason == "ret" && result.entries[0].decoded_bytes == 2 &&
              test.decoder.calls == std::vector<std::uint32_t>{kEntry, kEntry + 1},
          "Aligned RET did not stop decoding independently of the bounded read");
  test.reader.patch(kEntry, {0xb8, 0xc3, 0xc2, 0, 0, 0xc2, 0x18, 0, 0xff});
  result = test.run();
  require(result.valid && result.entries[0].decoded_bytes == 8 && result.entries[0].instructions.size() == 2,
          "RET inside an immediate prematurely stopped decoding or C2 was not recognized");
  test.reader.patch(kEntry, {0x48, 0xb8, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3});
  result = test.run();
  require(result.valid && result.entries[0].decoded_bytes == 11 && result.entries[0].instructions.size() == 2,
          "RET inside a 64-bit immediate stopped decoding");
  test.reader.patch(kEntry, std::vector<std::uint8_t>(64, 0x90));
  result = test.run();
  require(result.valid && !result.entries[0].ret_observed && result.entries[0].stop_reason == "prefix_limit" &&
              result.entries[0].decoded_bytes == 64,
          "A bounded prefix without RET was misrepresented as a complete method or rejected");
  test.image.sections[0].size = kEntry + 7 - 0x1000;
  result = test.run();
  require(result.valid && result.read_bytes == 7 && result.entries[0].stop_reason == "section_end", "Read crossed the executable section");
  test.image.sections[0].size = kEntry + 1 - 0x1000;
  test.reader.patch(kEntry, {0xc3});
  result = test.run();
  require(result.valid && result.read_bytes == 1 && result.entries[0].ret_observed, "Last-byte RET was not safely readable");
  test.reader.patch(kEntry, {0xb8});
  result = test.run();
  require(!result.valid && result.entries[0].stop_reason == "decode_failed" && result.entries[0].decoded_bytes == 0,
          "Incomplete instruction at section end was accepted");
}

void static_refusals() {
  for (const auto flags : {0xc0000040u, 0x60000040u, 0x42000040u, 0x00000040u}) {
    Fixture test;
    test.image.sections[1].flags = flags;
    reject_before_read(test);
  }
  for (const auto flags : {0xe0000020u, 0x62000020u, 0x40000020u, 0x20000020u}) {
    Fixture test;
    test.image.sections[0].flags = flags;
    reject_before_read(test);
  }
  for (const auto slot : {kSlot + 1, 0x2000u, 0x3ffcu, 0x4000u, 0xfffffff8u}) {
    Fixture test;
    test.requests[0].slot_rva = slot;
    reject_before_read(test);
  }
  for (const auto entry : {0u, 0x3000u, 0x5000u, 0xffffffffu}) {
    Fixture test;
    test.requests[0].entry_rva = entry;
    reject_before_read(test);
  }
  Fixture test;
  test.image.sections[1].size = kSlot + 7 - 0x3000;
  reject_before_read(test);
  test = Fixture{};
  test.image.sections[1].rva = kSlot + 1;
  reject_before_read(test);
  for (const auto base : {std::uint64_t(0), kBase + 1, std::numeric_limits<std::uint64_t>::max() - 7}) {
    test = Fixture{};
    test.base = base;
    reject_before_read(test);
  }
  for (int mutation = 0; mutation < 8; ++mutation) {
    test = Fixture{};
    switch (mutation) {
      case 0:
        test.image.valid_image = false;
        break;
      case 1:
        test.image.machine = 0x14c;
        break;
      case 2:
        test.image.image_size = 0;
        break;
      case 3:
        test.image.sections.clear();
        break;
      case 4:
        test.image.sections[1].rva = 0x2000;
        break;
      case 5:
        test.image.sections[1].size = 0xffffffff;
        break;
      case 6:
        test.image.sections[1].rva = 0xffffffff;
        break;
      case 7:
        test.image.sections.resize(97);
        break;
    }
    reject_before_read(test);
  }
}

void stale_proofs_and_read_failures() {
  for (const auto pointer : {std::uint64_t(0), kBase - 8, kBase + 0x5000, kBase + kEntry + 1, std::numeric_limits<std::uint64_t>::max()}) {
    Fixture test;
    test.reader.pointer(kSlot, pointer);
    const auto result = test.run();
    require(!result.valid && result.proof_bytes == 8 && result.read_bytes == 0 && result.entries[0].stop_reason == "proof_mismatch" &&
                test.reader.reads.size() == 1 && test.reader.queries.size() == 1 && test.decoder.calls.empty(),
            "Stale or foreign pointer allowed target access");
  }
  Fixture test;
  test.base += 0x10000;
  auto result = test.run();
  require(!result.valid && result.read_bytes == 0, "A stale image base allowed a target read");
  for (const auto address : {kSlot, kEntry}) {
    test = Fixture{};
    test.reader.fail_rva = address;
    result = test.run();
    require(!result.valid && result.read_failures == 1 && result.proof_bytes == 8 && result.entries[0].stop_reason == "unreadable" &&
                result.read_bytes == (address == kEntry ? 64u : 0u) && test.decoder.calls.empty(),
            "Failed exact read accounting or refusal is incorrect");
    test = Fixture{};
    test.reader.hole_rva = address + (address == kSlot ? 7 : 63);
    result = test.run();
    require(!result.valid && result.read_failures == 1 && result.read_bytes == 0 && result.proof_bytes == (address == kEntry ? 8u : 0u),
            "Unreadable query allowed a read or consumed attempted-read bytes");
  }
  test = Fixture{};
  test.reader.query_chunk = 1;
  result = test.run();
  require(result.valid && test.reader.queries.size() == 72, "Fragmented readable query windows were not handled");
  test.reader.query_chunk = 0;
  result = test.run();
  require(!result.valid && test.reader.queries.size() == 1 && test.reader.reads.empty(), "Zero-progress query loop was not refused");
  test.reader.oversized_window = true;
  result = test.run();
  require(!result.valid && test.reader.reads.empty(), "Oversized query window was accepted");
}

void decoder_and_no_follow() {
  for (const auto target : {0x1000u, 0x2800u, 0x3100u, 0x5000u, 0u, 0xffffffffu}) {
    Fixture test;
    test.reader.relative(kEntry, target);
    test.reader.patch(kEntry + 5, {0xc3});
    const auto result = test.run();
    require(result.valid &&
                result.entries[0].instructions[0].direct_call_target_rva == (target == 0x1000 || target == 0x2800 ? target : 0) &&
                test.reader.reads.size() == 2 && test.reader.queries.size() == 2,
            "Outgoing call was followed or its metadata escaped executable image bounds");
  }
  Fixture test;
  test.reader.relative(kEntry, 0x2800, 0xe9);
  test.reader.patch(kEntry + 5, {0xeb, 0x60, 0xc3});
  auto result = test.run();
  require(result.valid && result.entries[0].decoded_bytes == 8 && test.reader.reads.size() == 2 &&
              std::all_of(result.entries[0].instructions.begin(), result.entries[0].instructions.end(),
                          [](const auto& instruction) { return instruction.direct_call_target_rva == 0; }),
          "Branch was followed or confused with direct-call metadata");
  for (int mutation = 0; mutation < 5; ++mutation) {
    test = Fixture{};
    switch (mutation) {
      case 0:
        test.decoder.fail_rva = kEntry + 1;
        break;
      case 1:
        test.decoder.forced_size = 16;
        break;
      case 2:
        test.decoder.forced_size = 65;
        break;
      case 3:
        test.decoder.oversized_text = true;
        break;
      case 4:
        test.decoder.empty_text = true;
        break;
    }
    result = test.run();
    require(!result.valid && result.entries[0].stop_reason == "decode_failed", "Invalid decoder result was accepted");
  }
}

void limits_and_partial_metadata() {
  Fixture test;
  test.requests.clear();
  test.reader.bytes.clear();
  for (std::uint32_t index = 0; index < 8; ++index) {
    test.requests.push_back({kSlot + index * 8, kEntry + index * 64});
    test.reader.pointer(kSlot + index * 8, kBase + kEntry + index * 64);
  }
  auto result = test.run();
  require(result.valid && result.read_bytes == 512 && result.proof_bytes == 64 && result.entries.size() == 8,
          "Eight-request aggregate limit was not supported exactly");
  test.requests.push_back({kSlot + 64, kEntry + 512});
  reject_before_read(test);
  test.requests.clear();
  reject_before_read(test);
  test = Fixture{};
  test.requests.push_back({kSlot + 8, kEntry + 64});
  test.reader.pointer(kSlot + 8, kBase + kEntry + 65);
  result = test.run();
  require(!result.valid && result.entries.size() == 2 && result.entries[0].ret_observed && result.entries[0].error.empty() &&
              result.entries[1].stop_reason == "proof_mismatch" && result.read_bytes == 64 && result.proof_bytes == 16,
          "A failed peer discarded successful metadata or silently validated partial results");
}
}  // namespace

int main() {
  try {
    returns_and_boundaries();
    static_refusals();
    stale_proofs_and_read_failures();
    decoder_and_no_follow();
    limits_and_partial_metadata();
    std::printf("slot_prefix_test: %u checks passed\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "slot_prefix_test: %s\n", error.what());
    return 1;
  }
}
