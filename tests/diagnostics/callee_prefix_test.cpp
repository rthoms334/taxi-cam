#include "../../tools/diagnostics/callee_prefix.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace {
using namespace taxi_camera::discovery;
static_assert(kMaximumTargetPrefixReadBytes == 512 && kMaximumCallProofReadBytes == 40 && kMaximumBranchProofReadBytes == 48);

constexpr std::uint32_t kCall = 0x1100;
constexpr std::uint32_t kTarget = 0x2000;
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

  void call(std::uint32_t rva, std::uint32_t target) {
    const auto displacement = target - (rva + 5);
    patch(rva, {0xe8, static_cast<std::uint8_t>(displacement), static_cast<std::uint8_t>(displacement >> 8),
                static_cast<std::uint8_t>(displacement >> 16), static_cast<std::uint8_t>(displacement >> 24)});
  }

  void branch(std::uint32_t rva, std::uint32_t target, std::uint8_t opcode = 0xe9, bool near_conditional = false) {
    const auto size = near_conditional ? 6u : opcode == 0xe9 ? 5u : 2u;
    const auto displacement = target - (rva + size);
    std::vector<std::uint8_t> data;
    if (near_conditional)
      data.push_back(0x0f);
    data.push_back(opcode);
    data.push_back(static_cast<std::uint8_t>(displacement));
    if (size != 2) {
      data.push_back(static_cast<std::uint8_t>(displacement >> 8));
      data.push_back(static_cast<std::uint8_t>(displacement >> 16));
      data.push_back(static_cast<std::uint8_t>(displacement >> 24));
    }
    patch(rva, data);
  }
};

struct Decoder final : InstructionDecoder {
  std::vector<std::uint32_t> calls;
  std::uint32_t fail_rva = 0;
  std::size_t forced_size = 0;
  bool oversized_text = false;

  std::size_t decode(std::uint8_t* bytes, std::size_t available, std::uint32_t rva, std::string& instruction) override {
    calls.push_back(rva);
    if (rva == fail_rva || available == 0)
      return 0;
    if (oversized_text) {
      instruction.assign(513, 'x');
      return 1;
    }
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
      case 0x0f:
        if (available >= 2 && bytes[1] >= 0x80 && bytes[1] <= 0x8f) {
          instruction = "jcc rel32 " + std::to_string(bytes[1]);
          size = 6;
        }
        break;
      case 0x48:
        if (available >= 2 && bytes[1] == 0xb8) {
          instruction = "mov rax, imm64";
          size = 10;
        }
        break;
      default:
        if (bytes[0] >= 0x70 && bytes[0] <= 0x7f) {
          instruction = "jcc rel8 " + std::to_string(bytes[0]);
          size = 2;
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
  FunctionInventory proof;
  std::vector<CalleePrefixRequest> requests{{kCall, kTarget}};

  Fixture() {
    image.valid_image = true;
    image.machine = 0x8664;
    image.image_size = 0x4000;
    image.sections = {{".text", 0x1000, 0x3000, 0x60000020}};
    proof.valid_targets = true;
    FunctionContext function;
    function.function_begin_rva = kCall;
    function.function_end_rva = kCall + 0x100;
    function.requested_rva = kCall;
    function.requested_boundary_found = true;
    function.bytes_read = function.decoded_bytes = 0x100;
    proof.functions.push_back(function);
    prove(kCall, kTarget);
    reader.patch(kTarget, {0x90, 0xc3, 0xff});
  }

  void prove(std::uint32_t call, std::uint32_t target) {
    proof.functions[0].instructions.push_back({call, "call rel32", target});
    reader.call(call, target);
  }

  CalleePrefixInventory run() {
    reader.reads.clear();
    reader.queries.clear();
    decoder.calls.clear();
    const auto result = inspect_callee_prefixes(reader, image, decoder, proof, requests);
    require(result.read_bytes <= 512 && result.proof_bytes <= 40 && result.entries.size() <= 8, "Aggregate cap exceeded");
    std::uint32_t prefix_bytes = 0;
    std::uint32_t proof_bytes = 0;
    for (const auto& read : reader.reads) {
      const auto request = std::find_if(requests.begin(), requests.end(), [&](const auto& candidate) {
        return candidate.call_rva == read.first || candidate.target_rva == read.first;
      });
      require(request != requests.end(), "Reader followed an unrequested address");
      if (request->call_rva == read.first) {
        require(read.second == 5, "Proof read was not exactly five bytes");
        proof_bytes += read.second;
      } else {
        require(read.second > 0 && read.second <= 64 && std::uint64_t(read.first) + read.second <= image.image_size,
                "Target read crossed the prefix or image cap");
        prefix_bytes += read.second;
      }
    }
    require(result.read_bytes == prefix_bytes && result.proof_bytes == proof_bytes,
            "Attempted read-byte counters differ from reader calls");
    for (const auto& entry : result.entries)
      require(entry.decoded_bytes <= entry.attempted_bytes && entry.attempted_bytes <= 64 && entry.instructions.size() <= 64,
              "Decoded metadata exceeded the read prefix");
    return result;
  }
};

void reject_before_read(Fixture& test, const char* message) {
  const auto result = test.run();
  require(!result.valid && !result.error.empty() && test.reader.reads.empty() && test.reader.queries.empty(), message);
}

void basic_and_returns() {
  Fixture test;
  auto result = test.run();
  const auto& entry = result.entries[0];
  require(result.valid && result.error.empty() && result.read_failures == 0 && result.read_bytes == 64 && result.proof_bytes == 5 &&
              entry.proof_call_rva == kCall && entry.entry_rva == kTarget && entry.ret_observed && entry.stop_reason == "ret" &&
              entry.attempted_bytes == 64 && entry.decoded_bytes == 2 && entry.instructions.size() == 2,
          "An aligned RET did not stop decoding after a bounded prefix read");
  require(test.decoder.calls == std::vector<std::uint32_t>{kTarget, kTarget + 1}, "Decoder continued after RET");

  test = Fixture{};
  test.reader.patch(kTarget, {0xb8, 0xc3, 0xc2, 0x00, 0x00, 0xc2, 0x18, 0x00, 0xff});
  result = test.run();
  require(result.valid && result.entries[0].ret_observed && result.entries[0].decoded_bytes == 8 &&
              test.decoder.calls == std::vector<std::uint32_t>{kTarget, kTarget + 5},
          "A RET byte inside an immediate was treated as an instruction boundary");

  test = Fixture{};
  test.reader.patch(kTarget, {0x48, 0xb8, 0xc3, 0xc2, 0xc3, 0x90, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3});
  result = test.run();
  require(result.valid && result.entries[0].decoded_bytes == 11 && result.entries[0].instructions.size() == 2,
          "An imm64 RET byte prematurely ended decoding");

  test = Fixture{};
  test.requests[0].target_rva = 0x1000;
  test.proof.functions[0].instructions[0].direct_call_target_rva = 0x1000;
  test.reader.call(kCall, 0x1000);
  test.reader.patch(0x1000, {0xc3});
  result = test.run();
  require(result.valid && result.entries[0].ret_observed && result.entries[0].decoded_bytes == 1,
          "A negative rel32 displacement failed normalization");

  test = Fixture{};
  test.reader.call(kTarget, 0x3000);
  test.reader.patch(kTarget + 5, {0xc3});
  result = test.run();
  require(result.valid && result.entries[0].instructions[0].direct_call_target_rva == 0x3000 && test.reader.reads.size() == 2,
          "An observed outgoing call was followed instead of recorded as metadata");

  test = Fixture{};
  test.reader.patch(kTarget, {0xe9, 0xfb, 0x0f, 0x00, 0x00, 0xc3});
  result = test.run();
  require(result.valid && test.decoder.calls == std::vector<std::uint32_t>{kTarget, kTarget + 5} && test.reader.reads.size() == 2,
          "A branch target was followed instead of decoding the captured linear prefix");

  test = Fixture{};
  test.reader.patch(kTarget, {0xc3});
  test.decoder.forced_size = 2;
  result = test.run();
  require(result.valid && !result.entries[0].ret_observed && result.entries[0].stop_reason == "prefix_limit",
          "A decoder result with the wrong RET instruction length was accepted as RET");
}

void proof_refusals() {
  Fixture test;
  test.proof.valid_targets = false;
  reject_before_read(test, "Invalid aggregate proof was accepted");
  test = Fixture{};
  test.proof.partial = true;
  reject_before_read(test, "Partial aggregate proof was accepted");
  test = Fixture{};
  test.proof.error = "failed";
  reject_before_read(test, "Failed aggregate proof was accepted");
  test = Fixture{};
  test.proof.read_failures = 1;
  reject_before_read(test, "Proof with failed reads was accepted");
  test = Fixture{};
  test.proof.functions.clear();
  reject_before_read(test, "Empty function proof was accepted");
  test = Fixture{};
  test.proof.functions.resize(9, test.proof.functions[0]);
  reject_before_read(test, "An oversized proof set was accepted");

  for (int invalid = 0; invalid < 6; ++invalid) {
    test = Fixture{};
    auto& function = test.proof.functions[0];
    switch (invalid) {
      case 0:
        function.truncated = true;
        break;
      case 1:
        function.decode_failed = true;
        break;
      case 2:
        function.error = "failed";
        break;
      case 3:
        function.function_begin_rva = kCall + 1;
        break;
      case 4:
        function.function_end_rva = kCall + 4;
        break;
      case 5:
        function.decoded_bytes = 4;
        break;
    }
    reject_before_read(test, "An incomplete or out-of-range source context proved a call");
  }

  test = Fixture{};
  test.proof.functions[0].instructions[0].direct_call_target_rva = kTarget + 1;
  reject_before_read(test, "A mismatched metadata target was accepted");
  test = Fixture{};
  test.proof.functions[0].instructions = {{kCall, "mov eax, imm32", 0}, {kCall + 5, "ret", 0}};
  test.reader.patch(kCall, {0xb8});
  test.reader.call(kCall + 1, kTarget);
  test.requests[0].call_rva = kCall + 1;
  reject_before_read(test, "An E8 byte inside an immediate supplied boundary proof");

  test = Fixture{};
  test.reader.call(kCall, kTarget + 1);
  auto result = test.run();
  require(!result.valid && result.entries[0].stop_reason == "proof_mismatch" && result.proof_bytes == 5 && result.read_bytes == 0 &&
              test.decoder.calls.empty(),
          "Mutation of the call target was not detected by the fresh read");
  test = Fixture{};
  test.reader.patch(kCall, {0xe9});
  result = test.run();
  require(!result.valid && result.entries[0].stop_reason == "proof_mismatch" && result.read_bytes == 0,
          "A changed call opcode was accepted");
  test = Fixture{};
  test.reader.patch(kCall, {0xe8, 0, 0, 0, 0x80});
  require(!test.run().valid, "A negative call target wrapped into a valid RVA");
}

void bounds_and_caps() {
  Fixture test;
  test.requests.clear();
  reject_before_read(test, "An empty target list was accepted");
  test = Fixture{};
  test.requests.resize(9, test.requests[0]);
  reject_before_read(test, "More than eight requests caused reads");

  test = Fixture{};
  test.reader.bytes.clear();
  test.proof.functions[0].instructions.clear();
  test.requests.clear();
  for (std::uint32_t index = 0; index < 8; ++index) {
    const auto call = kCall + 16 * index;
    const auto target = kTarget + 128 * index;
    test.prove(call, target);
    test.requests.push_back({call, target});
  }
  auto result = test.run();
  require(result.valid && result.read_bytes == 512 && result.proof_bytes == 40 && result.entries.size() == 8,
          "Eight full prefixes did not respect both independent byte budgets");
  for (const auto& entry : result.entries)
    require(entry.stop_reason == "prefix_limit" && !entry.ret_observed && entry.decoded_bytes == 64,
            "A prefix cap was misreported as function completion or failure");

  for (const auto ret_offset : {63u, 64u}) {
    test = Fixture{};
    test.reader.bytes.clear();
    test.reader.call(kCall, kTarget);
    test.reader.patch(kTarget + ret_offset, {0xc3});
    result = test.run();
    require(result.valid && result.read_bytes == 64 && result.entries[0].ret_observed == (ret_offset == 63),
            "RET handling crossed the 64-byte target-prefix cap");
  }

  for (std::uint32_t remaining = 1; remaining <= 64; ++remaining) {
    test = Fixture{};
    test.reader.bytes.clear();
    test.reader.call(kCall, kTarget);
    test.image.image_size = kTarget + remaining;
    test.image.sections[0].size = test.image.image_size - test.image.sections[0].rva;
    result = test.run();
    require(result.valid && result.read_bytes == remaining && result.entries[0].decoded_bytes == remaining &&
                result.entries[0].stop_reason == (remaining == 64 ? "prefix_limit" : "section_end"),
            "A prefix was not clipped to its section/image boundary");
  }
  test = Fixture{};
  test.image.sections[0].size = kTarget + 1 - 0x1000;
  test.image.sections.push_back({".next", kTarget + 1, 0x100, 0x60000020});
  result = test.run();
  require(result.valid && result.read_bytes == 1 && result.entries[0].stop_reason == "section_end",
          "A prefix crossed into an adjacent executable section");

  for (const auto flags : {0u, 0x40000020u, 0x20000020u, 0xe0000020u, 0x62000020u}) {
    test = Fixture{};
    test.image.sections[0].flags = flags;
    reject_before_read(test, "Unreadable, writable, non-executable or discardable code was read");
  }
  for (const auto target : {0u, 0xfffu, 0x4000u, std::numeric_limits<std::uint32_t>::max()}) {
    test = Fixture{};
    test.requests[0].target_rva = target;
    test.proof.functions[0].instructions[0].direct_call_target_rva = target;
    reject_before_read(test, "An out-of-image or undeclared target was read");
  }
  test = Fixture{};
  test.requests[0].call_rva = 0x3ffc;
  reject_before_read(test, "A five-byte call extending beyond the image was read");
  test = Fixture{};
  test.image.sections[0].size = std::numeric_limits<std::uint32_t>::max();
  reject_before_read(test, "Overflowed section metadata was accepted");
  test = Fixture{};
  test.image.sections.push_back({".overlap", kTarget, 32, 0x60000020});
  reject_before_read(test, "Overlapping sections were accepted");
  test = Fixture{};
  test.image.sections.clear();
  reject_before_read(test, "An image without sections was accepted");
  test = Fixture{};
  test.image.machine = 0x14c;
  reject_before_read(test, "A non-AMD64 image was accepted");
  test = Fixture{};
  test.image.valid_image = false;
  reject_before_read(test, "An unvalidated image was accepted");
}

void failure_and_peer_preservation() {
  for (const auto failing_rva : {kCall, kTarget}) {
    Fixture test;
    test.reader.fail_rva = failing_rva;
    auto result = test.run();
    require(!result.valid && result.read_failures == 1 && result.entries[0].stop_reason == "unreadable" && result.proof_bytes == 5 &&
                result.read_bytes == (failing_rva == kCall ? 0u : 64u) &&
                result.entries[0].attempted_bytes == (failing_rva == kCall ? 0u : 64u) && result.entries[0].decoded_bytes == 0,
            "A failed exact read was accepted or incorrectly counted");
    test = Fixture{};
    test.reader.hole_rva = failing_rva;
    result = test.run();
    require(!result.valid && result.read_failures == 1 && result.read_bytes == 0 && result.proof_bytes == (failing_rva == kCall ? 0u : 5u),
            "A failed readability query was counted as an attempted byte read");
  }
  for (std::uint32_t chunk = 1; chunk <= 8; ++chunk) {
    Fixture test;
    test.reader.query_chunk = chunk;
    require(test.run().valid, "Readable split query windows were refused");
  }
  Fixture test;
  test.reader.query_chunk = 0;
  require(!test.run().valid, "A zero-sized query window failed to stop the reader");
  test = Fixture{};
  test.reader.oversized_window = true;
  require(!test.run().valid, "An oversized query window was accepted");

  test = Fixture{};
  test.decoder.fail_rva = kTarget + 1;
  auto result = test.run();
  require(!result.valid && result.entries[0].stop_reason == "decode_failed" && result.entries[0].decoded_bytes == 1 &&
              result.entries[0].instructions.size() == 1 && !result.entries[0].ret_observed && result.read_failures == 0,
          "A decoder failure discarded the valid prefix evidence or became a read failure");
  test = Fixture{};
  test.decoder.forced_size = 16;
  require(!test.run().valid, "An instruction longer than the x86 maximum was accepted");
  test = Fixture{};
  test.decoder.oversized_text = true;
  require(!test.run().valid, "Unbounded decoder text was accepted");
  test = Fixture{};
  test.image.sections[0].size = kTarget + 1 - 0x1000;
  test.decoder.forced_size = 2;
  require(!test.run().valid, "A decoded instruction extending beyond the prefix was accepted");
  test = Fixture{};
  test.reader.patch(kTarget, {0xc2});
  test.image.sections[0].size = kTarget + 2 - 0x1000;
  result = test.run();
  require(!result.valid && result.entries[0].stop_reason == "decode_failed" && !result.entries[0].ret_observed,
          "A clipped RET imm16 was treated as a decoded RET");

  test = Fixture{};
  test.requests.push_back({kCall + 16, kTarget + 128});
  test.prove(kCall + 16, kTarget + 128);
  test.reader.patch(kTarget + 128, {0xc3});
  test.reader.call(kCall, kTarget + 1);
  result = test.run();
  require(!result.valid && result.entries.size() == 2 && !result.entries[0].error.empty() && result.entries[1].error.empty() &&
              result.entries[1].ret_observed && result.proof_bytes == 10 && result.read_bytes == 64,
          "An earlier proof failure prevented a successful peer capture");
  test = Fixture{};
  test.requests.push_back({kCall + 16, kTarget + 128});
  test.prove(kCall + 16, kTarget + 128);
  test.reader.patch(kTarget + 128, {0xff});
  result = test.run();
  require(!result.valid && result.entries[0].ret_observed && result.entries[0].error.empty() &&
              result.entries[1].stop_reason == "decode_failed" && result.read_bytes == 128,
          "A later decoder failure destroyed the successful earlier peer");
}

struct BranchFixture : Fixture {
  CalleePrefixInventory prefix_proof;
  std::vector<BranchPrefixRequest> branch_requests{{kTarget, 0x2800}};

  BranchFixture() {
    reader.branch(kTarget, 0x2800);
    reader.patch(kTarget + 5, {0xc3});
    reader.patch(0x2800, {0x90, 0xc3});
    capture();
  }

  void capture() {
    prefix_proof = Fixture::run();
    require(prefix_proof.valid, "The branch fixture did not capture a successful real callee prefix");
  }

  BranchPrefixInventory run_branch() {
    reader.reads.clear();
    reader.queries.clear();
    decoder.calls.clear();
    auto result = inspect_branch_prefixes(reader, image, decoder, prefix_proof, branch_requests);
    require(result.read_bytes <= 512 && result.proof_bytes <= 48 && result.entries.size() <= 8, "Branch aggregate cap exceeded");
    std::uint32_t target_bytes = 0;
    std::uint32_t proof_bytes = 0;
    for (const auto& read : reader.reads) {
      const auto proof_read = std::find_if(branch_requests.begin(), branch_requests.end(),
                                           [&](const auto& request) { return request.proof_branch_rva == read.first; });
      const auto target_read = std::find_if(branch_requests.begin(), branch_requests.end(),
                                            [&](const auto& request) { return request.entry_rva == read.first; });
      require(proof_read != branch_requests.end() || target_read != branch_requests.end(), "Branch reader followed an unrequested address");
      if (proof_read != branch_requests.end()) {
        require(read.second == 2 || read.second == 5 || read.second == 6, "Branch proof read had an unbounded or unsupported size");
        proof_bytes += read.second;
      } else {
        require(read.second > 0 && read.second <= 64 && std::uint64_t(read.first) + read.second <= image.image_size,
                "Branch target read crossed its image or prefix bound");
        target_bytes += read.second;
      }
    }
    require(result.read_bytes == target_bytes && result.proof_bytes == proof_bytes, "Branch attempted-byte counters were wrong");
    for (const auto& entry : result.entries)
      require(entry.attempted_bytes <= 64 && entry.decoded_bytes <= entry.attempted_bytes && entry.instructions.size() <= 64,
              "Branch decoded metadata exceeded the target prefix");
    return result;
  }
};

void reject_branch_before_read(BranchFixture& test, const char* message) {
  const auto result = test.run_branch();
  require(!result.valid && !result.error.empty() && test.reader.reads.empty() && test.reader.queries.empty(), message);
}

void branch_encodings_and_returns() {
  BranchFixture test;
  auto result = test.run_branch();
  require(result.valid && result.entries[0].proof_branch_rva == kTarget && result.entries[0].entry_rva == 0x2800 &&
              result.entries[0].ret_observed && result.entries[0].decoded_bytes == 2 && result.proof_bytes == 5 && result.read_bytes == 64,
          "An exact E9 branch did not produce one bounded target prefix");
  require(test.decoder.calls == std::vector<std::uint32_t>{kTarget, 0x2800, 0x2801}, "Branch proof or target decoding escaped its scope");

  for (const auto opcode :
       {0xebu, 0x70u, 0x71u, 0x72u, 0x73u, 0x74u, 0x75u, 0x76u, 0x77u, 0x78u, 0x79u, 0x7au, 0x7bu, 0x7cu, 0x7du, 0x7eu, 0x7fu}) {
    for (const auto target : {kTarget - 32, kTarget + 32}) {
      test = BranchFixture{};
      test.branch_requests[0].entry_rva = target;
      test.reader.branch(kTarget, target, static_cast<std::uint8_t>(opcode));
      test.reader.patch(kTarget + 2, {0xc3});
      test.reader.patch(target, {0xc3});
      test.capture();
      result = test.run_branch();
      require(result.valid && result.proof_bytes == 2 && result.entries[0].ret_observed,
              "An accepted short relative branch failed positive/negative target normalization");
    }
  }
  for (std::uint8_t opcode = 0x80; opcode <= 0x8f; ++opcode) {
    for (const auto target : {0x1000u, 0x2800u}) {
      test = BranchFixture{};
      test.branch_requests[0].entry_rva = target;
      test.reader.branch(kTarget, target, opcode, true);
      test.reader.patch(kTarget + 6, {0xc3});
      test.reader.patch(target, {0xc3});
      test.capture();
      result = test.run_branch();
      require(result.valid && result.proof_bytes == 6 && result.entries[0].ret_observed,
              "An accepted near Jcc failed positive/negative target normalization");
    }
  }
  test = BranchFixture{};
  test.branch_requests[0].entry_rva = 0x1000;
  test.reader.branch(kTarget, 0x1000);
  test.reader.patch(0x1000, {0xc3});
  test.capture();
  require(test.run_branch().valid, "A backward E9 branch failed normalization");

  test = BranchFixture{};
  test.reader.patch(0x2800, {0xb8, 0xc3, 0xc2, 0xc3, 0, 0xc2, 0x18, 0, 0xff});
  result = test.run_branch();
  require(result.valid && result.entries[0].ret_observed && result.entries[0].decoded_bytes == 8,
          "A RET byte inside a branch target immediate ended decoding");
  test = BranchFixture{};
  test.reader.branch(0x2800, 0x3000);
  test.reader.patch(0x2805, {0xc3});
  result = test.run_branch();
  require(result.valid && test.reader.reads.size() == 2 && result.entries[0].decoded_bytes == 6,
          "The branch target reader recursively followed a further branch");
}

void branch_proof_and_mutation_refusals() {
  for (unsigned mutation = 0; mutation < 13; ++mutation) {
    BranchFixture test;
    switch (mutation) {
      case 0:
        test.prefix_proof.valid = false;
        break;
      case 1:
        test.prefix_proof.error = "partial";
        break;
      case 2:
        test.prefix_proof.read_failures = 1;
        break;
      case 3:
        test.prefix_proof.entries.clear();
        break;
      case 4:
        test.prefix_proof.entries.resize(9, test.prefix_proof.entries[0]);
        break;
      case 5:
        test.prefix_proof.entries[0].error = "failed";
        break;
      case 6:
        test.prefix_proof.entries[0].decoded_bytes = 0;
        break;
      case 7:
        test.prefix_proof.entries[0].decoded_bytes = 65;
        break;
      case 8:
        test.prefix_proof.entries[0].stop_reason = "decode_failed";
        break;
      case 9:
        test.prefix_proof.entries[0].instructions[1].rva = kTarget;
        break;
      case 10:
        test.prefix_proof.entries[0].instructions[0].rva = kTarget + 1;
        break;
      case 11:
        test.prefix_proof.read_bytes = 513;
        break;
      case 12:
        test.prefix_proof.proof_bytes = 41;
        break;
    }
    reject_branch_before_read(test, "Invalid or incomplete callee metadata supplied branch proof");
  }
  BranchFixture test;
  test.reader.patch(kTarget, {0xb8, 0xeb, 0x1d, 0, 0, 0xc3});
  test.capture();
  test.branch_requests[0] = {kTarget + 1, kTarget + 32};
  reject_branch_before_read(test, "A branch opcode inside an immediate supplied an instruction boundary");

  test = BranchFixture{};
  test.reader.branch(kTarget, 0x2801);
  auto result = test.run_branch();
  require(!result.valid && result.entries[0].stop_reason == "proof_mismatch" && result.read_bytes == 0 && result.proof_bytes == 5,
          "A changed branch target survived the exact proof reread");
  test = BranchFixture{};
  test.reader.patch(kTarget, {0xb8});
  test.capture();
  test.reader.patch(kTarget, {0xe9});
  result = test.run_branch();
  require(!result.valid && result.read_bytes == 0 && result.entries[0].stop_reason == "proof_mismatch",
          "A newly written branch replaced a proven nonbranch instruction without detection");
  test = BranchFixture{};
  test.branch_requests[0].proof_branch_rva = kTarget + 6;
  reject_branch_before_read(test, "Bytes after a proven RET supplied branch-boundary proof");
  test = BranchFixture{};
  test.prefix_proof.entries[0].decoded_bytes = 7;
  test.prefix_proof.entries[0].instructions[1].rva = kTarget + 6;
  result = test.run_branch();
  require(!result.valid && result.proof_bytes == 6 && result.read_bytes == 0, "A mismatched decoded branch length was accepted");
  for (const auto opcode : {0xe8u, 0xeau, 0xffu, 0x90u, 0x66u}) {
    test = BranchFixture{};
    test.reader.patch(kTarget, {static_cast<std::uint8_t>(opcode)});
    result = test.run_branch();
    require(!result.valid && result.read_bytes == 0 && result.entries[0].stop_reason == "proof_mismatch",
            "A replaced call, indirect/far jump, prefix or nonbranch opcode was accepted");
  }
  test = BranchFixture{};
  test.reader.branch(kTarget, 0x2800, 0x85, true);
  test.reader.patch(kTarget + 6, {0xc3});
  test.capture();
  test.reader.patch(kTarget + 1, {0x84});
  result = test.run_branch();
  require(!result.valid && result.read_bytes == 0 && result.entries[0].stop_reason == "proof_mismatch",
          "Mutation of a condition code was not detected against decoded proof text");
  test = BranchFixture{};
  test.reader.patch(kTarget, {0xe9, 0, 0, 0, 0x80});
  require(!test.run_branch().valid, "Negative rel32 arithmetic wrapped into a permitted target");
  test = BranchFixture{};
  test.decoder.fail_rva = kTarget;
  result = test.run_branch();
  require(!result.valid && result.read_bytes == 0 && result.read_failures == 0,
          "A branch proof decoder failure triggered a target read or was counted as a memory-read failure");
}

void branch_bounds_failures_and_caps() {
  BranchFixture test;
  test.branch_requests.clear();
  reject_branch_before_read(test, "Empty branch requests caused reads");
  test = BranchFixture{};
  test.branch_requests.resize(9, test.branch_requests[0]);
  reject_branch_before_read(test, "More than eight branch requests caused reads");
  test = BranchFixture{};
  test.reader.bytes.clear();
  test.reader.call(kCall, kTarget);
  test.branch_requests.clear();
  for (std::uint32_t index = 0; index < 8; ++index) {
    const auto branch = kTarget + 6 * index;
    const auto target = 0x2800 + 128 * index;
    test.reader.branch(branch, target, 0x85, true);
    test.branch_requests.push_back({branch, target});
  }
  test.reader.patch(kTarget + 48, {0xc3});
  test.capture();
  auto result = test.run_branch();
  require(result.valid && result.read_bytes == 512 && result.proof_bytes == 48 && result.entries.size() == 8,
          "Eight branch requests did not retain independent 512+48-byte limits");

  for (std::uint32_t remaining = 1; remaining <= 64; ++remaining) {
    test = BranchFixture{};
    test.reader.bytes.erase(0x2801);
    test.image.image_size = 0x2800 + remaining;
    test.image.sections[0].size = test.image.image_size - 0x1000;
    result = test.run_branch();
    require(result.valid && result.read_bytes == remaining && result.entries[0].decoded_bytes == remaining &&
                result.entries[0].stop_reason == (remaining == 64 ? "prefix_limit" : "section_end"),
            "A branch prefix crossed its section or image boundary");
  }
  for (const auto target : {0u, 0xfffu, 0x4000u, std::numeric_limits<std::uint32_t>::max()}) {
    test = BranchFixture{};
    test.branch_requests[0].entry_rva = target;
    reject_branch_before_read(test, "An invalid branch target caused reads");
  }
  for (const auto flags : {0u, 0x40000020u, 0x20000020u, 0xe0000020u, 0x62000020u}) {
    test = BranchFixture{};
    test.image.sections[0].size = 0x1800;
    test.image.sections.push_back({".target", 0x2800, 0x1000, flags});
    reject_branch_before_read(test, "An ineligible target section supplied branch bytes");
  }
  test = BranchFixture{};
  test.image.sections[0].size = std::numeric_limits<std::uint32_t>::max();
  reject_branch_before_read(test, "Overflowed image section metadata supplied branch proof");
  test = BranchFixture{};
  test.image.sections[0].size = 0x1801;
  test.image.sections.push_back({".next", 0x2801, 0x100, 0x60000020});
  result = test.run_branch();
  require(result.valid && result.read_bytes == 1 && result.entries[0].stop_reason == "section_end",
          "A branch prefix crossed an adjacent executable section");
  test = BranchFixture{};
  test.requests[0].target_rva = 0x3ffb;
  test.proof.functions[0].instructions[0].direct_call_target_rva = 0x3ffb;
  test.reader.call(kCall, 0x3ffb);
  test.reader.branch(0x3ffb, 0x2800);
  test.branch_requests[0] = {0x3ffb, 0x2800};
  test.capture();
  result = test.run_branch();
  require(result.valid && result.proof_bytes == 5 && result.entries[0].ret_observed,
          "An exact branch ending at its source image boundary was refused or over-read");

  for (const auto rva : {kTarget, 0x2800u}) {
    test = BranchFixture{};
    test.reader.fail_rva = rva;
    result = test.run_branch();
    require(!result.valid && result.read_failures == 1 && result.proof_bytes == 5 && result.read_bytes == (rva == kTarget ? 0u : 64u),
            "A failed branch proof/target exact read had incorrect accounting");
    test = BranchFixture{};
    test.reader.hole_rva = rva;
    result = test.run_branch();
    require(!result.valid && result.read_failures == 1 && result.proof_bytes == (rva == kTarget ? 0u : 5u) && result.read_bytes == 0,
            "A refused branch proof/target query attempted to read bytes");
  }
  test = BranchFixture{};
  test.reader.query_chunk = 1;
  require(test.run_branch().valid, "Split readable branch query windows were refused");
  test = BranchFixture{};
  test.decoder.fail_rva = 0x2801;
  result = test.run_branch();
  require(!result.valid && result.entries[0].decoded_bytes == 1 && result.entries[0].stop_reason == "decode_failed",
          "A branch target decoder failure discarded partial evidence");

  test = BranchFixture{};
  test.reader.branch(kTarget + 5, 0x2900);
  test.reader.patch(kTarget + 10, {0xc3});
  test.reader.patch(0x2900, {0xc3});
  test.branch_requests.push_back({kTarget + 5, 0x2900});
  test.capture();
  test.reader.branch(kTarget, 0x2801);
  result = test.run_branch();
  require(!result.valid && result.entries.size() == 2 && !result.entries[0].error.empty() && result.entries[1].ret_observed &&
              result.entries[1].error.empty() && result.read_bytes == 64 && result.proof_bytes == 10,
          "A branch proof failure discarded its successful peer");
}

struct FurtherBranchFixture : BranchFixture {
  BranchPrefixInventory branch_proof;
  std::vector<BranchPrefixRequest> further_requests{{0x2800, 0x2820}};

  FurtherBranchFixture() {
    reader.branch(0x2800, 0x2820, 0x75);
    reader.patch(0x2802, {0xc3});
    reader.patch(0x2820, {0x90, 0xc3});
    capture_branch();
  }

  void capture_branch() {
    branch_proof = run_branch();
    require(branch_proof.valid, "The further-branch fixture requires a real successful first-branch capture");
  }

  BranchPrefixInventory run_further() {
    reader.reads.clear();
    reader.queries.clear();
    decoder.calls.clear();
    auto result = inspect_branch_prefixes(reader, image, decoder, branch_proof, further_requests);
    require(result.read_bytes <= 512 && result.proof_bytes <= 48 && result.entries.size() <= 8, "Further-branch aggregate cap exceeded");
    std::uint32_t proof_bytes = 0;
    std::uint32_t target_bytes = 0;
    for (const auto& read : reader.reads) {
      const auto proof_read = std::find_if(further_requests.begin(), further_requests.end(),
                                           [&](const auto& request) { return request.proof_branch_rva == read.first; });
      const auto target_read = std::find_if(further_requests.begin(), further_requests.end(),
                                            [&](const auto& request) { return request.entry_rva == read.first; });
      require(proof_read != further_requests.end() || target_read != further_requests.end(),
              "The further-branch reader revisited an ancestor or followed an unrequested address");
      if (proof_read != further_requests.end()) {
        require(read.second == 2 || read.second == 5 || read.second == 6, "Unbounded further-branch proof read");
        proof_bytes += read.second;
      } else {
        require(read.second > 0 && read.second <= 64 && std::uint64_t(read.first) + read.second <= image.image_size,
                "Further-branch target read escaped its bound");
        target_bytes += read.second;
      }
    }
    require(result.read_bytes == target_bytes && result.proof_bytes == proof_bytes, "Further-branch attempted-byte counters were wrong");
    for (const auto& entry : result.entries)
      require(entry.attempted_bytes <= 64 && entry.decoded_bytes <= entry.attempted_bytes && entry.instructions.size() <= 64,
              "Further-branch decoded metadata exceeded its read");
    return result;
  }
};

void reject_further_before_read(FurtherBranchFixture& test) {
  const auto result = test.run_further();
  require(!result.valid && !result.error.empty() && test.reader.reads.empty() && test.reader.queries.empty(),
          "Invalid second-edge proof/request caused queries or reads");
}

void further_branch_success_and_refusals() {
  FurtherBranchFixture test;
  auto result = test.run_further();
  require(result.valid && result.proof_bytes == 2 && result.read_bytes == 64 && result.entries[0].proof_branch_rva == 0x2800 &&
              result.entries[0].entry_rva == 0x2820 && result.entries[0].ret_observed && result.entries[0].decoded_bytes == 2 &&
              test.decoder.calls == std::vector<std::uint32_t>{0x2800, 0x2820, 0x2821},
          "The fresh first-branch inventory did not directly prove the explicit second edge");
  test.reader.branch(0x2820, 0x2900);
  test.reader.patch(0x2825, {0xc3});
  result = test.run_further();
  require(result.valid && result.entries[0].decoded_bytes == 6 && test.reader.reads.size() == 2,
          "Second-edge capture automatically traversed a third edge");

  for (unsigned mutation = 0; mutation < 14; ++mutation) {
    test = FurtherBranchFixture{};
    switch (mutation) {
      case 0:
        test.branch_proof.valid = false;
        break;
      case 1:
        test.branch_proof.error = "partial";
        break;
      case 2:
        test.branch_proof.read_failures = 1;
        break;
      case 3:
        test.branch_proof.entries.clear();
        break;
      case 4:
        test.branch_proof.entries.resize(9, test.branch_proof.entries[0]);
        break;
      case 5:
        test.branch_proof.entries[0].error = "partial";
        break;
      case 6:
        test.branch_proof.entries[0].attempted_bytes = 0;
        break;
      case 7:
        test.branch_proof.entries[0].decoded_bytes = 65;
        break;
      case 8:
        test.branch_proof.entries[0].stop_reason = "decode_failed";
        break;
      case 9:
        test.branch_proof.entries[0].instructions[1].rva = 0x2800;
        break;
      case 10:
        test.branch_proof.entries[0].instructions[0].rva = 0x2801;
        break;
      case 11:
        test.branch_proof.read_bytes = 513;
        break;
      case 12:
        test.branch_proof.proof_bytes = 49;
        break;
      case 13:
        test.branch_proof.entries[0].instructions[0].text.clear();
        break;
    }
    reject_further_before_read(test);
  }
  test = FurtherBranchFixture{};
  test.further_requests[0].proof_branch_rva = 0x2803;
  reject_further_before_read(test);
  test = FurtherBranchFixture{};
  test.reader.patch(0x2800, {0xb8, 0x75, 0x1d, 0, 0, 0xc3});
  test.capture_branch();
  test.further_requests[0] = {0x2801, 0x2820};
  reject_further_before_read(test);

  test = FurtherBranchFixture{};
  test.reader.branch(0x2800, 0x2821, 0x75);
  result = test.run_further();
  require(!result.valid && result.read_bytes == 0 && result.proof_bytes == 2 && result.entries[0].stop_reason == "proof_mismatch",
          "A stale second-edge target survived fresh proof verification");
  test = FurtherBranchFixture{};
  test.reader.patch(0x2800, {0x74});
  result = test.run_further();
  require(!result.valid && result.read_bytes == 0 && result.entries[0].stop_reason == "proof_mismatch",
          "A changed second-edge condition survived exact decoded-text verification");
  test = FurtherBranchFixture{};
  test.decoder.fail_rva = 0x2800;
  result = test.run_further();
  require(!result.valid && result.read_bytes == 0 && result.read_failures == 0, "Second-edge proof decode failure allowed target access");
}

void further_branch_bounds_and_failures() {
  FurtherBranchFixture test;
  test.further_requests.clear();
  reject_further_before_read(test);
  test = FurtherBranchFixture{};
  test.further_requests.resize(9, test.further_requests[0]);
  reject_further_before_read(test);
  test = FurtherBranchFixture{};
  test.image.valid_image = false;
  reject_further_before_read(test);
  test = FurtherBranchFixture{};
  test.image.sections.push_back({".overlap", 0x2800, 64, 0x60000020});
  reject_further_before_read(test);
  test = FurtherBranchFixture{};
  test.reader.branch(0x2800, 0x3000);
  test.reader.patch(0x2805, {0xc3});
  test.further_requests[0] = {0x2800, 0x3000};
  test.capture_branch();
  test.image.sections[0].size = 0x2000;
  test.image.sections.push_back({".writable", 0x3000, 0x1000, 0xe0000020});
  reject_further_before_read(test);
  test = FurtherBranchFixture{};
  test.further_requests[0].entry_rva = 0x4000;
  reject_further_before_read(test);

  for (std::uint32_t remaining = 1; remaining <= 64; ++remaining) {
    test = FurtherBranchFixture{};
    test.reader.branch(0x2800, 0x3000);
    test.reader.patch(0x2805, {0xc3});
    test.further_requests[0] = {0x2800, 0x3000};
    test.capture_branch();
    test.image.image_size = 0x3000 + remaining;
    test.image.sections[0].size = test.image.image_size - 0x1000;
    const auto result = test.run_further();
    require(result.valid && result.read_bytes == remaining && result.entries[0].decoded_bytes == remaining &&
                result.entries[0].stop_reason == (remaining == 64 ? "prefix_limit" : "section_end") && !result.entries[0].ret_observed,
            "Second-edge prefix was not clipped to its section/image boundary");
  }
  for (const auto rva : {0x2800u, 0x2820u}) {
    test = FurtherBranchFixture{};
    test.reader.fail_rva = rva;
    auto result = test.run_further();
    require(!result.valid && result.read_failures == 1 && result.proof_bytes == 2 && result.read_bytes == (rva == 0x2800 ? 0u : 64u),
            "Failed second-edge exact read had inaccurate counters");
    test = FurtherBranchFixture{};
    test.reader.hole_rva = rva;
    result = test.run_further();
    require(!result.valid && result.read_failures == 1 && result.read_bytes == 0 && result.proof_bytes == (rva == 0x2800 ? 0u : 2u),
            "Refused second-edge query caused an exact read");
  }
  test = FurtherBranchFixture{};
  test.decoder.fail_rva = 0x2821;
  auto result = test.run_further();
  require(!result.valid && result.entries[0].decoded_bytes == 1 && result.entries[0].stop_reason == "decode_failed",
          "Second-edge target failure discarded its earlier decoded instruction");

  test = FurtherBranchFixture{};
  test.reader.bytes.erase(0x2802);
  test.further_requests.clear();
  for (std::uint32_t index = 0; index < 8; ++index) {
    test.reader.branch(0x2800 + index * 6, 0x3000 + index * 128, 0x85, true);
    test.further_requests.push_back({0x2800 + index * 6, 0x3000 + index * 128});
  }
  test.reader.patch(0x2830, {0xc3});
  test.capture_branch();
  result = test.run_further();
  require(result.valid && result.proof_bytes == 48 && result.read_bytes == 512 && result.entries.size() == 8,
          "Second-edge invocation did not support its independent eight-request 512+48-byte cap");
  test.reader.branch(0x2800, 0x3001, 0x85, true);
  result = test.run_further();
  require(!result.valid && result.entries[0].stop_reason == "proof_mismatch" && result.entries[1].error.empty() &&
              result.read_bytes == 448 && result.proof_bytes == 48,
          "A failed second edge discarded successful peers or reused its ancestor's budget");

  // A first-branch capture can itself consume the full 48 proof bytes. That
  // real result must not be incorrectly subjected to the callee's 40-byte cap.
  test = FurtherBranchFixture{};
  test.reader.bytes.clear();
  test.reader.call(kCall, kTarget);
  test.branch_requests.clear();
  for (std::uint32_t index = 0; index < 8; ++index) {
    test.reader.branch(kTarget + index * 6, 0x2800 + index * 128, 0x85, true);
    test.branch_requests.push_back({kTarget + index * 6, 0x2800 + index * 128});
  }
  test.reader.patch(kTarget + 48, {0xc3});
  test.reader.branch(0x2800, 0x3820);
  test.reader.patch(0x2805, {0xc3});
  test.reader.patch(0x3820, {0xc3});
  test.capture();
  test.capture_branch();
  require(test.branch_proof.proof_bytes == 48 && test.branch_proof.read_bytes == 512, "Maximum parent proof fixture was not captured");
  test.further_requests = {{0x2800, 0x3820}};
  result = test.run_further();
  require(result.valid && result.proof_bytes == 5 && result.read_bytes == 64 && result.entries[0].ret_observed,
          "Valid 48-byte branch proof was incorrectly limited to the callee proof budget");
}

}  // namespace

int main() {
  try {
    basic_and_returns();
    proof_refusals();
    bounds_and_caps();
    failure_and_peer_preservation();
    branch_encodings_and_returns();
    branch_proof_and_mutation_refusals();
    branch_bounds_failures_and_caps();
    further_branch_success_and_refusals();
    further_branch_bounds_and_failures();
    std::printf("PASS: %u callee/branch-prefix checks; exact call/branch proof, aligned RET, failures, clipping and 512+40/48-byte caps.\n",
                checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
