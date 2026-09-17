#include "../../src/camera/camera_contract.hpp"
#include "../../src/camera/camera_contract_model.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

namespace {
using namespace taxi_camera;
using namespace native_camera;
namespace cm = camera_contract_model;
namespace rc = relocatable;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
struct RuntimeFunction {
  std::uint32_t begin, end, unwind;
};
struct Fixture : discovery::ImageReader {
  static constexpr std::uint32_t Code = 0x10000, Static = 0x2000000, Writable = 0x2400000, Pdata = 0x2800000, Extent = 0x2900000;
  static constexpr std::uint64_t Base = 0x178000000;
  std::uint32_t shift;
  std::uint64_t base;
  std::map<std::uint32_t, std::uint8_t> memory;
  std::map<std::uint32_t, unsigned> reads;
  std::vector<std::uint32_t> symbols;
  std::vector<RuntimeFunction> functions;
  discovery::Inventory image;
  CameraFunctions expected_functions;
  CameraImageLayout expected_layout;
  std::array<std::uint32_t, 6> vtables{}, locators{};
  std::uint32_t fail_at = UINT32_MAX, change_at = UINT32_MAX, change_after = 2;
  std::uint32_t trigger_at = UINT32_MAX;
  std::uint8_t change_xor = 1;
  std::uint32_t reads_count = 0;

  void bytes(std::uint32_t at, const std::uint8_t* data, std::size_t size) {
    require(std::uint64_t(at) + size <= Extent + shift, "Fixture write escaped the sparse image");
    for (std::size_t i = 0; i < size; ++i)
      memory[at + static_cast<std::uint32_t>(i)] = data[i];
  }
  void integer(std::uint32_t at, std::uint64_t value, unsigned size = 4) {
    for (unsigned i = 0; i < size; ++i)
      memory[at + i] = static_cast<std::uint8_t>(value >> (i * 8));
  }
  std::uint32_t rva(const char* name) const {
    const auto index = cm::symbol_index(name);
    require(index < symbols.size(), "Required semantic role is absent from the generated model");
    return symbols[index];
  }
  explicit Fixture(std::uint32_t displacement = 0, std::uint64_t loaded_base = Base) : shift(displacement), base(loaded_base) {
    const auto& model = cm::model();
    symbols.resize(model.symbols.size());
    for (std::size_t i = 0; i < symbols.size(); ++i) {
      const auto kind = model.symbols[i].kind;
      symbols[i] = kind == rc::SectionKind::image_base       ? 0
                   : kind == rc::SectionKind::code           ? Code + shift + static_cast<std::uint32_t>(i) * 0x8000 + 0x2000
                   : kind == rc::SectionKind::read_only_data ? Static + shift + static_cast<std::uint32_t>(i) * 0x1000
                                                             : Writable + shift + static_cast<std::uint32_t>(i) * 0x1000;
    }
    // A short branch to a separately named basic block still needs a nearby
    // target. Its RVA is independent of the captured build, but constrained by
    // all incoming signed eight-bit displacements in the reviewed templates.
    std::map<std::uint32_t, std::pair<std::int64_t, std::int64_t>> short_targets;
    for (const auto& code : model.code)
      for (const auto& operand : code.operands) {
        if (operand.width != 1 || operand.target_symbol == code.symbol)
          continue;
        require(
            operand.kind == rc::AddressKind::pc_relative &&
                std::none_of(model.code.begin(), model.code.end(), [&](const auto& item) { return item.symbol == operand.target_symbol; }),
            "Synthetic short-target placement requires a separate basic block");
        const auto middle = std::int64_t(symbols[code.symbol]) + operand.pc_offset - operand.addend;
        auto [found, inserted] = short_targets.emplace(operand.target_symbol, std::pair{middle - 128, middle + 127});
        if (!inserted) {
          found->second.first = std::max(found->second.first, middle - 128);
          found->second.second = std::min(found->second.second, middle + 127);
        }
      }
    for (const auto& [symbol, interval] : short_targets) {
      require(interval.first <= interval.second && interval.first > 0 && interval.second <= UINT32_MAX,
              "Synthetic short branches have inconsistent target intervals");
      symbols[symbol] = static_cast<std::uint32_t>(interval.second);
    }
    for (const auto& code : model.code) {
      auto rewritten = code.bytes;
      for (const auto& operand : code.operands) {
        std::int64_t value = std::int64_t(symbols[operand.target_symbol]) + operand.addend;
        if (operand.kind == rc::AddressKind::pc_relative)
          value -= std::int64_t(symbols[code.symbol]) + operand.pc_offset;
        if (!(operand.width == 1 ? value >= -128 && value <= 127 : value >= INT32_MIN && value <= UINT32_MAX))
          throw std::runtime_error("Synthetic placement cannot encode " + model.symbols[code.symbol].name + " -> " +
                                   model.symbols[operand.target_symbol].name + " width " + std::to_string(operand.width) + " addend " +
                                   std::to_string(operand.addend) + " offset " + std::to_string(operand.pc_offset));
        for (unsigned byte = 0; byte < operand.width; ++byte)
          rewritten[operand.offset + byte] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) >> (8 * byte));
      }
      bytes(symbols[code.symbol], rewritten.data(), rewritten.size());
    }
    for (const auto& constant : model.constants)
      bytes(symbols[constant.symbol] + constant.offset, constant.bytes.data(), constant.bytes.size());
    for (const auto& pointer : model.pointers)
      integer(symbols[pointer.owner_symbol] + pointer.offset, base + symbols[pointer.target_symbol], 8);
    for (const auto& boundary : cm::boundaries()) {
      if (!boundary.required_pdata)
        continue;
      const auto begin = std::int64_t(symbols[boundary.symbol]) + boundary.begin_addend;
      require(begin >= Code + shift && begin + boundary.bytes < Static + shift, "Synthetic function escaped executable storage");
      functions.push_back({static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(begin + boundary.bytes), Pdata + shift + 0x2000});
    }
    std::sort(functions.begin(), functions.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
    for (std::size_t i = 1; i < functions.size(); ++i)
      require(functions[i - 1].end <= functions[i].begin, "Synthetic runtime-function records overlap");
    bytes(Pdata + shift, reinterpret_cast<const std::uint8_t*>(functions.data()), functions.size() * sizeof(RuntimeFunction));
    integer(Pdata + shift + 0x2000, 1);
    require(cm::bind(symbols, expected_functions, expected_layout), "Generated roles could not bind their synthetic symbols");
    vtables = {expected_layout.manager_vtable,           expected_layout.aircraft_facade_vtable, expected_layout.aircraft_controller_vtable,
               expected_layout.aircraft_selected_vtable, expected_layout.scene_node_vtable,      expected_layout.scene_model_vtable};
    require(std::all_of(vtables.begin(), vtables.end(), [](const auto value) { return value != 0; }),
            "The complete model omitted a required statically proven class identity");
    const std::array<std::uint32_t, 6> last_offsets{120, 1256, 432, 344, 0, 0};
    const std::array<std::uint32_t, 6> methods{expected_functions.manager_update,          expected_layout.aircraft_facade_method,
                                               expected_layout.aircraft_controller_method, expected_layout.aircraft_selected_method,
                                               expected_functions.initialize_descriptor,   expected_functions.initialize_descriptor};
    for (std::size_t i = 0; i < vtables.size(); ++i) {
      const auto metadata = Static + shift + 0x2e0000 + static_cast<std::uint32_t>(i) * 0x200;
      const auto type = Writable + shift + 0x2e0000 + static_cast<std::uint32_t>(i) * 0x100;
      locators[i] = metadata;
      integer(vtables[i] - 8, base + metadata, 8);
      if (!memory.contains(vtables[i]))
        integer(vtables[i], base + expected_functions.initialize_descriptor, 8);
      integer(vtables[i] + last_offsets[i], base + methods[i], 8);
      integer(metadata, 1);
      integer(metadata + 12, type);
      integer(metadata + 16, metadata + 32);
      integer(metadata + 20, metadata);
      integer(type, base + expected_functions.initialize_descriptor, 8);
      integer(metadata + 40, 1);
      integer(metadata + 44, metadata + 64);
      integer(metadata + 64, metadata + 80);
      integer(metadata + 80, type);
      integer(metadata + 92, UINT32_MAX);
      integer(metadata + 100, 64);
      integer(metadata + 104, metadata + 32);
    }
    image.valid_image = true;
    image.machine = 0x8664;
    image.timestamp = 0x8a765432;
    image.checksum = 0x1234abcd;
    image.image_size = Extent + shift;
    image.section_count = 4;
    image.exception_rva = Pdata + shift;
    image.exception_size = static_cast<std::uint32_t>(functions.size() * sizeof(RuntimeFunction));
    image.sections = {{".text", Code + shift, Static - Code, 0x60000020},
                      {".rdata", Static + shift, 0x300000, 0x40000040},
                      {".data", Writable + shift, 0x300000, 0xc0000040},
                      {".pdata", Pdata + shift, 0x10000, 0x40000040}};
    headers();
  }
  void headers() {
    integer(0, 0x5a4d, 2);
    integer(60, 0x80);
    integer(0x80, 0x4550);
    integer(0x84, image.machine, 2);
    integer(0x86, image.section_count, 2);
    integer(0x88, image.timestamp);
    integer(0x94, 0xf0, 2);
    integer(0x98, 0x20b, 2);
    integer(0xd0, image.image_size);
    integer(0xd4, 0x1000);
    integer(0xd8, image.checksum);
    integer(0x104, 16);
    integer(0x120, image.exception_rva);
    integer(0x124, image.exception_size);
    for (std::size_t i = 0; i < image.sections.size(); ++i) {
      const auto& s = image.sections[i];
      const auto at = 0x188 + static_cast<std::uint32_t>(i) * 40;
      bytes(at, reinterpret_cast<const std::uint8_t*>(s.name.data()), s.name.size());
      integer(at + 8, s.size);
      integer(at + 12, s.rva);
      integer(at + 36, s.flags);
    }
  }
  discovery::ReadWindow query(std::uint32_t at, std::uint32_t maximum) override {
    require(maximum <= 32768 && at < image.image_size && maximum <= image.image_size - at,
            "Wrapper query exceeded the bounded image window");
    return {maximum, true};
  }
  bool read(std::uint32_t at, void* destination, std::size_t size) override {
    require(size && size <= 32768 && std::uint64_t(at) + size <= image.image_size, "Wrapper issued an out-of-image/excessive read");
    ++reads_count;
    const auto count = ++reads[at];
    if (at == fail_at)
      return false;
    if (at == (trigger_at == UINT32_MAX ? change_at : trigger_at) && count == change_after)
      memory[change_at] ^= change_xor;
    std::memset(destination, 0, size);
    for (auto it = memory.lower_bound(at); it != memory.end() && std::uint64_t(it->first) < std::uint64_t(at) + size; ++it)
      static_cast<std::uint8_t*>(destination)[it->first - at] = it->second;
    return true;
  }
  CameraContractResolution run() { return resolve_camera_contract(*this, image, base); }
};
bool empty(const CameraContract& contract) {
  const CameraContract zero{};
  // Both public value structs contain only explicitly initialized uint32 fields.
  return !std::memcmp(&contract.functions, &zero.functions, sizeof(zero.functions)) &&
         !std::memcmp(&contract.layout, &zero.layout, sizeof(zero.layout));
}
void refused(const CameraContractResolution& result, const char* error) {
  require(!result.valid && empty(result.contract) && result.matched_ranges == 0 && !result.error.empty(), error);
}
void complete_resolution() {
  for (const auto shift : {0u, 0x1000u}) {
    Fixture fixture(shift, Fixture::Base + std::uint64_t(shift) * 0x40000);
    const auto result = fixture.run();
    require(result.valid && result.error.empty() && result.matched_ranges == cm::model().code.size(),
            "Complete coherent relocated camera contract did not resolve");
    require(!std::memcmp(&result.contract.functions, &fixture.expected_functions, sizeof(CameraFunctions)),
            "Resolved function roles differ from independent synthetic placement");
    require(result.contract.layout.manager_vtable == fixture.vtables[0] &&
                result.contract.layout.aircraft_facade_vtable == fixture.vtables[1] &&
                result.contract.layout.aircraft_controller_vtable == fixture.vtables[2] &&
                result.contract.layout.aircraft_selected_vtable == fixture.vtables[3] &&
                result.contract.layout.scene_node_vtable == fixture.vtables[4] &&
                result.contract.layout.scene_model_vtable == fixture.vtables[5],
            "Resolved static object identities did not follow the synthetic RTTI tables");
  }
}
void refusals() {
  const std::array<const char*, 8> stages{"Fresh main-image headers differ",
                                          "Instruction discovery refused:",
                                          "The discovered instructions do not match their reviewed function boundaries.",
                                          "The discovered instructions do not match their reviewed function boundaries.",
                                          "The discovered instructions do not match their reviewed function boundaries.",
                                          "The discovered instructions do not match their reviewed function boundaries.",
                                          "Code/relocation verification refused:",
                                          "Instruction discovery refused: code_changed_during_resolution"};
  for (unsigned scenario = 0; scenario < 8; ++scenario) {
    Fixture fixture;
    switch (scenario) {
      case 0:
        fixture.integer(0x88, fixture.image.timestamp + 1);
        break;
      case 1:
        fixture.memory[fixture.rva("create_entry")] ^= 1;
        break;
      case 2:
        fixture.integer(fixture.image.exception_rva + 4, fixture.functions[0].end + 1);
        break;
      case 3:
        fixture.integer(fixture.image.exception_rva + 12, fixture.functions[0].begin);
        break;
      case 4:
        fixture.fail_at = fixture.image.exception_rva;
        break;
      case 5:
        fixture.change_at = fixture.image.exception_rva;
        fixture.change_after = 2;
        break;
      case 6: {
        const auto relocation = Fixture::Pdata + 0x3000;
        fixture.integer(0x130, relocation);
        fixture.integer(0x134, 12);
        const auto target = fixture.rva("create_entry");
        fixture.integer(relocation, target & ~4095u);
        fixture.integer(relocation + 4, 12);
        fixture.integer(relocation + 8, 0xa000 | (target & 4095), 2);
        break;
      }
      case 7:
        fixture.change_at = fixture.rva("create_entry");
        fixture.change_after = 2;
        break;
    }
    const auto result = fixture.run();
    refused(result, "Invalid or changing headers/instructions/pdata/relocations published addresses");
    require(result.error.starts_with(stages[scenario]), "Failure did not reach its intended validation stage");
    if (scenario == 1 || scenario == 7) {
      require(result.error.find("[image timestamp=") != std::string::npos &&
                  result.error.find(" size=") != std::string::npos &&
                  result.error.find(" checksum=") != std::string::npos &&
                  result.error.find(" sections=") != std::string::npos,
              "Instruction-discovery refusal omitted loaded-image PE identity");
    }
    if (fixture.change_at != UINT32_MAX)
      require(fixture.reads[fixture.change_at] >= fixture.change_after, "The intended evidence mutation was never exercised");
  }
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    Fixture fixture;
    if (scenario == 0)
      fixture.integer(fixture.locators[0] + 20, fixture.locators[0] + 4);
    else if (scenario == 1)
      fixture.integer(fixture.vtables[0], fixture.base + fixture.image.image_size + 8, 8);
    else
      fixture.integer(fixture.locators[0] + 40, 0);
    const auto bad_rtti = fixture.run();
    refused(bad_rtti, "Inconsistent primary RTTI published a contract");
    require(bad_rtti.error.starts_with("Static object identity verification refused:"), "RTTI refusal bypassed the intended wrapper stage");
  }
  for (const auto base : {std::uint64_t{0}, UINT64_MAX - Fixture::Extent + 1}) {
    Fixture invalid_base;
    invalid_base.base = base;
    refused(invalid_base.run(), "An invalid loaded-image base published addresses");
    require(invalid_base.reads_count == 0, "Invalid loaded-image arithmetic reached the reader");
  }
}
void evidence_rechecks() {
  Fixture baseline;
  require(baseline.run().valid, "Evidence-change tests require an initially complete contract");
  for (const auto address : {std::uint32_t{0}, baseline.rva("initialize_descriptor"), baseline.locators[0]}) {
    Fixture fixture;
    fixture.change_at = address;
    fixture.change_after = baseline.reads[address];
    require(fixture.change_after >= 2, "Accepted evidence was not rechecked before publication");
    refused(fixture.run(), "Changing accepted PE/code/RTTI evidence published addresses");
    require(fixture.reads[address] >= fixture.change_after, "The accepted-evidence mutation was never exercised");
  }
  for (const bool change_directory : {true, false}) {
    Fixture drift;
    const auto first_table = Fixture::Pdata + 0x3000, second_table = first_table + 0x100;
    const auto target = drift.rva("create_entry");
    drift.integer(0x130, first_table);
    drift.integer(0x134, 12);
    for (const auto table : {first_table, second_table}) {
      drift.integer(table, target & ~4095u);
      drift.integer(table + 4, 12);
    }
    drift.integer(first_table + 8, target & 4095, 2);
    drift.integer(second_table + 8, 0xa000 | (target & 4095), 2);
    // Change the directory pointer or an entry between independent code
    // batches. The new evidence relocates a range from the first batch.
    drift.trigger_at = 0x130;
    drift.change_at = change_directory ? 0x131 : first_table + 9;
    drift.change_xor = change_directory ? 1 : 0xa0;
    drift.change_after = 2;
    const auto result = drift.run();
    require(drift.reads[drift.trigger_at] >= 2, "The relocation-evidence transition was not exercised");
    refused(result, "Different relocation evidence across code batches published addresses");
  }
}
void constructor_pointer_relations() {
  const auto& model = cm::model();
  require(model.pointers.size() >= 3, "The complete model omitted required static getter relations");
  Fixture duplicate_getters;
  std::uint32_t duplicate = Fixture::Code + 0x1e00000;
  unsigned duplicated = 0;
  for (const auto& relation : model.pointers) {
    const auto code =
        std::find_if(model.code.begin(), model.code.end(), [&](const auto& item) { return item.symbol == relation.target_symbol; });
    require(code != model.code.end(), "Static method relation has no reviewed body");
    if (code->bytes.size() > 8 || !code->operands.empty())
      continue;
    for (unsigned copy = 0; copy < 2; ++copy) {
      duplicate_getters.bytes(duplicate, code->bytes.data(), code->bytes.size());
      duplicate += 0x1000;
      ++duplicated;
    }
  }
  require(duplicated >= 2, "Duplicate-getter regression requires an actual short getter relation");
  const auto resolved = duplicate_getters.run();
  require(resolved.valid && resolved.contract.layout.aircraft_selected_method == duplicate_getters.expected_layout.aircraft_selected_method,
          "Identical unrelated getters prevented constructor-bound method resolution");

  Fixture baseline;
  require(baseline.run().valid, "Static method-slot checks require an initially complete contract");
  for (const auto& relation : model.pointers) {
    const auto slot = baseline.symbols[relation.owner_symbol] + relation.offset;
    Fixture invalid;
    invalid.integer(slot, invalid.base + invalid.image.image_size, 8);
    refused(invalid.run(), "An out-of-image static method slot published addresses");
    Fixture changed;
    changed.change_at = slot;
    changed.change_after = baseline.reads[slot];
    require(changed.change_after >= 2, "Accepted static method slot was not rechecked");
    refused(changed.run(), "Changing accepted static method slot published addresses");
    require(changed.reads[slot] >= changed.change_after, "Static method-slot mutation was never exercised");
  }
}
}  // namespace
int main() {
  try {
    complete_resolution();
    refusals();
    evidence_rechecks();
    constructor_pointer_relations();
    std::printf("Camera contract wrapper: PASS %u checks; sparse synthetic PE images only.\n", checks);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL camera contract wrapper after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
