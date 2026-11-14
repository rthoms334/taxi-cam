#include "../../src/camera/image_inventory.hpp"
#include "../../tools/diagnostics/reference_inventory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {
using namespace taxi_camera::discovery;

struct SyntheticImage final : ImageReader {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x6000);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> holes;
  std::uint32_t window_size = 64;
  bool no_progress = false;
  bool fail_reads = false;

  ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    if (no_progress || rva >= bytes.size()) {
      return {};
    }
    auto count = std::min<std::uint32_t>({maximum, window_size, static_cast<std::uint32_t>(bytes.size()) - rva});
    for (const auto& hole : holes) {
      if (rva >= hole.first && rva < hole.second) {
        return {std::min(count, hole.second - rva), false};
      }
      if (rva < hole.first) {
        count = std::min(count, hole.first - rva);
      }
    }
    return {count, true};
  }

  bool read(std::uint32_t rva, void* destination, std::size_t size) override {
    if (fail_reads || rva > bytes.size() || size > bytes.size() - rva) {
      return false;
    }
    for (const auto& hole : holes) {
      if (rva < hole.second && rva + size > hole.first) {
        return false;
      }
    }
    if (size != 0) {
      std::memcpy(destination, bytes.data() + rva, size);
    }
    return true;
  }

  void word(std::uint32_t offset, std::uint16_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value);
    bytes.at(offset + 1) = static_cast<std::uint8_t>(value >> 8);
  }
  void dword(std::uint32_t offset, std::uint32_t value) {
    word(offset, static_cast<std::uint16_t>(value));
    word(offset + 2, static_cast<std::uint16_t>(value >> 16));
  }
  void text(std::uint32_t offset, const char* value) { std::memcpy(bytes.data() + offset, value, std::strlen(value) + 1); }
  void section(std::uint32_t index, const char* name, std::uint32_t rva, std::uint32_t size, std::uint32_t flags) {
    const auto offset = 0x188 + index * 40;
    std::memcpy(bytes.data() + offset, name, std::min<std::size_t>(8, std::strlen(name)));
    dword(offset + 8, size);
    dword(offset + 12, rva);
    dword(offset + 16, size);
    dword(offset + 20, 0x5000);  // Deliberately different FILE offset; parser must ignore it.
    dword(offset + 36, flags);
  }
};

SyntheticImage fixture() {
  SyntheticImage image;
  image.word(0, 0x5a4d);
  image.dword(60, 0x80);
  image.dword(0x80, 0x00004550);
  image.word(0x84, 0x8664);
  image.word(0x86, 3);
  image.dword(0x88, 0x12345678);
  image.word(0x94, 0xf0);
  image.word(0x98, 0x20b);
  image.dword(0xd0, 0x6000);
  image.dword(0xd4, 0x400);
  image.dword(0xd8, 0x87654321);
  image.dword(0x104, 16);
  image.section(0, ".text", 0x1000, 0x1000, 0x60000020);
  image.section(1, ".rdata", 0x2000, 0x2000, 0x40000040);
  image.section(2, ".data", 0x4000, 0x1000, 0xc0000040);
  image.text(0x1100, "WorldRenderContext");
  image.text(0x2400, "CameraToTexture::create");
  image.text(0x2460, "off_screen framebuffer");
  image.text(0x24c0, "Mirror::frame");
  image.text(0x253c, "camera manager across a reader boundary");
  image.text(0x4200, "CameraToTexture private runtime data must be excluded");
  return image;
}

void add_exports(SyntheticImage& image) {
  image.dword(0x108, 0x2000);
  image.dword(0x10c, 0x200);
  image.dword(0x2010, 10);
  image.dword(0x2014, 2);
  image.dword(0x2018, 2);
  image.dword(0x201c, 0x2050);
  image.dword(0x2020, 0x2060);
  image.dword(0x2024, 0x2070);
  image.dword(0x2050, 0x1100);
  image.dword(0x2054, 0x20d0);
  image.dword(0x2060, 0x2080);
  image.dword(0x2064, 0x20a0);
  image.word(0x2070, 0);
  image.word(0x2072, 1);
  image.text(0x2080, "CreateCamera");
  image.text(0x20a0, "RenderTargetExport");
  image.text(0x20d0, "engine.RenderTargetExport");
}

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool has(const Inventory& result, std::uint32_t rva, RecordKind kind = RecordKind::ascii_literal) {
  return std::any_of(result.records.begin(), result.records.end(),
                     [=](const Record& record) { return record.rva == rva && record.kind == kind; });
}

class SyntheticDecoder final : public InstructionDecoder {
 public:
  std::size_t decode(std::uint8_t* bytes, std::size_t available, std::uint32_t, std::string& instruction) override {
    if (available >= 7 && bytes[0] == 0x48 && bytes[1] == 0x8d) {
      instruction = "lea rcx, [rip + displacement]";
      return 7;
    }
    if (available >= 5 && bytes[0] == 0xe8) {
      instruction = "call target";
      return 5;
    }
    if (available != 0 && (bytes[0] == 0x90 || bytes[0] == 0xc3)) {
      instruction = bytes[0] == 0x90 ? "nop" : "ret";
      return 1;
    }
    return 0;
  }
};
}  // namespace

int main() {
  try {
    auto image = fixture();
    const auto normal = inspect_image(image);
    require(normal.valid_image && normal.machine == 0x8664 && normal.timestamp == 0x12345678 && normal.image_size == 0x6000 &&
                normal.checksum == 0x87654321,
            "Valid loaded PE identity failed");
    require(normal.records.size() == 5 && has(normal, 0x2400) && has(normal, 0x253c) && has(normal, 0x1100),
            "Literal matching/RVA/chunk carry failed");
    require(!has(normal, 0x4200), "Writable runtime data was inspected");

    add_exports(image);
    const auto exported = inspect_image(image);
    require(has(exported, 0x2080, RecordKind::export_name) && has(exported, 0x20a0, RecordKind::forwarded_export),
            "Named/forwarded exports failed");
    require(exported.records[0].ordinal == 10 && exported.records[1].function_rva == 0x20d0, "Export metadata incorrect");

    for (const auto offset : {0u, 0x80u, 0x84u, 0x98u}) {
      auto malformed = fixture();
      malformed.word(offset, 0);
      require(!inspect_image(malformed).valid_image, "Invalid PE signature/machine was accepted");
    }
    for (const auto value : {0u, 97u, 65535u}) {
      auto malformed = fixture();
      malformed.word(0x86, static_cast<std::uint16_t>(value));
      require(!inspect_image(malformed).valid_image, "Invalid section count was accepted");
    }
    for (const auto value : {0u, 0x80000001u, 0xffffffffu}) {
      auto malformed = fixture();
      malformed.dword(0xd0, value);
      require(!inspect_image(malformed).valid_image, "Invalid image size was accepted");
    }
    auto malformed = fixture();
    malformed.dword(60, 0xfffffff0);
    require(!inspect_image(malformed).valid_image, "Overflowed PE offset was accepted");
    malformed = fixture();
    malformed.word(0x94, 112);
    require(!inspect_image(malformed).valid_image, "Truncated export directory entry was accepted");
    malformed = fixture();
    malformed.dword(0x188 + 12, 0xfffff000);
    require(!inspect_image(malformed).valid_image, "Overflowed section RVA was accepted");
    malformed = fixture();
    malformed.dword(0x188 + 40 + 12, 0x1800);
    require(!inspect_image(malformed).valid_image, "Overlapping sections were accepted");
    malformed = fixture();
    malformed.bytes.resize(100);
    require(!inspect_image(malformed).valid_image, "Truncated image was accepted");

    malformed = image;
    malformed.word(0x2070, 200);
    require(inspect_image(malformed).malformed_exports == 1, "Invalid export ordinal was not rejected");
    malformed = image;
    malformed.dword(0x2018, 0xffffffff);
    require(inspect_image(malformed).malformed_exports == 1, "Overflowed export arrays were accepted");
    malformed = image;
    malformed.dword(0x2010, 0xffffffff);
    require(inspect_image(malformed).malformed_exports == 1, "Overflowed ordinal base was accepted");
    malformed = image;
    malformed.dword(0x2060, 0x4200);
    require(inspect_image(malformed).malformed_exports == 1, "Export name in writable runtime data was read");
    malformed = image;
    malformed.dword(0x2060, 0x3000);
    std::fill_n(malformed.bytes.data() + 0x3000, 512, static_cast<std::uint8_t>('R'));
    require(inspect_image(malformed).malformed_exports == 1, "An unterminated export name was accepted");
    malformed = image;
    malformed.dword(0x2050, 0xffffffff);
    require(inspect_image(malformed).malformed_exports == 1, "An export target outside the image was accepted");
    malformed = image;
    malformed.dword(0x2054, 0x21ff);
    malformed.bytes[0x21ff] = 'R';
    require(inspect_image(malformed).malformed_exports == 1, "A forwarder crossing the export-directory boundary was accepted");
    malformed = image;
    malformed.holes = {{0x2088, 0x2090}};
    require(!has(inspect_image(malformed), 0x2080, RecordKind::export_name), "Export across inaccessible page was read");
    malformed = fixture();
    malformed.holes = {{0, 64}};
    require(!inspect_image(malformed).valid_image, "Inaccessible DOS header was read");
    malformed = fixture();
    malformed.holes = {{0x2400, 0x2480}};
    const auto holes = inspect_image(malformed);
    require(holes.valid_image && holes.skipped_bytes == 128 && !has(holes, 0x2400) && has(holes, 0x24c0),
            "Unreadable region skipping failed");
    malformed = fixture();
    malformed.no_progress = true;
    require(inspect_image(malformed).read_failures != 0, "Zero-length reader window failed to terminate");

    Limits limits;
    limits.scan_bytes = 1024;
    auto bounded = inspect_image(image, limits);
    require(bounded.scan_truncated && bounded.scanned_bytes + bounded.skipped_bytes <= 1024, "Scan budget exceeded");
    limits.scan_bytes = 0x1000;
    bounded = inspect_image(image, limits);
    require(bounded.scan_truncated && has(bounded, 0x2400) && !has(bounded, 0x1100),
            "Executable sections were scanned before embedded data under a byte budget");
    limits.scan_bytes = 16 * 1024 * 1024;
    limits.records = 1;
    bounded = inspect_image(image, limits);
    require(bounded.records.size() == 1 && bounded.record_limit_reached, "Record budget exceeded");
    limits.records = 128;
    limits.export_names = 1;
    require(inspect_image(image, limits).exports_truncated, "Export count budget not reported");
    limits.metadata_bytes = 0;
    bounded = inspect_image(image, limits);
    require(!bounded.valid_image && bounded.metadata_bytes == 0, "Metadata budget exceeded");
    limits = {};
    limits.string_bytes = 8;
    malformed = fixture();
    require(inspect_image(malformed, limits).records.empty(), "Overlong literal was emitted");
    auto reference_image = fixture();
    reference_image.dword(0x120, 0x2100);
    reference_image.dword(0x124, 12);
    reference_image.dword(0x2100, 0x1000);
    reference_image.dword(0x2104, 0x100e);
    reference_image.dword(0x2108, 0x2600);
    reference_image.bytes[0x1000] = 0x90;
    reference_image.bytes[0x1001] = 0x48;
    reference_image.bytes[0x1002] = 0x8d;
    reference_image.bytes[0x1003] = 0x0d;
    reference_image.dword(0x1004, 0x2400 - 0x1008);
    reference_image.bytes[0x1008] = 0xe8;
    reference_image.dword(0x1009, 0x1200 - 0x100d);
    reference_image.bytes[0x100d] = 0xc3;
    SyntheticDecoder decoder;
    const std::vector<LiteralTarget> targets{{"fixture", 0x2400, "CameraToTexture::create"}};
    auto references = find_literal_references(reference_image, inspect_image(reference_image), decoder, targets);
    require(references.valid_targets && references.error.empty() && references.references.size() == 1 &&
                references.references[0].instruction_rva == 0x1001 && references.references[0].context.size() == 4 &&
                references.references[0].context[2].direct_call_target_rva == 0x1200,
            "RIP-relative target, function-boundary validation or direct-call metadata failed");
    reference_image.dword(0x2100, 0x1002);
    references = find_literal_references(reference_image, inspect_image(reference_image), decoder, targets);
    require(references.references.empty(), "An instruction outside the function boundary was accepted");
    reference_image.text(0x2400, "Other text");
    references = find_literal_references(reference_image, inspect_image(reference_image), decoder, targets);
    require(!references.valid_targets && !references.error.empty(), "A stale literal RVA was accepted");

    // The function-context path reads only explicit runtime-function prefixes.
    reference_image.text(0x2400, "CameraToTexture::create");
    reference_image.dword(0x2100, 0x1000);
    const auto function_image = inspect_image(reference_image);
    auto contexts = inspect_function_contexts(reference_image, function_image, decoder, targets, {0x1001});
    require(contexts.valid_targets && !contexts.partial && contexts.error.empty() && contexts.functions.size() == 1 &&
                contexts.code_bytes == 14 && contexts.decoded_bytes == 14 && contexts.functions[0].bytes_read == 14 &&
                contexts.functions[0].requested_boundary_found && contexts.functions[0].function_begin_rva == 0x1000 &&
                contexts.functions[0].function_end_rva == 0x100e && contexts.functions[0].instructions.size() == 4 &&
                contexts.functions[0].instructions[2].direct_call_target_rva == 0x1200,
            "Explicit function prefix, instruction metadata or exact read bounds failed");
    reference_image.holes = {{0x100e, 0x2000}};
    contexts = inspect_function_contexts(reference_image, function_image, decoder, targets, {0x1001});
    require(!contexts.partial && contexts.code_bytes == 14, "The function reader accessed past its exclusive end");
    reference_image.holes = {{0x1008, 0x1009}};
    contexts = inspect_function_contexts(reference_image, function_image, decoder, targets, {0x1001});
    require(contexts.partial && contexts.read_failures == 1 && contexts.functions[0].bytes_read == 0 &&
                contexts.functions[0].instructions.empty(),
            "Unreadable function prefix was accepted");
    reference_image.holes.clear();
    contexts = inspect_function_contexts(reference_image, function_image, decoder, targets, {0x1001, 0x100e, 0x4000});
    require(contexts.partial && contexts.missing_function_bounds == 2 && contexts.functions.size() == 3 &&
                contexts.functions[0].requested_boundary_found && contexts.functions[1].instructions.empty() &&
                contexts.functions[2].instructions.empty(),
            "Missing bounds, writable target refusal or partial successful contexts failed");
    contexts = inspect_function_contexts(reference_image, function_image, decoder, targets, {0x1003});
    require(contexts.partial && !contexts.functions[0].requested_boundary_found && !contexts.functions[0].decode_failed,
            "An RVA in the middle of an instruction was represented as an instruction boundary");
    auto bad_function_image = function_image;
    bad_function_image.valid_image = false;
    require(inspect_function_contexts(reference_image, bad_function_image, decoder, targets, {0x1001}).functions.empty(),
            "An invalid image was accepted for a function read");
    contexts = inspect_function_contexts(reference_image, function_image, decoder, targets, {});
    require(contexts.partial && contexts.code_bytes == 0 && !contexts.error.empty(), "An empty function request was accepted");
    contexts = inspect_function_contexts(reference_image, function_image, decoder, targets, std::vector<std::uint32_t>(9, 0x1001));
    require(contexts.partial && contexts.code_bytes == 0 && !contexts.error.empty(), "An oversized function request was accepted");
    reference_image.text(0x2400, "Stale literal");
    contexts = inspect_function_contexts(reference_image, function_image, decoder, targets, {0x1001});
    require(!contexts.valid_targets && contexts.partial && contexts.code_bytes == 0 && contexts.functions.empty(),
            "The function reader accepted a stale literal");
    reference_image.text(0x2400, "CameraToTexture::create");
    reference_image.bytes[0x1008] = 0;
    contexts = inspect_function_contexts(reference_image, function_image, decoder, targets, {0x1001});
    require(contexts.partial && contexts.functions[0].decode_failed && contexts.functions[0].requested_boundary_found &&
                contexts.functions[0].decoded_bytes == 8 && contexts.functions[0].instructions.size() == 2,
            "Decoder failure did not retain and flag bounded partial metadata");
    reference_image.bytes[0x1008] = 0xe8;
    reference_image.dword(0x1009, 0x4200 - 0x100d);
    contexts = inspect_function_contexts(reference_image, function_image, decoder, targets, {0x1001});
    require(!contexts.partial && contexts.functions[0].instructions[2].direct_call_target_rva == 0,
            "A direct-call target in writable/non-executable data was emitted");
    reference_image.dword(0x2104, 0x1000);
    contexts = inspect_function_contexts(reference_image, function_image, decoder, targets, {0x1001});
    require(contexts.partial && !contexts.error.empty() && contexts.functions.empty(), "Malformed runtime-function bounds were accepted");
    reference_image.dword(0x2104, 0x100e);
    reference_image.section(0, ".text", 0x1000, 0x1000, 0xe0000020);
    contexts = inspect_function_contexts(reference_image, inspect_image(reference_image), decoder, targets, {0x1001});
    require(contexts.partial && contexts.functions.empty(), "A writable executable runtime-function section was accepted");

    auto long_function = fixture();
    long_function.section(0, ".text", 0x1000, 0x1800, 0x60000020);
    long_function.section(1, ".rdata", 0x3000, 0x1000, 0x40000040);
    long_function.dword(0x120, 0x3100);
    long_function.dword(0x124, 12);
    long_function.dword(0x3100, 0x1000);
    long_function.dword(0x3104, 0x2100);
    long_function.dword(0x3108, 0x3600);
    std::fill(long_function.bytes.begin() + 0x1000, long_function.bytes.begin() + 0x2100, 0x90);
    long_function.text(0x3400, "CameraToTexture::create");
    const auto long_image = inspect_image(long_function);
    const std::vector<LiteralTarget> long_targets{{"long-fixture", 0x3400, "CameraToTexture::create"}};
    require(long_image.valid_image, "Long-function fixture was invalid");
    long_function.holes = {{0x2000, 0x2100}};
    contexts = inspect_function_contexts(long_function, long_image, decoder, long_targets, {0x1000, 0x2080});
    require(contexts.partial && contexts.code_bytes == 8192 && contexts.read_failures == 0 && contexts.functions[0].truncated &&
                !contexts.functions[0].decode_failed && contexts.functions[0].requested_boundary_found &&
                contexts.functions[0].instructions.size() == 4096 && contexts.functions[1].truncated &&
                !contexts.functions[1].requested_boundary_found && !contexts.functions[1].error.empty(),
            "The 4096-byte function cap, outside-prefix request or truncation metadata failed");
    contexts = inspect_function_contexts(long_function, long_image, decoder, long_targets, std::vector<std::uint32_t>(8, 0x1000));
    require(contexts.partial && contexts.code_bytes == 32768 && contexts.decoded_bytes == 32768 && contexts.functions.size() == 8,
            "The aggregate 32768-byte function budget failed");
    long_function.holes.clear();
    constexpr std::uint32_t expanded_size = 5001;
    long_function.dword(0x3104, 0x1000 + expanded_size);
    std::fill(long_function.bytes.begin() + 0x1000, long_function.bytes.begin() + 0x1000 + expanded_size, 0x90);
    const auto expanded_image = inspect_image(long_function);
    require(expanded_image.valid_image, "Expanded function fixture was invalid");
    long_function.holes = {{0x1000 + expanded_size, 0x2800}};
    contexts = inspect_function_contexts(long_function, expanded_image, decoder, long_targets, {0x1000 + expanded_size - 1}, 16384);
    require(!contexts.partial && contexts.code_bytes == expanded_size && contexts.functions[0].bytes_read == expanded_size &&
                contexts.functions[0].decoded_bytes == expanded_size && contexts.functions[0].requested_boundary_found &&
                contexts.functions[0].instructions.size() == expanded_size && !contexts.functions[0].truncated,
            "A 16-KiB prefix did not capture the complete 5001-byte function or read past its boundary");
    contexts =
        inspect_function_contexts(long_function, expanded_image, decoder, long_targets, std::vector<std::uint32_t>(8, 0x1000), 16384);
    require(contexts.partial && contexts.code_bytes == 32768 && contexts.decoded_bytes == 32768 && contexts.functions.size() == 8 &&
                !contexts.functions[5].truncated && contexts.functions[5].bytes_read == expanded_size && contexts.functions[6].truncated &&
                contexts.functions[6].bytes_read == 32768 - 6 * expanded_size && contexts.functions[6].requested_boundary_found &&
                contexts.functions[7].truncated && contexts.functions[7].bytes_read == 0 && contexts.functions[7].instructions.empty() &&
                !contexts.functions[7].error.empty() && contexts.read_failures == 0,
            "Expanded prefixes exceeded the aggregate budget or lost partial/exhausted-request metadata");
    for (const auto invalid_limit : {0u, 16385u, 0xffffffffu}) {
      contexts = inspect_function_contexts(long_function, expanded_image, decoder, long_targets, {0x1000}, invalid_limit);
      require(contexts.partial && !contexts.error.empty() && contexts.code_bytes == 0 && contexts.functions.empty(),
              "An invalid function prefix limit was accepted");
    }
    contexts = inspect_function_contexts(long_function, expanded_image, decoder, long_targets, {0x1000}, 1);
    require(contexts.partial && contexts.code_bytes == 1 && contexts.functions[0].requested_boundary_found &&
                contexts.functions[0].truncated && contexts.functions[0].instructions.size() == 1,
            "The minimum one-byte function prefix failed");
    std::puts(
        "PASS: loaded PE/literal/export checks, reference boundaries, explicit function contexts, partial failures and bounded reads.");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
