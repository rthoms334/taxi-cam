#include "../../src/camera/relocatable_contract.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
using namespace taxi_camera;
using namespace native_camera::relocatable;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
struct Fixture : discovery::ImageReader {
  enum : std::uint32_t { Root, Helper, Leaf, Text, Global, Base, Tail };
  discovery::Inventory image;
  ContractModel model;
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x20000, 0xcc);
  std::vector<std::uint32_t> locations = {0x2000, 0x4000, 0x5000, 0x11000, 0x18000, 0, 0x6000};
  std::uint32_t window = 32768, hole = 0, fail_at = 0, mutate_at = 0, mutate_offset = 0, seen = 0;
  bool invalid_window = false;
  Fixture() {
    image.valid_image = true;
    image.machine = 0x8664;
    image.image_size = static_cast<std::uint32_t>(bytes.size());
    image.sections = {
        {".text", 0x1000, 0xf000, 0x60000020}, {".rdata", 0x10000, 0x4000, 0x40000040}, {".data", 0x18000, 0x4000, 0xc0000040}};
    image.section_count = static_cast<std::uint16_t>(image.sections.size());
    model.symbols = {{"root", SectionKind::code, 68},
                     {"helper", SectionKind::code, 15},
                     {"leaf", SectionKind::code, 4},
                     {"text", SectionKind::read_only_data, 9},
                     {"global", SectionKind::writable_data, 8},
                     {"base", SectionKind::image_base, 0},
                     {"tail", SectionKind::code, 5}};
    CodeTemplate root;
    root.symbol = Root;
    root.bytes = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x20, 0x48, 0x89, 0xcb, 0xe8, 0, 0, 0, 0,  // call leaf
                  0x48, 0x8d, 0x05, 0,    0,    0,    0,                                               // lea text
                  0x48, 0x8b, 0x0d, 0,    0,    0,    0,                                               // mov global
                  0x48, 0x89, 0x41, 0x18,                                                              // member offset must remain exact
                  0xe8, 0,    0,    0,    0,                                                           // call helper
                  0x41, 0xb9, 0,    0,    0,    0,                                                     // image-relative integer
                  0x48, 0x8d, 0x15, 0,    0,    0,    0,                                               // lea image base
                  0xe8, 0,    0,    0,    0,                                                           // call tail
                  0x48, 0x8d, 0x35, 0,    0,    0,    0,                                               // repeated text reference
                  0x48, 0x83, 0xc4, 0x20, 0x5d, 0xc3};
    root.operands = {{12, 4, 16, AddressKind::pc_relative, Leaf, 0},   {19, 4, 23, AddressKind::pc_relative, Text, 0},
                     {26, 4, 30, AddressKind::pc_relative, Global, 0}, {35, 4, 39, AddressKind::pc_relative, Helper, 0},
                     {41, 4, 0, AddressKind::image_rva, Global, 0},    {48, 4, 52, AddressKind::pc_relative, Base, 0},
                     {53, 4, 57, AddressKind::pc_relative, Tail, 0},   {60, 4, 64, AddressKind::pc_relative, Text, 2}};
    model.symbols[Root].extent = static_cast<std::uint32_t>(root.bytes.size());
    model.code = {root,
                  {Helper, {0x48, 0x8b, 0x41, 0x08, 0x48, 0x8b, 0x50, 0x10, 0x48, 0x89, 0x51, 0x20, 0x33, 0xc0, 0xc3}, {}},
                  {Leaf, {0x89, 0x51, 0x18, 0xc3}, {}},
                  {Tail, {0xe9, 0, 0, 0, 0}, {{1, 4, 5, AddressKind::pc_relative, Helper, 0}}}};
    model.constants = {{Text, 0, {'C', 'a', 'm', 'e', 'r', 'a', '%', 'd', 0}}};
    populate();
  }
  void put(std::uint32_t at, std::uint32_t value, std::uint8_t width = 4) {
    for (std::uint8_t i = 0; i < width; ++i)
      bytes.at(at + i) = static_cast<std::uint8_t>(value >> (8 * i));
  }
  void place(std::size_t index, std::uint32_t at) {
    const auto& code = model.code[index];
    std::copy(code.bytes.begin(), code.bytes.end(), bytes.begin() + at);
    for (const auto& operand : code.operands) {
      const auto target = std::int64_t(locations[operand.target_symbol]) + operand.addend;
      const auto value = operand.kind == AddressKind::pc_relative ? target - at - operand.pc_offset : target;
      put(at + operand.offset, static_cast<std::uint32_t>(value), operand.width);
    }
  }
  void populate() {
    std::fill(bytes.begin(), bytes.end(), 0xcc);
    for (std::size_t i = 0; i < model.code.size(); ++i)
      place(i, locations[model.code[i].symbol]);
    for (const auto& constant : model.constants)
      std::copy(constant.bytes.begin(), constant.bytes.end(), bytes.begin() + locations[constant.symbol] + constant.offset);
  }
  discovery::ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    if (invalid_window)
      return {maximum + 1, true};
    if (rva >= bytes.size())
      return {};
    const auto count = std::min({maximum, window, static_cast<std::uint32_t>(bytes.size()) - rva});
    if (hole && rva <= hole && std::uint64_t(rva) + count > hole)
      return rva == hole ? discovery::ReadWindow{1, false} : discovery::ReadWindow{hole - rva, true};
    return {count, true};
  }
  bool read(std::uint32_t rva, void* out, std::size_t size) override {
    require(size > 0 && size <= 32768 && std::uint64_t(rva) + size <= bytes.size(), "Invalid resolver read extent");
    if (fail_at && rva <= fail_at && std::uint64_t(rva) + size > fail_at)
      return false;
    if (rva == mutate_at && ++seen == 2)
      bytes.at(rva + mutate_offset) ^= 1;
    std::memcpy(out, bytes.data() + rva, size);
    return true;
  }
  ContractResolution resolve() { return resolve_contract(*this, image, model); }
};
void refused(const ContractResolution& result, const char* message) {
  require(!result.valid && !result.error.empty() && result.symbols.empty(), message);
}
void relocation_and_edges() {
  for (const auto window : {17u, 4096u, 32768u}) {
    Fixture f;
    f.window = window;
    const auto result = f.resolve();
    require(result.valid && result.symbols == f.locations && result.scanned_bytes == 0xf000 && result.candidate_count == 4,
            "Baseline template/graph/constant resolution failed");
  }
  Fixture moved;
  moved.locations = {0x8ffc, 0x3219, 0x7141, 0x12f00, 0x19008, 0, 0x2fa0};
  moved.populate();
  const auto result = moved.resolve();
  require(result.valid && result.symbols == moved.locations, "Nonuniform moves or chunk-crossing seed failed");

  Fixture member;
  member.bytes[member.locations[Fixture::Root] + 33] = 0x20;
  refused(member.resolve(), "Changed member displacement was normalized");
  Fixture opcode;
  opcode.bytes[opcode.locations[Fixture::Root] + 18] ^= 8;
  refused(opcode.resolve(), "Changed register/opcode was normalized");
  Fixture repeated;
  repeated.put(repeated.locations[Fixture::Root] + 60, repeated.locations[Fixture::Text] + 3 - repeated.locations[Fixture::Root] - 64);
  refused(repeated.resolve(), "Repeated target inconsistency was accepted");
  Fixture helper;
  helper.bytes[helper.locations[Fixture::Helper] + 11] ^= 1;
  refused(helper.resolve(), "Changed dependent helper was accepted");
  Fixture tail;
  tail.put(tail.locations[Fixture::Tail] + 1, tail.locations[Fixture::Helper] + 1 - tail.locations[Fixture::Tail] - 5);
  refused(tail.resolve(), "Conflicting tail target was accepted");
  Fixture leaf;
  leaf.bytes[leaf.locations[Fixture::Leaf] + 2] ^= 1;
  refused(leaf.resolve(), "Changed inferred short setter was accepted");
  Fixture absent;
  std::fill_n(absent.bytes.begin() + absent.locations[Fixture::Root], absent.model.code[0].bytes.size(),
              static_cast<std::uint8_t>(0xcc));
  const auto missing = absent.resolve();
  refused(missing, "Missing discoverable template was accepted");
  require(missing.error.rfind("template_not_found:", 0) == 0 && missing.error.find("root") != std::string::npos,
          "Missing template omitted its semantic name");
}
void ambiguity_and_constants() {
  Fixture duplicate;
  duplicate.place(0, 0x7000);
  const auto ambiguous = duplicate.resolve();
  refused(ambiguous, "Ambiguous complete root adopted arbitrarily");
  require(ambiguous.error == "ambiguous_template: root (2 candidates)", "Ambiguity omitted the affected semantic template and count");
  Fixture disambiguated;
  disambiguated.locations[Fixture::Text] = 0x12000;
  disambiguated.place(0, 0x7000);
  disambiguated.locations[Fixture::Text] = 0x11000;
  const auto unique = disambiguated.resolve();
  require(unique.valid && unique.symbols == disambiguated.locations, "Known readonly literal failed to reject a false candidate");
  Fixture shared_helper;
  shared_helper.place(1, 0x7500);
  const auto shared = shared_helper.resolve();
  require(shared.valid && shared.symbols == shared_helper.locations, "Verified caller failed to disambiguate helper copies");
  Fixture constant;
  constant.bytes[constant.locations[Fixture::Text] + 4] ^= 1;
  refused(constant.resolve(), "Changed readonly constant was accepted");
  Fixture no_edge;
  no_edge.model.code[0].operands.erase(no_edge.model.code[0].operands.begin());
  // Make the leaf call bytes exact for this fixture, while withholding its
  // discovery edge. A coincidental short byte pattern must not seed a symbol.
  std::copy_n(no_edge.bytes.begin() + no_edge.locations[Fixture::Root] + 12, 4, no_edge.model.code[0].bytes.begin() + 12);
  refused(no_edge.resolve(), "Unreferenced short template was discovered by scanning");
  Fixture orphan;
  orphan.model.symbols.push_back({"orphan", SectionKind::read_only_data, 1});
  refused(orphan.resolve(), "Unbound non-code symbol escaped in valid result");
}
void model_and_section_failures() {
  for (unsigned test = 0; test < 28; ++test) {
    Fixture f;
    auto& operand = f.model.code[0].operands[0];
    switch (test) {
      case 0:
        operand.width = 2;
        break;
      case 1:
        operand.offset = 0xffffffff;
        break;
      case 2:
        operand.pc_offset = 15;
        break;
      case 3:
        operand.pc_offset = 10000;
        break;
      case 4:
        operand.target_symbol = 10000;
        break;
      case 5:
        operand.kind = static_cast<AddressKind>(99);
        break;
      case 6:
        f.model.code[0].operands.push_back(operand);
        break;
      case 7:
        f.model.code[0].bytes.clear();
        break;
      case 8:
        f.model.code[0].symbol = 10000;
        break;
      case 9:
        f.model.code.push_back(f.model.code[0]);
        break;
      case 10:
        f.model.symbols[0].extent = 1;
        break;
      case 11:
        f.model.symbols[0].kind = SectionKind::read_only_data;
        break;
      case 12:
        f.model.symbols[1].name = f.model.symbols[0].name;
        break;
      case 13:
        f.model.symbols[1].kind = static_cast<SectionKind>(99);
        break;
      case 14:
        f.model.constants[0].offset = 0xffffffff;
        break;
      case 15:
        f.model.constants[0].symbol = Fixture::Global;
        break;
      case 16:
        f.model.constants[0].bytes.clear();
        break;
      case 17:
        f.model.constants[0].symbol = 10000;
        break;
      case 18:
        f.image.valid_image = false;
        break;
      case 19:
        f.image.machine = 0x14c;
        break;
      case 20:
        f.image.section_count = 1;
        break;
      case 21:
        f.image.sections[1].rva = 0x8000;
        break;
      case 22:
        f.image.sections[0].size = 0xffffffff;
        break;
      case 23:
        f.image.sections[0].flags |= 0x80000000;
        break;
      case 24:
        f.image.sections[0].flags |= 0x02000000;
        break;
      case 25:
        f.model.symbols[Fixture::Base].extent = 2;
        break;
      case 26:
        f.model.symbols[0].extent = 0;
        break;
      case 27:
        f.model.code[0].operands[4].pc_offset = 45;
        break;
    }
    refused(f.resolve(), "Malformed model or sections were accepted");
  }
  Fixture wrong_section;
  wrong_section.locations[Fixture::Global] = 0x12000;
  wrong_section.populate();
  refused(wrong_section.resolve(), "Writable symbol rebound into readonly section");
  Fixture overflow;
  overflow.put(overflow.locations[Fixture::Root] + 41, 0xfffffffcu);
  refused(overflow.resolve(), "Out-of-image integer address was accepted");
  Fixture negative;
  negative.put(negative.locations[Fixture::Root] + 12, 0x80000000u);
  refused(negative.resolve(), "Negative PC-relative target wrapped to a valid symbol");
  Fixture addend;
  addend.model.code[0].operands[0].addend = std::numeric_limits<std::int32_t>::max();
  refused(addend.resolve(), "Negative symbol base after addend was accepted");
}
void short_fields_and_limits() {
  Fixture local;
  local.model.code[2] = {Fixture::Leaf, {0x75, 0, 0xc3}, {{1, 1, 2, AddressKind::pc_relative, Fixture::Leaf, 2}}};
  local.populate();
  require(local.resolve().valid, "Signed one-byte local branch did not bind its own function");
  local.model.code[2].operands[0].addend = 0;
  local.populate();
  require(local.resolve().valid, "Negative one-byte displacement did not sign extend");

  Fixture candidate_limit;
  for (unsigned i = 0; i < 65; ++i)
    candidate_limit.place(0, 0x8000 + i * 128);
  const auto candidates = candidate_limit.resolve();
  refused(candidates, "Candidate cap was silently truncated");
  require(candidates.error == "candidate_limit", "Candidate cap did not produce a bounded refusal");
  Fixture calls;
  calls.window = 1;
  calls.image.sections.push_back({".text2", 0x14000, 0x4000, 0x60000020});
  ++calls.image.section_count;
  const auto reads = calls.resolve();
  refused(reads, "Read-call cap was silently bypassed");
  require(reads.error == "read_call_limit" && reads.read_calls == 65536, "Read-call refusal had incorrect accounting");
  Fixture large_scan;
  large_scan.image.image_size = 0x10000000;
  large_scan.image.sections = {{".text", 0x1000, 193 * 1024 * 1024, 0x60000020}};
  large_scan.image.section_count = 1;
  const auto excessive = large_scan.resolve();
  refused(excessive, "Over-limit code inventory was partially scanned");
  require(excessive.read_calls == 0 && excessive.error == "code_scan_limit", "Code limit should refuse before reads");
}
void reads_and_consistency() {
  Fixture hole;
  hole.hole = 0xf010;
  refused(hole.resolve(), "Unreadable code after all candidates was silently ignored");
  Fixture failure;
  failure.fail_at = 0xf010;
  refused(failure.resolve(), "Read failure after all candidates was silently ignored");
  Fixture bad_window;
  bad_window.invalid_window = true;
  refused(bad_window.resolve(), "Invalid reader window was accepted");
  Fixture changing;
  changing.mutate_at = changing.locations[Fixture::Root];
  changing.mutate_offset = 33;
  refused(changing.resolve(), "Final reread accepted modified invariant bytes");
  Fixture changing_operand;
  changing_operand.mutate_at = changing_operand.locations[Fixture::Root];
  changing_operand.mutate_offset = 12;
  refused(changing_operand.resolve(), "Final reread silently adopted a different address operand");
  Fixture changing_constant;
  changing_constant.mutate_at = changing_constant.locations[Fixture::Text];
  changing_constant.mutate_offset = 4;
  refused(changing_constant.resolve(), "Final reread accepted changed readonly data");
}
void caller_roots() {
  Fixture f;
  // Remove the leaf's only code edge; a caller-verified vtable can now supply
  // its root, while its complete short body must still match independently.
  f.model.code[0].operands.erase(f.model.code[0].operands.begin());
  std::copy_n(f.bytes.begin() + f.locations[Fixture::Root] + 12, 4, f.model.code[0].bytes.begin() + 12);
  const std::vector<SymbolBinding> roots = {{Fixture::Leaf, f.locations[Fixture::Leaf]}};
  const auto resolved = resolve_contract(f, f.image, f.model, roots);
  require(resolved.valid && resolved.symbols == f.locations, "Caller-bound short function failed complete body matching");
  f.bytes[f.locations[Fixture::Leaf] + 2] ^= 1;
  refused(resolve_contract(f, f.image, f.model, roots), "Root bypassed short function body validation");
  for (unsigned test = 0; test < 6; ++test) {
    Fixture invalid;
    std::vector<SymbolBinding> bad;
    switch (test) {
      case 0:
        bad = {{10000, 0x5000}};
        break;
      case 1:
        bad = {{Fixture::Leaf, 0x11000}};
        break;
      case 2:
        bad = {{Fixture::Leaf, 0x5000}, {Fixture::Leaf, 0x5000}};
        break;
      case 3:
        bad = {{Fixture::Leaf, 0x5000}, {Fixture::Leaf, 0x5001}};
        break;
      case 4:
        bad = {{Fixture::Base, 1}};
        break;
      case 5:
        bad = {{Fixture::Root, 0x2001}};
        break;
    }
    refused(resolve_contract(invalid, invalid.image, invalid.model, bad), "Invalid or conflicting caller root was accepted");
  }
  Fixture data;
  refused(resolve_contract(data, data.image, data.model, {{Fixture::Text, 0x12000}}), "Caller data root bypassed known constant");
  Fixture duplicate;
  duplicate.place(0, 0x7000);
  const auto fixed = resolve_contract(duplicate, duplicate.image, duplicate.model, {{Fixture::Root, 0x2000}});
  require(fixed.valid && fixed.symbols == duplicate.locations, "Caller root did not constrain an otherwise ambiguous code domain");
}
void explicit_seven_byte_discovery() {
  const auto getter = static_cast<std::uint32_t>(Fixture::Tail + 1);
  Fixture f;
  f.model.symbols.push_back({"virtual_getter", SectionKind::code, 7});
  f.locations.push_back(0x8ffe);
  f.model.code.push_back({getter, {0x8b, 0x81, 0xa8, 0x06, 0, 0, 0xc3}, {}});
  f.populate();
  refused(f.resolve(), "Default seven-byte body was discovered without an edge or root");
  const auto rooted = resolve_contract(f, f.image, f.model, {{getter, f.locations[getter]}});
  require(rooted.valid && rooted.symbols == f.locations, "Default seven-byte body could not resolve through caller root");
  f.model.code.back().minimum_seed = 7;
  const auto explicit_seed = f.resolve();
  require(explicit_seed.valid && explicit_seed.symbols == f.locations, "Explicit unique seven-byte seed failed at a chunk boundary");
  f.place(f.model.code.size() - 1, 0x7200);
  refused(f.resolve(), "Explicit seven-byte discovery selected an ambiguous duplicate");
  f.populate();
  f.bytes[f.locations[getter] + 2] ^= 1;
  refused(f.resolve(), "Seven-byte discovery ignored an altered member displacement");
  for (const auto minimum : {0u, 6u, 9u, 255u}) {
    Fixture invalid;
    invalid.model.code[0].minimum_seed = static_cast<std::uint8_t>(minimum);
    const auto result = invalid.resolve();
    refused(result, "Unsupported minimum seed was accepted");
    require(result.read_calls == 0, "Invalid minimum seed reached the reader");
  }
}
struct PointerFixture : Fixture {
  static constexpr std::uint32_t Owner = 7, Getter = 8;
  static constexpr std::uint64_t BaseAddress = 0x140000000ull;
  PointerFixture() {
    model.symbols.push_back({"owner_vtable", SectionKind::read_only_data, 352});
    model.symbols.push_back({"virtual_method", SectionKind::code, 8});
    locations.push_back(0x12000);
    locations.push_back(0x7400);
    auto& code = model.code[0];
    code.bytes.insert(code.bytes.begin() + 64, {0x48, 0x8d, 0x0d, 0, 0, 0, 0});
    code.operands.push_back({67, 4, 71, AddressKind::pc_relative, Owner, 0});
    model.symbols[Root].extent = static_cast<std::uint32_t>(code.bytes.size());
    model.code.push_back({Getter, {0x48, 0x8b, 0x81, 0x88, 0x01, 0, 0, 0xc3}, {}});
    model.pointers.push_back({Owner, 344, Getter});
    populate();
    slot(BaseAddress + locations[Getter]);
  }
  void slot(std::uint64_t value) {
    put(locations[Owner] + 344, static_cast<std::uint32_t>(value));
    put(locations[Owner] + 348, static_cast<std::uint32_t>(value >> 32));
  }
  ContractResolution resolve() { return resolve_contract(*this, image, model, {}, BaseAddress); }
};
void pointer_relations() {
  PointerFixture f;
  f.place(f.model.code.size() - 1, 0x7600);
  const auto unique = f.resolve();
  require(unique.valid && unique.symbols == f.locations, "Constructor-bound static slot did not disambiguate duplicate getter bodies");
  f.model.pointers.clear();
  refused(f.resolve(), "Duplicate getter body lost ambiguity without pointer evidence");
  for (unsigned test = 0; test < 11; ++test) {
    PointerFixture invalid;
    switch (test) {
      case 0:
        invalid.model.pointers[0].owner_symbol = 10000;
        break;
      case 1:
        invalid.model.pointers[0].target_symbol = 10000;
        break;
      case 2:
        invalid.model.pointers[0].owner_symbol = Fixture::Global;
        break;
      case 3:
        invalid.model.pointers[0].target_symbol = Fixture::Text;
        break;
      case 4:
        invalid.model.pointers[0].offset = 345;
        break;
      case 5:
        invalid.model.pointers[0].offset = 352;
        break;
      case 6:
        invalid.model.pointers.push_back(invalid.model.pointers[0]);
        break;
      case 7:
        invalid.slot(PointerFixture::BaseAddress - 8);
        break;
      case 8:
        invalid.slot(PointerFixture::BaseAddress + invalid.image.image_size);
        break;
      case 9:
        invalid.slot(PointerFixture::BaseAddress + invalid.locations[Fixture::Text]);
        break;
      case 10:
        invalid.slot(PointerFixture::BaseAddress + invalid.locations[Fixture::Root]);
        break;
    }
    refused(invalid.resolve(), "Invalid pointer relation or target was accepted");
  }
  for (const std::uint64_t base : {std::uint64_t{0}, std::uint64_t{0x140000001ull}, UINT64_MAX - 7}) {
    PointerFixture invalid;
    const auto result = resolve_contract(invalid, invalid.image, invalid.model, {}, base);
    refused(result, "Invalid actual image base was accepted for a pointer relation");
    require(result.read_calls == 0, "Invalid image base reached the reader");
  }
  PointerFixture wrong_slot;
  wrong_slot.locations[PointerFixture::Owner] = 0x18000;
  wrong_slot.populate();
  wrong_slot.slot(PointerFixture::BaseAddress + wrong_slot.locations[PointerFixture::Getter]);
  refused(wrong_slot.resolve(), "Pointer owner was followed in writable data");
  PointerFixture unaligned;
  ++unaligned.locations[PointerFixture::Owner];
  unaligned.populate();
  unaligned.slot(PointerFixture::BaseAddress + unaligned.locations[PointerFixture::Getter]);
  refused(unaligned.resolve(), "Unaligned pointer owner produced an accepted slot");
  PointerFixture changed;
  changed.mutate_at = changed.locations[PointerFixture::Owner] + 344;
  changed.mutate_offset = 0;
  refused(changed.resolve(), "Changed slot was adopted during final reread");
  PointerFixture unreadable;
  unreadable.hole = unreadable.locations[PointerFixture::Owner] + 344;
  refused(unreadable.resolve(), "Unreadable static slot was ignored");
  PointerFixture ambiguous_owner;
  ambiguous_owner.locations[PointerFixture::Owner] = 0x13000;
  ambiguous_owner.place(0, 0x7200);
  ambiguous_owner.slot(PointerFixture::BaseAddress + ambiguous_owner.locations[PointerFixture::Getter]);
  refused(ambiguous_owner.resolve(), "Pointer relation arbitrarily selected between ambiguous constructor owners");
  PointerFixture root_owner;
  root_owner.model.code[0].operands.pop_back();
  std::copy_n(root_owner.bytes.begin() + root_owner.locations[Fixture::Root] + 67, 4, root_owner.model.code[0].bytes.begin() + 67);
  root_owner.model.code.back().bytes = {0x89, 0x51, 0x78, 0xc3};
  root_owner.populate();
  root_owner.slot(PointerFixture::BaseAddress + root_owner.locations[PointerFixture::Getter]);
  const auto rooted = resolve_contract(root_owner, root_owner.image, root_owner.model,
                                       {{PointerFixture::Owner, root_owner.locations[PointerFixture::Owner]}}, PointerFixture::BaseAddress);
  require(rooted.valid && rooted.symbols == root_owner.locations, "Verified owner root did not infer a complete short method body");
  root_owner.bytes[root_owner.locations[PointerFixture::Getter] + 2] ^= 1;
  refused(resolve_contract(root_owner, root_owner.image, root_owner.model,
                           {{PointerFixture::Owner, root_owner.locations[PointerFixture::Owner]}}, PointerFixture::BaseAddress),
          "Owner root bypassed inferred short method body validation");
  PointerFixture conflict;
  conflict.model.pointers.push_back({PointerFixture::Owner, 336, PointerFixture::Getter});
  conflict.put(conflict.locations[PointerFixture::Owner] + 336,
               static_cast<std::uint32_t>(PointerFixture::BaseAddress + conflict.locations[PointerFixture::Getter] + 1));
  conflict.put(conflict.locations[PointerFixture::Owner] + 340, static_cast<std::uint32_t>(PointerFixture::BaseAddress >> 32));
  refused(conflict.resolve(), "Inconsistent slots bound one method to two addresses");
}
}  // namespace

int main() {
  try {
    relocation_and_edges();
    ambiguity_and_constants();
    model_and_section_failures();
    short_fields_and_limits();
    reads_and_consistency();
    caller_roots();
    explicit_seven_byte_discovery();
    pointer_relations();
    std::printf("Relocatable contract: %u checks passed.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Relocatable contract check failed after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
