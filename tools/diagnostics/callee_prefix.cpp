#include "callee_prefix.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace taxi_camera::discovery {
namespace {

constexpr std::uint32_t kRequired = 0x40000000 | 0x20000000;
constexpr std::uint32_t kExcluded = 0x80000000 | 0x02000000;
constexpr std::uint8_t kCallRel32Opcode = 0xe8;
constexpr std::uint8_t kRetOpcode = 0xc3;
constexpr std::uint8_t kRetImm16Opcode = 0xc2;
constexpr std::uint32_t kRetImm16Bytes = 3;
constexpr std::uint8_t kJmpRel8Opcode = 0xeb;
constexpr std::uint8_t kJmpRel32Opcode = 0xe9;
constexpr std::uint8_t kJccRel8FirstOpcode = 0x70;
constexpr std::uint8_t kJccRel8LastOpcode = 0x7f;
constexpr std::uint8_t kTwoByteOpcodeEscape = 0x0f;
constexpr std::uint8_t kJccRel32FirstOpcode = 0x80;
constexpr std::uint8_t kJccRel32LastOpcode = 0x8f;
constexpr std::uint32_t kRel8BranchBytes = 2;
constexpr std::uint32_t kJmpRel32Bytes = 5;
constexpr std::uint32_t kJccRel32Bytes = kLongestBranchProofBytes;
constexpr std::uint32_t kMaximumX86InstructionBytes = 15;
constexpr std::uint32_t kMaximumInstructionTextBytes = 512;

bool valid_sections(const Inventory& image) {
  if (image.sections.empty() || image.sections.size() > 96)
    return false;
  for (std::size_t index = 0; index < image.sections.size(); ++index) {
    const auto& section = image.sections[index];
    if (section.rva > image.image_size || section.size > image.image_size - section.rva)
      return false;
    for (std::size_t previous = 0; previous < index; ++previous) {
      const auto& other = image.sections[previous];
      if (section.size != 0 && other.size != 0 && section.rva < std::uint64_t(other.rva) + other.size &&
          other.rva < std::uint64_t(section.rva) + section.size)
        return false;
    }
  }
  return true;
}

const ImageSection* executable_section(const Inventory& image, std::uint32_t rva, std::uint32_t size) {
  if (size == 0 || rva >= image.image_size || size > image.image_size - rva)
    return nullptr;
  for (const auto& section : image.sections) {
    if ((section.flags & kRequired) == kRequired && (section.flags & kExcluded) == 0 && rva >= section.rva &&
        rva - section.rva < section.size && size <= section.size - (rva - section.rva))
      return &section;
  }
  return nullptr;
}

bool proven_call(const FunctionInventory& proof, const CalleePrefixRequest& request) {
  for (const auto& function : proof.functions) {
    if (function.truncated || function.decode_failed || !function.error.empty() || function.function_begin_rva > request.call_rva ||
        function.function_end_rva <= request.call_rva || kCallProofBytes > function.function_end_rva - request.call_rva ||
        std::uint64_t(request.call_rva) + kCallProofBytes > std::uint64_t(function.function_begin_rva) + function.decoded_bytes)
      continue;
    for (const auto& instruction : function.instructions) {
      if (instruction.rva == request.call_rva && instruction.direct_call_target_rva == request.target_rva)
        return true;
    }
  }
  return false;
}

bool readable_range(ImageReader& reader, std::uint32_t rva, std::uint32_t size) {
  std::uint32_t checked = 0;
  while (checked < size) {
    const auto window = reader.query(rva + checked, size - checked);
    if (!window.readable || window.size == 0 || window.size > size - checked)
      return false;
    checked += window.size;
  }
  return true;
}

std::int64_t sign_extended(std::uint32_t value, std::uint32_t bits) {
  return (value & (std::uint32_t(1) << (bits - 1))) != 0 ? std::int64_t(value) - (std::int64_t(1) << bits) : std::int64_t(value);
}

std::int64_t call_target(const std::uint8_t* bytes, std::uint32_t call_rva) {
  std::uint32_t displacement = 0;
  for (std::uint32_t index = 0; index < 4; ++index)
    displacement |= std::uint32_t(bytes[index + 1]) << (8 * index);
  return std::int64_t(call_rva) + kCallProofBytes + sign_extended(displacement, 32);
}

template <typename Prefix, typename Result>
void inspect_target(ImageReader& reader,
                    const Inventory& image,
                    InstructionDecoder& decoder,
                    const ImageSection& storage,
                    Prefix& entry,
                    Result& result) {
  const auto count = std::min(kTargetPrefixBytes, storage.size - (entry.entry_rva - storage.rva));
  if (!readable_range(reader, entry.entry_rva, count)) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The bounded target prefix is not in readable query windows.";
    return;
  }
  std::array<std::uint8_t, kTargetPrefixBytes> prefix{};
  entry.attempted_bytes = count;
  result.read_bytes += count;
  if (!reader.read(entry.entry_rva, prefix.data(), count)) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The bounded target prefix could not be read exactly.";
    return;
  }

  while (entry.decoded_bytes < count) {
    const auto offset = entry.decoded_bytes;
    const auto rva = entry.entry_rva + offset;
    const auto available = count - offset;
    std::string text;
    const auto size = decoder.decode(prefix.data() + offset, available, rva, text);
    if (size == 0 || size > kMaximumX86InstructionBytes || size > available || text.size() > kMaximumInstructionTextBytes) {
      entry.stop_reason = "decode_failed";
      entry.error = "Instruction decoding failed within the bounded prefix; its last instruction may be incomplete.";
      return;
    }
    std::uint32_t target = 0;
    if (prefix[offset] == kCallRel32Opcode && size == kCallProofBytes) {
      const auto candidate = call_target(prefix.data() + offset, rva);
      if (candidate > 0 && candidate < image.image_size && executable_section(image, static_cast<std::uint32_t>(candidate), 1))
        target = static_cast<std::uint32_t>(candidate);
    }
    entry.instructions.push_back({rva, std::move(text), target});
    entry.decoded_bytes += static_cast<std::uint32_t>(size);
    if ((prefix[offset] == kRetOpcode && size == 1) || (prefix[offset] == kRetImm16Opcode && size == kRetImm16Bytes)) {
      entry.ret_observed = true;
      entry.stop_reason = "ret";
      return;
    }
  }
  entry.stop_reason = count == kTargetPrefixBytes ? "prefix_limit" : "section_end";
}

CalleePrefix inspect_one(ImageReader& reader,
                         const Inventory& image,
                         InstructionDecoder& decoder,
                         const FunctionInventory& proof,
                         const CalleePrefixRequest& request,
                         CalleePrefixInventory& result) {
  CalleePrefix entry;
  entry.proof_call_rva = request.call_rva;
  entry.entry_rva = request.target_rva;
  entry.stop_reason = "proof_rejected";
  const auto* storage = executable_section(image, request.target_rva, 1);
  if (request.target_rva == 0 || !storage || !executable_section(image, request.call_rva, kCallProofBytes)) {
    entry.error = "The call or target is outside declared readable, non-writable executable image storage.";
    return entry;
  }
  if (!proven_call(proof, request)) {
    entry.error = "No exact decoded direct-call boundary proves this requested callee.";
    return entry;
  }
  if (!readable_range(reader, request.call_rva, kCallProofBytes)) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The proven call bytes are not in readable query windows.";
    return entry;
  }
  std::array<std::uint8_t, kCallProofBytes> call{};
  result.proof_bytes += kCallProofBytes;
  if (!reader.read(request.call_rva, call.data(), call.size())) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The proven call bytes could not be re-read exactly.";
    return entry;
  }
  if (call[0] != kCallRel32Opcode || call_target(call.data(), request.call_rva) != request.target_rva) {
    entry.stop_reason = "proof_mismatch";
    entry.error = "The current instruction is not an E8 rel32 call to the proved target.";
    return entry;
  }
  inspect_target(reader, image, decoder, *storage, entry, result);
  return entry;
}

template <typename Proof>
bool valid_prefix_proof(const Proof& proof, const Inventory& image, std::uint32_t maximum_proof_bytes) {
  if (!proof.valid || !proof.error.empty() || proof.read_failures != 0 || proof.entries.empty() ||
      proof.entries.size() > kMaximumPrefixRequests || proof.read_bytes > kMaximumTargetPrefixReadBytes ||
      proof.proof_bytes > maximum_proof_bytes)
    return false;
  for (const auto& entry : proof.entries) {
    if (!entry.error.empty() || entry.attempted_bytes == 0 || entry.attempted_bytes > kTargetPrefixBytes || entry.decoded_bytes == 0 ||
        entry.decoded_bytes > entry.attempted_bytes || entry.instructions.empty() || entry.instructions.size() > kTargetPrefixBytes ||
        !executable_section(image, entry.entry_rva, entry.attempted_bytes) || entry.instructions.front().rva != entry.entry_rva ||
        (entry.stop_reason != "ret" && entry.stop_reason != "prefix_limit" && entry.stop_reason != "section_end"))
      return false;
    const auto end = std::uint64_t(entry.entry_rva) + entry.decoded_bytes;
    for (std::size_t index = 0; index < entry.instructions.size(); ++index) {
      const auto& instruction = entry.instructions[index];
      const auto next = index + 1 == entry.instructions.size() ? end : entry.instructions[index + 1].rva;
      if (instruction.rva < entry.entry_rva || instruction.rva >= end || next <= instruction.rva || next > end ||
          next - instruction.rva > kMaximumX86InstructionBytes || instruction.text.empty() ||
          instruction.text.size() > kMaximumInstructionTextBytes)
        return false;
    }
  }
  return true;
}

template <typename Proof>
const InstructionMetadata* proven_branch(const Proof& proof, std::uint32_t rva, std::uint32_t& size) {
  for (const auto& entry : proof.entries) {
    for (std::size_t index = 0; index < entry.instructions.size(); ++index) {
      if (entry.instructions[index].rva != rva)
        continue;
      const auto next =
          index + 1 == entry.instructions.size() ? std::uint64_t(entry.entry_rva) + entry.decoded_bytes : entry.instructions[index + 1].rva;
      size = static_cast<std::uint32_t>(next - rva);
      return &entry.instructions[index];
    }
  }
  return nullptr;
}

bool unprefixed_relative_branch_reaches(const std::uint8_t* bytes, std::uint32_t size, std::uint32_t rva, std::uint32_t target) {
  std::uint32_t offset = 0;
  std::uint32_t displacement_bytes = 0;
  if (size == kRel8BranchBytes && (bytes[0] == kJmpRel8Opcode || (bytes[0] >= kJccRel8FirstOpcode && bytes[0] <= kJccRel8LastOpcode))) {
    offset = 1;
    displacement_bytes = 1;
  } else if (size == kJmpRel32Bytes && bytes[0] == kJmpRel32Opcode) {
    offset = 1;
    displacement_bytes = 4;
  } else if (size == kJccRel32Bytes && bytes[0] == kTwoByteOpcodeEscape && bytes[1] >= kJccRel32FirstOpcode &&
             bytes[1] <= kJccRel32LastOpcode) {
    offset = 2;
    displacement_bytes = 4;
  } else {
    return false;
  }
  std::uint32_t displacement = 0;
  for (std::uint32_t index = 0; index < displacement_bytes; ++index)
    displacement |= std::uint32_t(bytes[offset + index]) << (8 * index);
  return std::int64_t(rva) + size + sign_extended(displacement, 8 * displacement_bytes) == target;
}

template <typename Proof>
BranchPrefix inspect_branch(ImageReader& reader,
                            const Inventory& image,
                            InstructionDecoder& decoder,
                            const Proof& proof,
                            const BranchPrefixRequest& request,
                            BranchPrefixInventory& result) {
  BranchPrefix entry;
  entry.proof_branch_rva = request.proof_branch_rva;
  entry.entry_rva = request.entry_rva;
  entry.stop_reason = "proof_rejected";
  const auto* storage = executable_section(image, request.entry_rva, 1);
  std::uint32_t size = 0;
  const auto* instruction = proven_branch(proof, request.proof_branch_rva, size);
  if (request.entry_rva == 0 || !storage || !instruction ||
      (size != kRel8BranchBytes && size != kJmpRel32Bytes && size != kJccRel32Bytes) ||
      !executable_section(image, request.proof_branch_rva, size)) {
    entry.error = "No exact eligible decoded branch boundary and static executable target prove this request.";
    return entry;
  }
  if (!readable_range(reader, request.proof_branch_rva, size)) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The proven branch bytes are not in readable query windows.";
    return entry;
  }
  std::array<std::uint8_t, kLongestBranchProofBytes> bytes{};
  result.proof_bytes += size;
  if (!reader.read(request.proof_branch_rva, bytes.data(), size)) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The proven branch bytes could not be re-read exactly.";
    return entry;
  }
  std::string text;
  if (!unprefixed_relative_branch_reaches(bytes.data(), size, request.proof_branch_rva, request.entry_rva) ||
      decoder.decode(bytes.data(), size, request.proof_branch_rva, text) != size || text != instruction->text) {
    entry.stop_reason = "proof_mismatch";
    entry.error = "The current instruction does not match the proved relative branch to the requested target.";
    return entry;
  }
  inspect_target(reader, image, decoder, *storage, entry, result);
  return entry;
}

}

CalleePrefixInventory inspect_callee_prefixes(ImageReader& reader,
                                              const Inventory& image,
                                              InstructionDecoder& decoder,
                                              const FunctionInventory& proof,
                                              const std::vector<CalleePrefixRequest>& requests) {
  CalleePrefixInventory result;
  if (!image.valid_image || image.machine != 0x8664 || image.image_size == 0 || !valid_sections(image)) {
    result.error = "Invalid loaded-image or section metadata.";
    return result;
  }
  if (requests.empty() || requests.size() > kMaximumPrefixRequests) {
    result.error = "Request one to eight explicitly proved direct callees.";
    return result;
  }
  if (!proof.valid_targets || proof.partial || !proof.error.empty() || proof.read_failures != 0 || proof.functions.empty() ||
      proof.functions.size() > kMaximumPrefixRequests) {
    result.error = "Fresh, successful and non-partial function contexts are required as call-boundary proof.";
    return result;
  }
  result.valid = true;
  result.entries.reserve(requests.size());
  for (const auto& request : requests) {
    result.entries.push_back(inspect_one(reader, image, decoder, proof, request, result));
    if (!result.entries.back().error.empty()) {
      result.valid = false;
      result.error = "One or more requested callee prefixes could not be verified.";
    }
  }
  return result;
}

BranchPrefixInventory inspect_branch_prefixes(ImageReader& reader,
                                              const Inventory& image,
                                              InstructionDecoder& decoder,
                                              const CalleePrefixInventory& proof,
                                              const std::vector<BranchPrefixRequest>& requests) {
  BranchPrefixInventory result;
  if (!image.valid_image || image.machine != 0x8664 || image.image_size == 0 || !valid_sections(image)) {
    result.error = "Invalid loaded-image or section metadata.";
    return result;
  }
  if (requests.empty() || requests.size() > kMaximumPrefixRequests) {
    result.error = "Request one to eight explicit branch targets.";
    return result;
  }
  if (!valid_prefix_proof(proof, image, kMaximumCallProofReadBytes)) {
    result.error = "Fresh, successful callee prefixes are required as branch-boundary proof.";
    return result;
  }
  result.valid = true;
  result.entries.reserve(requests.size());
  for (const auto& request : requests) {
    result.entries.push_back(inspect_branch(reader, image, decoder, proof, request, result));
    if (!result.entries.back().error.empty()) {
      result.valid = false;
      result.error = "One or more requested branch prefixes could not be verified.";
    }
  }
  return result;
}

BranchPrefixInventory inspect_branch_prefixes(ImageReader& reader,
                                              const Inventory& image,
                                              InstructionDecoder& decoder,
                                              const BranchPrefixInventory& proof,
                                              const std::vector<BranchPrefixRequest>& requests) {
  BranchPrefixInventory result;
  if (!image.valid_image || image.machine != 0x8664 || image.image_size == 0 || !valid_sections(image)) {
    result.error = "Invalid loaded-image or section metadata.";
    return result;
  }
  if (requests.empty() || requests.size() > kMaximumPrefixRequests) {
    result.error = "Request one to eight explicit branch targets.";
    return result;
  }
  if (!valid_prefix_proof(proof, image, kMaximumBranchProofReadBytes)) {
    result.error = "Fresh, successful branch prefixes are required as branch-boundary proof.";
    return result;
  }
  result.valid = true;
  result.entries.reserve(requests.size());
  for (const auto& request : requests) {
    result.entries.push_back(inspect_branch(reader, image, decoder, proof, request, result));
    if (!result.entries.back().error.empty()) {
      result.valid = false;
      result.error = "One or more requested branch prefixes could not be verified.";
    }
  }
  return result;
}

}
