#include "camera_contract.hpp"

#include "activation_mask.hpp"
#include "camera_contract_model.hpp"
#include "code_contract.hpp"
#include "rtti_vtables.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace taxi_camera::native_camera {
namespace {

class ContractReader final : public discovery::ImageReader {
 public:
  explicit ContractReader(discovery::ImageReader& source) : source_(source) {}
  discovery::ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    if (!maximum || ++queries_ > 131072)
      return {};
    const auto bounded = std::min<std::uint32_t>(maximum, 32768);
    const auto result = source_.query(rva, bounded);
    return result.size <= bounded ? result : discovery::ReadWindow{};
  }
  bool read(std::uint32_t rva, void* output, std::size_t size) override {
    constexpr std::uint64_t limit = 512ull * 1024 * 1024;
    if (!size || size > 32768 || ++reads_ > 131072 || size > limit - bytes_)
      return false;
    bytes_ += size;
    return source_.read(rva, output, size);
  }

 private:
  discovery::ImageReader& source_;
  std::uint64_t bytes_ = 0;
  std::uint32_t queries_ = 0, reads_ = 0;
};

bool exact(discovery::ImageReader& reader, std::uint32_t rva, void* output, std::uint32_t size) {
  std::uint32_t done = 0;
  while (done < size) {
    const auto window = reader.query(rva + done, size - done);
    if (!window.readable || !window.size || window.size > size - done ||
        !reader.read(rva + done, static_cast<std::uint8_t*>(output) + done, window.size))
      return false;
    done += window.size;
  }
  return true;
}

bool section(const discovery::Inventory& image, std::uint32_t rva, std::uint32_t size, bool code) {
  return size && rva < image.image_size && size <= image.image_size - rva &&
         std::any_of(
             image.sections.begin(), image.sections.end(),
             [&](const auto& item) {
               const auto required = code ? 0x60000000u : 0x40000000u;
               const auto excluded = code ? 0x82000000u : 0xa2000000u;
               return (item.flags & required) == required && !(item.flags & excluded) && rva >= item.rva &&
                      std::uint64_t(rva) + size <= std::uint64_t(item.rva) + item.size;
             });
}

bool same_image(const discovery::Inventory& a, const discovery::Inventory& b) {
  if (!a.valid_image || !b.valid_image || a.machine != 0x8664 || b.machine != a.machine || a.timestamp != b.timestamp ||
      a.image_size != b.image_size || a.checksum != b.checksum || a.section_count != b.section_count ||
      a.sections.size() != a.section_count || b.sections.size() != b.section_count || a.exception_rva != b.exception_rva ||
      a.exception_size != b.exception_size || a.dll_characteristics != b.dll_characteristics)
    return false;
  for (std::size_t i = 0; i < a.sections.size(); ++i)
    if (a.sections[i].rva != b.sections[i].rva || a.sections[i].size != b.sections[i].size || a.sections[i].flags != b.sections[i].flags)
      return false;
  return true;
}

bool relocation_bytes(discovery::ImageReader& reader,
                      const discovery::Inventory& image,
                      const CodeContractInventory& metadata,
                      std::vector<std::uint8_t>& bytes) {
  if (!metadata.relocation_rva && !metadata.relocation_size) {
    bytes.clear();
    return true;
  }
  if (!metadata.relocation_rva || !metadata.relocation_size || metadata.relocation_size > 8 * 1024 * 1024 ||
      metadata.relocation_rva >= image.image_size || metadata.relocation_size > image.image_size - metadata.relocation_rva ||
      !std::any_of(image.sections.begin(), image.sections.end(), [&](const auto& item) {
        // The PE directory may be stored in an executable or discardable
        // section. It must remain within declared readable, nonwritable data.
        return (item.flags & 0x40000000u) && !(item.flags & 0x80000000u) && metadata.relocation_rva >= item.rva &&
               std::uint64_t(metadata.relocation_rva) + metadata.relocation_size <= std::uint64_t(item.rva) + item.size;
      }))
    return false;
  bytes.resize(metadata.relocation_size);
  return exact(reader, metadata.relocation_rva, bytes.data(), metadata.relocation_size);
}

struct RuntimeFunction {
  std::uint32_t begin = 0, end = 0, unwind = 0;
  bool operator==(const RuntimeFunction&) const = default;
};

bool boundaries(discovery::ImageReader& reader, const discovery::Inventory& image, const std::vector<std::uint32_t>& symbols) {
  if (!image.exception_size || image.exception_size % sizeof(RuntimeFunction) || image.exception_size > 8 * 1024 * 1024 ||
      !section(image, image.exception_rva, image.exception_size, false))
    return false;
  std::vector<RuntimeFunction> functions(image.exception_size / sizeof(RuntimeFunction));
  if (!exact(reader, image.exception_rva, functions.data(), image.exception_size))
    return false;
  std::uint32_t previous = 0;
  for (const auto& entry : functions) {
    if (entry.begin < previous || entry.end <= entry.begin || !section(image, entry.begin, entry.end - entry.begin, true) ||
        !section(image, entry.unwind, 4, false))
      return false;
    previous = entry.end;
  }
  for (const auto& expected : camera_contract_model::boundaries()) {
    if (expected.symbol >= symbols.size() || !expected.bytes)
      return false;
    const auto begin64 = std::int64_t(symbols[expected.symbol]) + expected.begin_addend;
    if (begin64 <= 0 || begin64 > UINT32_MAX || std::uint64_t(begin64) + expected.bytes > image.image_size)
      return false;
    const auto begin = static_cast<std::uint32_t>(begin64);
    auto found = std::lower_bound(functions.begin(), functions.end(), begin,
                                  [](const RuntimeFunction& entry, std::uint32_t value) { return entry.begin < value; });
    if (expected.required_pdata) {
      if (found == functions.end() || found->begin != begin || found->end != begin + expected.bytes)
        return false;
    } else {
      // Reviewed leaf entry points may have no unwind record. They must not
      // secretly be interior bytes of a different enclosing function.
      if (found != functions.begin() && (found - 1)->end > begin)
        return false;
      if (found != functions.end() && found->begin < begin + expected.bytes &&
          (found->begin != begin || found->end != begin + expected.bytes))
        return false;
    }
    if (found != functions.end() && found->begin == begin) {
      RuntimeFunction again;
      const auto index = static_cast<std::uint32_t>(found - functions.begin());
      if (!exact(reader, image.exception_rva + index * sizeof(RuntimeFunction), &again, sizeof(again)) || again != *found)
        return false;
    }
  }
  return true;
}

bool same_templates(discovery::ImageReader& reader,
                    const relocatable::ContractModel& model,
                    const std::vector<std::uint32_t>& symbols,
                    std::uint64_t loaded_image_base) {
  if (symbols.size() != model.symbols.size())
    return false;
  for (const auto& code : model.code) {
    if (code.symbol >= symbols.size())
      return false;
    std::vector<std::uint8_t> bytes(code.bytes.size()), variable(code.bytes.size(), 0);
    if (!exact(reader, symbols[code.symbol], bytes.data(), static_cast<std::uint32_t>(bytes.size())))
      return false;
    for (const auto& operand : code.operands) {
      if (operand.target_symbol >= symbols.size() || operand.offset > bytes.size() || operand.width > bytes.size() - operand.offset ||
          (operand.width != 1 && operand.width != 4))
        return false;
      std::uint32_t raw = 0;
      for (std::uint32_t n = 0; n < operand.width; ++n) {
        raw |= std::uint32_t(bytes[operand.offset + n]) << (n * 8);
        variable[operand.offset + n] = 1;
      }
      std::int64_t target = raw;
      if (operand.kind == relocatable::AddressKind::pc_relative) {
        const auto displacement = operand.width == 1 ? (raw < 128 ? std::int64_t(raw) : std::int64_t(raw) - 256)
                                                     : (raw < 0x80000000u ? std::int64_t(raw) : std::int64_t(raw) - 0x100000000ll);
        target = std::int64_t(symbols[code.symbol]) + operand.pc_offset + displacement;
      }
      if (target != std::int64_t(symbols[operand.target_symbol]) + operand.addend)
        return false;
    }
    for (std::size_t i = 0; i < bytes.size(); ++i)
      if (!variable[i] && bytes[i] != code.bytes[i])
        return false;
  }
  for (const auto& constant : model.constants) {
    if (constant.symbol >= symbols.size() || constant.offset > UINT32_MAX - symbols[constant.symbol])
      return false;
    std::vector<std::uint8_t> bytes(constant.bytes.size());
    if (!exact(reader, symbols[constant.symbol] + constant.offset, bytes.data(), static_cast<std::uint32_t>(bytes.size())) ||
        bytes != constant.bytes)
      return false;
  }
  for (const auto& pointer : model.pointers) {
    if (pointer.owner_symbol >= symbols.size() || pointer.target_symbol >= symbols.size() ||
        pointer.offset > UINT32_MAX - symbols[pointer.owner_symbol] || loaded_image_base > UINT64_MAX - symbols[pointer.target_symbol])
      return false;
    std::uint64_t observed = 0;
    if (!exact(reader, symbols[pointer.owner_symbol] + pointer.offset, &observed, sizeof(observed)) ||
        observed != loaded_image_base + symbols[pointer.target_symbol])
      return false;
  }
  return true;
}

}  // namespace

CameraContractResolution resolve_camera_contract(discovery::ImageReader& source,
                                                 const discovery::Inventory& image,
                                                 std::uint64_t loaded_image_base) {
  CameraContractResolution result;
  ContractReader reader(source);
  const auto fail = [&](std::string message) {
    result.error = std::move(message);
    result.contract = {};
    return result;
  };
  if (!loaded_image_base || !image.image_size || loaded_image_base > UINT64_MAX - image.image_size)
    return fail("The loaded main-image identity is invalid.");
  discovery::Limits limits;
  limits.scan_bytes = limits.export_names = limits.records = 0;
  limits.metadata_bytes = 8192;
  if (!same_image(image, discovery::inspect_image(reader, limits)))
    return fail("Fresh main-image headers differ from the selected image.");
  const auto& model = camera_contract_model::model();
  const auto resolved = relocatable::resolve_contract(reader, image, model, {}, loaded_image_base);
  result.scanned_bytes = resolved.scanned_bytes;
  if (!resolved.valid) {
    // PE fields annotate the refusal only. They are not an allowlist; a later
    // build may still match if reviewed instruction templates survive.
    return fail("Instruction discovery refused: " + resolved.error + " [image timestamp=" +
                std::to_string(image.timestamp) + " size=" + std::to_string(image.image_size) +
                " checksum=" + std::to_string(image.checksum) + " sections=" +
                std::to_string(image.section_count) + "]");
  }
  CameraContract contract;
  if (!camera_contract_model::bind(resolved.symbols, contract.functions, contract.layout))
    return fail("The resolved camera contract is incomplete.");
  std::vector<CodeRange> ranges;
  for (const auto& code : model.code)
    ranges.push_back({resolved.symbols[code.symbol], static_cast<std::uint32_t>(code.bytes.size())});
  if (ranges.empty() || ranges.size() > 64)
    return fail("The required instruction-range count is invalid.");
  auto ordered_ranges = ranges;
  std::sort(ordered_ranges.begin(), ordered_ranges.end(), [](const auto& a, const auto& b) { return a.rva < b.rva; });
  for (std::size_t i = 0; i < ordered_ranges.size(); ++i) {
    const auto& range = ordered_ranges[i];
    if (!range.size || range.size > 8192 || !section(image, range.rva, range.size, true) ||
        (i && std::uint64_t(ordered_ranges[i - 1].rva) + ordered_ranges[i - 1].size > range.rva))
      return fail("The resolved instruction ranges overlap or escape static executable storage.");
  }
  // Keep the existing independent PE/relocation parser and its per-batch caps.
  // Observed fingerprints are inspection evidence, never a build allowlist.
  const std::vector<CodeRange> first_batch(ranges.begin(), ranges.begin() + std::min<std::size_t>(ranges.size(), 32));
  const auto initial_relocations = inspect_code_contract(reader, image, first_batch);
  if (!initial_relocations.valid)
    return fail("Code/relocation verification refused: " + initial_relocations.error);
  std::vector<std::uint8_t> relocation_snapshot;
  if (!relocation_bytes(reader, image, initial_relocations, relocation_snapshot))
    return fail("The declared relocation metadata could not be captured within its bounds.");
  const auto same_relocations = [&](const CodeContractInventory& current) {
    return current.valid && current.relocation_rva == initial_relocations.relocation_rva &&
           current.relocation_size == initial_relocations.relocation_size;
  };
  // Every range is checked after the shared metadata snapshot. No batch may
  // accept a different directory, and its complete contents are rechecked below.
  for (std::size_t begin = 0; begin < ranges.size(); begin += 32) {
    const auto end = std::min(ranges.size(), begin + 32);
    const std::vector<CodeRange> batch(ranges.begin() + begin, ranges.begin() + end);
    const auto checked = inspect_code_contract(reader, image, batch);
    if (!checked.valid)
      return fail("Code/relocation verification refused: " + checked.error);
    if (!same_relocations(checked))
      return fail("The relocation directory changed between verification batches.");
  }
  if (!boundaries(reader, image, resolved.symbols))
    return fail("The discovered instructions do not match their reviewed function boundaries.");
  const auto& layout = contract.layout;
  if (!layout.scene_node_vtable || !layout.scene_model_vtable)
    return fail("The scene Node and model identities have not been resolved from verified code.");
  const std::vector<VtableRequest> requests{
      {"", 128, {{120, contract.functions.manager_update}}, layout.manager_vtable},
      {"", 1264, {{1256, layout.aircraft_facade_method}}, layout.aircraft_facade_vtable},
      {"", 440, {{432, layout.aircraft_controller_method}}, layout.aircraft_controller_vtable},
      {"", 352, {{344, layout.aircraft_selected_method}}, layout.aircraft_selected_vtable},
      {"", 8, {}, layout.scene_node_vtable},
      {"", 8, {}, layout.scene_model_vtable},
  };
  const auto identities = resolve_rtti_vtables(reader, image, loaded_image_base, requests);
  if (!identities.valid || identities.vtables.size() != requests.size())
    return fail("Static object identity verification refused: " + identities.error);
  contract.layout.manager_vtable = identities.vtables[0];
  contract.layout.aircraft_facade_vtable = identities.vtables[1];
  contract.layout.aircraft_controller_vtable = identities.vtables[2];
  contract.layout.aircraft_selected_vtable = identities.vtables[3];
  contract.layout.scene_node_vtable = identities.vtables[4];
  contract.layout.scene_model_vtable = identities.vtables[5];
  const auto mask = inspect_activation_disable_mask(reader, image, contract.layout);
  if (!mask.valid)
    return fail("The resolved activation mask is invalid: " + mask.error);
  const auto final_relocations = inspect_code_contract(reader, image, first_batch);
  std::vector<std::uint8_t> final_relocation_bytes;
  if (!same_relocations(final_relocations) || !relocation_bytes(reader, image, final_relocations, final_relocation_bytes) ||
      final_relocation_bytes != relocation_snapshot)
    return fail("The relocation directory or its contents changed during verification.");
  // This is the last code/data read: even the final relocation parser's code
  // observations must still agree with the reviewed instruction semantics.
  if (!same_image(image, discovery::inspect_image(reader, limits)) || !same_templates(reader, model, resolved.symbols, loaded_image_base))
    return fail("The loaded image or resolved instruction/data contract changed during verification.");
  result.valid = true;
  result.contract = contract;
  result.matched_ranges = static_cast<std::uint32_t>(ranges.size());
  return result;
}

}  // namespace taxi_camera::native_camera
