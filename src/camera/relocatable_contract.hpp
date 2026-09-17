#pragma once

#include "image_inventory.hpp"

namespace taxi_camera::native_camera::relocatable {

enum class SectionKind { code, read_only_data, writable_data, image_base };
struct Symbol {
  std::string name;
  SectionKind kind = SectionKind::code;
  std::uint32_t extent = 1;
};
enum class AddressKind { pc_relative, image_rva };
struct AddressOperand {
  std::uint32_t offset = 0;
  std::uint8_t width = 4;
  std::uint32_t pc_offset = 0;
  AddressKind kind = AddressKind::pc_relative;
  std::uint32_t target_symbol = 0;
  std::int32_t addend = 0;
};
struct CodeTemplate {
  std::uint32_t symbol = 0;
  std::vector<std::uint8_t> bytes;
  std::vector<AddressOperand> operands;
  // Explicit seven-byte discovery is for caller-verified structural roots.
  // The caller must independently prove the corresponding RTTI/vtable edge.
  std::uint8_t minimum_seed = 8;
};
struct DataConstant {
  std::uint32_t symbol = 0;
  std::uint32_t offset = 0;
  std::vector<std::uint8_t> bytes;
};
struct PointerRelation {
  std::uint32_t owner_symbol = 0, offset = 0, target_symbol = 0;
};
struct ContractModel {
  std::vector<Symbol> symbols;
  std::vector<CodeTemplate> code;
  std::vector<DataConstant> constants;
  std::vector<PointerRelation> pointers;
};
struct SymbolBinding {
  std::uint32_t symbol = 0, rva = 0;
};
struct ContractResolution {
  bool valid = false;
  std::string error;
  // Published only after every template and symbol has one consistent binding.
  std::vector<std::uint32_t> symbols;
  std::uint64_t scanned_bytes = 0, requested_bytes = 0;
  std::uint32_t read_calls = 0, candidate_count = 0;
};

// Matches complete, externally decoded instruction templates. All non-operand
// bytes remain exact, including member displacements, opcodes and registers.
// A decoded operand addresses target_symbol + addend. PC-relative fields are
// signed and based on the instruction-end pc_offset; image RVAs are unsigned.
// The caller, not this matcher, proves operand spans are actual decoded address
// operands and provides complete instruction/function boundaries.
//
// Scans all readable, executable, non-writable/non-discardable image sections,
// at most 192 MiB. A template normally needs an invariant seed of eight bytes
// to discover a location independently. An explicit minimum_seed of seven is
// supported only for models whose caller separately proves the corresponding
// static RTTI/vtable relationship before promotion. No smaller value is valid.
// Shorter templates require caller roots or another template's address operands.
// All declared symbols must resolve.
// Ambiguity, incomplete reads, invalid bounds and resource limits refuse the
// entire result. A discoverable template with no candidates refuses as
// template_not_found: <semantic names>. Accepted code and constants are read
// again before publication.
//
// Limits: 64 templates, 4096 symbols, 16 KiB per template, 256 KiB template bytes,
// 64 candidates per template, 65536 read calls, 512 MiB requested bytes. Constants
// are nonempty read-only data, at most 4096 bytes each and 64 KiB in aggregate.
// No process operations, writes, pointer following or native calls occur here.
//
// The supplied Inventory is a caller-owned metadata snapshot. Before promoting
// a result to a callable contract the caller must independently check fresh PE
// and relocation metadata, enclosing function boundaries, ABI/data layouts,
// image identity, engine lifetime and execution context. A successful match
// alone proves none of those prerequisites or resistance to later mutation.
// Optional roots are fixed caller-provided bindings, not additional trust
// established here. The caller must justify and recheck them independently,
// for example through a bounded read-only RTTI/vtable graph. Roots never skip
// template body checks; short root-bound templates are matched in full.
// Pointer relations follow at most 256 declared aligned eight-byte slots in
// already bound read-only image symbols. They bind only reviewed code-template
// symbols within this same loaded image; they never inspect heap objects.
// Their actual loaded_image_base must be supplied, nonzero and eight-byte aligned.
// All accepted slots are reread before publication. This proves address graph
// consistency, not a complete RTTI identity or virtual calling convention.
ContractResolution resolve_contract(discovery::ImageReader& reader,
                                    const discovery::Inventory& image,
                                    const ContractModel& model,
                                    const std::vector<SymbolBinding>& roots = {},
                                    std::uint64_t loaded_image_base = 0);

}  // namespace taxi_camera::native_camera::relocatable
