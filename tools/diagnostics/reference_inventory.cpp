#include "reference_inventory.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace taxi_camera::discovery {
namespace {

constexpr std::uint32_t kExceptionLimit = 8 * 1024 * 1024;
constexpr std::uint32_t kChunk = 32 * 1024;
constexpr std::uint32_t kCallRel32Bytes = 5;

std::uint32_t u32(const std::uint8_t* data) {
  return data[0] | (static_cast<std::uint32_t>(data[1]) << 8) | (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

bool section_contains(const ImageSection& section, std::uint32_t rva, std::uint64_t bytes) {
  return rva >= section.rva && rva <= section.rva + section.size && bytes <= section.rva + section.size - rva;
}

bool readable_static(const ImageSection& section) {
  return (section.flags & 0x40000000) != 0 && (section.flags & (0x80000000 | 0x02000000)) == 0;
}

bool executable_range(const Inventory& image, std::uint32_t rva, std::uint32_t bytes) {
  return std::any_of(image.sections.begin(), image.sections.end(), [&](const ImageSection& section) {
    return readable_static(section) && (section.flags & 0x20000000) != 0 && section_contains(section, rva, bytes);
  });
}

bool candidate_opcode(const std::uint8_t* instruction, std::uint32_t candidate_size) {
  return candidate_size == kCallRel32Bytes ? instruction[0] == 0xe8
                                           : (instruction[0] & 0xf8) == 0x48 && instruction[1] == 0x8d && (instruction[2] & 0xc7) == 0x05;
}

std::int64_t candidate_destination(const std::uint8_t* instruction, std::uint32_t candidate_size, std::uint32_t rva) {
  const auto displacement_offset = candidate_size == kCallRel32Bytes ? 1 : 3;
  return static_cast<std::int64_t>(rva) + candidate_size + static_cast<std::int32_t>(u32(instruction + displacement_offset));
}

struct RuntimeFunction {
  std::uint32_t begin;
  std::uint32_t end;
};

bool validate_literals(ImageReader& reader, const Inventory& image, const std::vector<LiteralTarget>& targets, std::string& error) {
  if (!image.valid_image || targets.empty() || targets.size() > kMaximumExplicitTargets) {
    error = "A validated image and between one and eight explicit literal targets are required.";
    return false;
  }
  for (const auto& target : targets) {
    const bool in_static_section = std::any_of(image.sections.begin(), image.sections.end(), [&](const ImageSection& section) {
      return readable_static(section) && section_contains(section, target.rva, target.expected_text.size() + 1);
    });
    if (!in_static_section || target.expected_text.empty() || target.expected_text.size() > 240) {
      error = "A literal target does not fit a declared read-only section.";
      return false;
    }
    std::vector<std::uint8_t> text(target.expected_text.size() + 1);
    if (!reader.read(target.rva, text.data(), text.size()) || text.back() != 0 ||
        std::memcmp(text.data(), target.expected_text.data(), target.expected_text.size()) != 0) {
      error = "A target literal no longer matches this image or is unreadable; stale RVAs were rejected.";
      return false;
    }
  }
  return true;
}

bool read_runtime_functions(ImageReader& reader,
                            const Inventory& image,
                            std::vector<RuntimeFunction>& functions,
                            std::string& error,
                            std::uint32_t& read_failures) {
  const bool table_in_static_section = std::any_of(image.sections.begin(), image.sections.end(), [&](const ImageSection& section) {
    return readable_static(section) && section_contains(section, image.exception_rva, image.exception_size);
  });
  if (!table_in_static_section || image.exception_size == 0 || image.exception_size > kExceptionLimit || image.exception_size % 12 != 0) {
    error = "A bounded AMD64 runtime-function table is required for instruction-boundary checks.";
    return false;
  }
  std::vector<std::uint8_t> table(image.exception_size);
  if (!reader.read(image.exception_rva, table.data(), table.size())) {
    error = "The runtime-function table is unreadable.";
    ++read_failures;
    return false;
  }
  functions.reserve(table.size() / 12);
  for (std::size_t i = 0; i < table.size(); i += 12) {
    const auto begin = u32(table.data() + i);
    const auto end = u32(table.data() + i + 4);
    const auto unwind = u32(table.data() + i + 8);
    if (begin == 0 && end == 0 && unwind == 0)
      continue;
    if (end <= begin || !executable_range(image, begin, end - begin) || unwind >= image.image_size || unwind == 0 ||
        (!functions.empty() && begin < functions.back().end)) {
      error = "The runtime-function table has invalid bounds, order or overlap.";
      return false;
    }
    functions.push_back({begin, end});
  }
  return true;
}

bool validate_boundary(ImageReader& reader,
                       InstructionDecoder& decoder,
                       const RuntimeFunction& function,
                       std::uint32_t candidate,
                       std::uint32_t candidate_size,
                       ReferenceInventory& result,
                       CodeReference& reference) {
  if (function.end - function.begin > kMaximumCandidateFunctionBytes) {
    return false;
  }
  std::vector<std::uint8_t> bytes(function.end - function.begin);
  if (!reader.read(function.begin, bytes.data(), bytes.size())) {
    ++result.read_failures;
    return false;
  }
  std::uint32_t offset = 0;
  bool found = false;
  std::vector<InstructionMetadata> preceding;
  std::size_t following = 0;
  while (offset < bytes.size()) {
    if (result.decoded_bytes >= kMaximumDecodedCodeBytes) {
      result.decode_limit_reached = true;
      return false;
    }
    std::string text;
    const auto available = std::min<std::size_t>(bytes.size() - offset, kMaximumDecodedCodeBytes - result.decoded_bytes);
    const auto size = decoder.decode(bytes.data() + offset, available, function.begin + offset, text);
    if (size == 0 || size > 15 || size > available) {
      if (available < bytes.size() - offset) {
        result.decode_limit_reached = true;
        return false;
      }
      return found;
    }
    result.decoded_bytes += size;
    const auto rva = function.begin + offset;
    if (!found && rva > candidate) {
      return false;
    }
    if (rva == candidate) {
      if (size != candidate_size) {
        return false;
      }
      const auto* function_read_instruction = bytes.data() + offset;
      if (!candidate_opcode(function_read_instruction, candidate_size) ||
          candidate_destination(function_read_instruction, candidate_size, rva) != reference.target_rva) {
        return false;
      }
      found = true;
      reference.context = preceding;
    }
    std::uint32_t call_target = 0;
    if (bytes[offset] == 0xe8 && size == 5) {
      const auto destination = static_cast<std::int64_t>(rva) + 5 + static_cast<std::int32_t>(u32(bytes.data() + offset + 1));
      if (destination > 0 && destination <= std::numeric_limits<std::uint32_t>::max()) {
        call_target = static_cast<std::uint32_t>(destination);
      }
    }
    if (found) {
      reference.context.push_back({rva, text, call_target});
      if (++following == 8 || bytes[offset] == 0xc3 || bytes[offset] == 0xc2) {
        return true;
      }
    } else {
      if (preceding.size() == 6) {
        preceding.erase(preceding.begin());
      }
      preceding.push_back({rva, text, call_target});
    }
    offset += static_cast<std::uint32_t>(size);
  }
  return found;
}

ReferenceInventory scan_references(ImageReader& reader,
                                   const Inventory& image,
                                   InstructionDecoder& decoder,
                                   const std::vector<CodeTarget>& targets,
                                   std::uint32_t candidate_size) {
  ReferenceInventory result;
  result.valid_targets = true;
  std::vector<RuntimeFunction> functions;
  if (!read_runtime_functions(reader, image, functions, result.error, result.read_failures)) {
    return result;
  }
  result.runtime_function_count = static_cast<std::uint32_t>(functions.size());
  std::array<std::uint8_t, kChunk + 6> buffer{};
  for (const auto& section : image.sections) {
    if (!readable_static(section) || (section.flags & 0x20000000) == 0) {
      continue;
    }
    std::uint32_t offset = section.rva;
    while (offset < section.rva + section.size) {
      if (result.code_bytes >= kMaximumScannedCodeBytes) {
        result.code_limit_reached = true;
        return result;
      }
      const auto count = std::min<std::uint32_t>(
          {kChunk, section.rva + section.size - offset, static_cast<std::uint32_t>(kMaximumScannedCodeBytes - result.code_bytes)});
      const auto overlap = std::min<std::uint32_t>(candidate_size - 1, section.rva + section.size - offset - count);
      result.code_bytes += count;
      if (!reader.read(offset, buffer.data(), count + overlap)) {
        ++result.read_failures;
        offset += count;
        continue;
      }
      for (std::uint32_t i = 0; i < count && i + candidate_size <= count + overlap; ++i) {
        if (!candidate_opcode(buffer.data() + i, candidate_size)) {
          continue;
        }
        const auto instruction_rva = offset + i;
        const auto destination = candidate_destination(buffer.data() + i, candidate_size, instruction_rva);
        for (const auto& target : targets) {
          if (destination != target.rva) {
            continue;
          }
          if (result.candidate_count >= kMaximumReferenceCandidates || result.references.size() >= kMaximumCodeReferences) {
            result.reference_limit_reached = true;
            return result;
          }
          ++result.candidate_count;
          auto function = std::upper_bound(functions.begin(), functions.end(), instruction_rva,
                                           [](std::uint32_t rva, const RuntimeFunction& item) { return rva < item.begin; });
          if (function == functions.begin() || instruction_rva >= (--function)->end) {
            ++result.missing_function_bounds;
            continue;
          }
          CodeReference reference{target.label, target.rva, function->begin, function->end, instruction_rva, {}};
          if (validate_boundary(reader, decoder, *function, instruction_rva, candidate_size, result, reference)) {
            for (auto& instruction : reference.context) {
              if (instruction.direct_call_target_rva != 0 && !executable_range(image, instruction.direct_call_target_rva, 1)) {
                instruction.direct_call_target_rva = 0;
              }
            }
            result.references.push_back(std::move(reference));
          } else {
            ++result.failed_boundary_checks;
          }
          if (result.decode_limit_reached) {
            return result;
          }
        }
      }
      offset += count;
    }
  }
  return result;
}

}

ReferenceInventory find_literal_references(ImageReader& reader,
                                           const Inventory& image,
                                           InstructionDecoder& decoder,
                                           const std::vector<LiteralTarget>& targets) {
  ReferenceInventory result;
  if (!validate_literals(reader, image, targets, result.error)) {
    return result;
  }
  std::vector<CodeTarget> addresses;
  addresses.reserve(targets.size());
  for (const auto& target : targets) {
    addresses.push_back({target.label, target.rva});
  }
  return scan_references(reader, image, decoder, addresses, 7);
}

ReferenceInventory find_direct_call_references(ImageReader& reader,
                                               const Inventory& image,
                                               InstructionDecoder& decoder,
                                               const std::vector<LiteralTarget>& required_literals,
                                               const std::vector<CodeTarget>& targets) {
  ReferenceInventory result;
  if (targets.empty() || targets.size() > kMaximumExplicitTargets) {
    result.error = "Between one and eight explicit direct-call target RVAs are required.";
    return result;
  }
  if (!validate_literals(reader, image, required_literals, result.error)) {
    return result;
  }
  for (std::size_t i = 0; i < targets.size(); ++i) {
    const auto rva = targets[i].rva;
    if (rva == 0 || rva >= image.image_size || !executable_range(image, rva, 1)) {
      result.error = "A direct-call target is outside declared read-only, non-discardable executable image sections.";
      return result;
    }
    for (std::size_t previous = 0; previous < i; ++previous) {
      if (targets[previous].rva == rva) {
        result.error = "Direct-call target RVAs must be distinct.";
        return result;
      }
    }
  }
  return scan_references(reader, image, decoder, targets, 5);
}

FunctionInventory inspect_function_contexts(ImageReader& reader,
                                            const Inventory& image,
                                            InstructionDecoder& decoder,
                                            const std::vector<LiteralTarget>& required_literals,
                                            const std::vector<std::uint32_t>& instruction_rvas,
                                            std::uint32_t prefix_limit) {
  FunctionInventory result;
  if (prefix_limit == 0 || prefix_limit > kMaximumFunctionPrefixBytes) {
    result.error = "The function prefix limit must be between 1 and 16384 bytes.";
    result.partial = true;
    return result;
  }
  if (instruction_rvas.empty() || instruction_rvas.size() > kMaximumExplicitTargets) {
    result.error = "Between one and eight explicit instruction RVAs are required.";
    result.partial = true;
    return result;
  }
  if (!validate_literals(reader, image, required_literals, result.error)) {
    result.partial = true;
    return result;
  }
  result.valid_targets = true;
  std::vector<RuntimeFunction> functions;
  if (!read_runtime_functions(reader, image, functions, result.error, result.read_failures)) {
    result.partial = true;
    return result;
  }
  result.runtime_function_count = static_cast<std::uint32_t>(functions.size());
  for (const auto requested : instruction_rvas) {
    FunctionContext context;
    context.requested_rva = requested;
    auto function = std::upper_bound(functions.begin(), functions.end(), requested,
                                     [](std::uint32_t rva, const RuntimeFunction& item) { return rva < item.begin; });
    if (function == functions.begin() || requested >= (--function)->end) {
      ++result.missing_function_bounds;
      context.error = "The requested RVA has no validated static executable runtime-function bounds.";
    } else {
      context.function_begin_rva = function->begin;
      context.function_end_rva = function->end;
      const auto remaining = static_cast<std::uint32_t>(kFunctionContextBudgetBytes - result.code_bytes);
      const auto count = std::min<std::uint32_t>({prefix_limit, function->end - function->begin, remaining});
      context.truncated = count < function->end - function->begin;
      if (count == 0) {
        context.error = "The aggregate 32768-byte function budget is exhausted.";
        result.partial = true;
        if (result.error.empty())
          result.error = context.error;
        result.functions.push_back(std::move(context));
        continue;
      }
      std::vector<std::uint8_t> bytes(count);
      result.code_bytes += count;
      if (!reader.read(function->begin, bytes.data(), bytes.size())) {
        ++result.read_failures;
        context.error = "The bounded function prefix is unreadable.";
      } else {
        context.bytes_read = count;
        std::uint32_t offset = 0;
        while (offset < count) {
          std::string text;
          const auto rva = function->begin + offset;
          const auto size = decoder.decode(bytes.data() + offset, count - offset, rva, text);
          if (size == 0 || size > 15 || size > count - offset || text.size() > 512) {
            context.decode_failed = true;
            context.error = "Instruction decoding failed within the bounded prefix; at its cap an instruction may be incomplete.";
            break;
          }
          if (rva == requested)
            context.requested_boundary_found = true;
          std::uint32_t call_target = 0;
          if (bytes[offset] == 0xe8 && size == 5) {
            const auto destination = static_cast<std::int64_t>(rva) + 5 + static_cast<std::int32_t>(u32(bytes.data() + offset + 1));
            if (destination > 0 && destination <= std::numeric_limits<std::uint32_t>::max() &&
                executable_range(image, static_cast<std::uint32_t>(destination), 1)) {
              call_target = static_cast<std::uint32_t>(destination);
            }
          }
          context.instructions.push_back({rva, std::move(text), call_target});
          offset += static_cast<std::uint32_t>(size);
          context.decoded_bytes = offset;
          result.decoded_bytes += size;
        }
        if (!context.requested_boundary_found && context.error.empty()) {
          context.error = requested >= function->begin + count ? "The requested RVA lies beyond the bounded function prefix."
                                                               : "The requested RVA is not a decoded instruction boundary.";
        }
      }
    }
    if (context.truncated || !context.error.empty()) {
      result.partial = true;
      if (result.error.empty() && !context.error.empty())
        result.error = context.error;
    }
    result.functions.push_back(std::move(context));
  }
  return result;
}

}
