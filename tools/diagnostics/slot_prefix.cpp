#include "slot_prefix.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace taxi_camera::discovery {
namespace {

constexpr std::uint32_t kReadable = 0x40000000;
constexpr std::uint32_t kWritable = 0x80000000;
constexpr std::uint32_t kExecutable = 0x20000000;
constexpr std::uint32_t kDiscardable = 0x02000000;

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

const ImageSection* section_for(const Inventory& image,
                                std::uint32_t rva,
                                std::uint32_t size,
                                std::uint32_t required,
                                std::uint32_t excluded) {
  if (size == 0 || rva >= image.image_size || size > image.image_size - rva)
    return nullptr;
  for (const auto& section : image.sections) {
    if ((section.flags & required) == required && (section.flags & excluded) == 0 && rva >= section.rva &&
        rva - section.rva < section.size && size <= section.size - (rva - section.rva))
      return &section;
  }
  return nullptr;
}

const ImageSection* executable_section(const Inventory& image, std::uint32_t rva) {
  return section_for(image, rva, 1, kReadable | kExecutable, kWritable | kDiscardable);
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

std::uint64_t pointer_word(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (std::uint32_t index = 0; index < kSlotWordBytes; ++index)
    value |= std::uint64_t(bytes[index]) << (index * 8);
  return value;
}

std::uint32_t direct_call_target(const Inventory& image, const std::uint8_t* bytes, std::uint32_t rva) {
  std::uint32_t displacement = 0;
  for (std::uint32_t index = 0; index < 4; ++index)
    displacement |= std::uint32_t(bytes[index + 1]) << (index * 8);
  const auto signed_displacement = (displacement & 0x80000000u) != 0 ? std::int64_t(displacement) - 0x100000000LL : displacement;
  const auto target = std::int64_t(rva) + 5 + signed_displacement;
  if (target > 0 && target < image.image_size && executable_section(image, static_cast<std::uint32_t>(target)))
    return static_cast<std::uint32_t>(target);
  return 0;
}

SlotPrefix inspect_one(ImageReader& reader,
                       const Inventory& image,
                       InstructionDecoder& decoder,
                       std::uint64_t loaded_image_base,
                       const SlotPrefixRequest& request,
                       SlotPrefixInventory& result) {
  SlotPrefix entry;
  entry.proof_slot_rva = request.slot_rva;
  entry.entry_rva = request.entry_rva;
  entry.stop_reason = "proof_rejected";
  const auto* storage = executable_section(image, request.entry_rva);
  if (request.slot_rva % kSlotWordBytes != 0 || request.entry_rva == 0 || !storage ||
      !section_for(image, request.slot_rva, kSlotWordBytes, kReadable, kWritable | kExecutable | kDiscardable)) {
    entry.error = "An aligned static data slot and a readable, non-writable executable entry are required.";
    return entry;
  }
  if (!readable_range(reader, request.slot_rva, kSlotWordBytes)) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The fixed slot word is not in readable query windows.";
    return entry;
  }
  std::array<std::uint8_t, kSlotWordBytes> proof{};
  result.proof_bytes += kSlotWordBytes;
  if (!reader.read(request.slot_rva, proof.data(), proof.size())) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The fixed slot pointer could not be read exactly.";
    return entry;
  }
  const auto pointer = pointer_word(proof.data());
  if (pointer < loaded_image_base || pointer - loaded_image_base >= image.image_size || pointer - loaded_image_base != request.entry_rva) {
    entry.stop_reason = "proof_mismatch";
    entry.error = "The current slot pointer does not normalize to the requested entry in the selected image.";
    return entry;
  }

  const auto count = std::min(kSlotTargetPrefixBytes, storage->size - (entry.entry_rva - storage->rva));
  if (!readable_range(reader, entry.entry_rva, count)) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The bounded target prefix is not in readable query windows.";
    return entry;
  }
  std::array<std::uint8_t, kSlotTargetPrefixBytes> prefix{};
  entry.attempted_bytes = count;
  result.read_bytes += count;
  if (!reader.read(entry.entry_rva, prefix.data(), count)) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The bounded target prefix could not be read exactly.";
    return entry;
  }
  while (entry.decoded_bytes < count) {
    const auto offset = entry.decoded_bytes;
    const auto rva = entry.entry_rva + offset;
    const auto available = count - offset;
    std::string text;
    const auto size = decoder.decode(prefix.data() + offset, available, rva, text);
    if (size == 0 || size > 15 || size > available || text.empty() || text.size() > 512) {
      entry.stop_reason = "decode_failed";
      entry.error = "Instruction decoding failed within the bounded prefix; its last instruction may be incomplete.";
      return entry;
    }
    const auto target = prefix[offset] == 0xe8 && size == 5 ? direct_call_target(image, prefix.data() + offset, rva) : 0;
    entry.instructions.push_back({rva, std::move(text), target});
    entry.decoded_bytes += static_cast<std::uint32_t>(size);
    if ((prefix[offset] == 0xc3 && size == 1) || (prefix[offset] == 0xc2 && size == 3)) {
      entry.ret_observed = true;
      entry.stop_reason = "ret";
      return entry;
    }
  }
  entry.stop_reason = count == kSlotTargetPrefixBytes ? "prefix_limit" : "section_end";
  return entry;
}
}

SlotPrefixInventory inspect_slot_prefixes(ImageReader& reader,
                                          const Inventory& image,
                                          InstructionDecoder& decoder,
                                          std::uint64_t loaded_image_base,
                                          const std::vector<SlotPrefixRequest>& requests) {
  SlotPrefixInventory result;
  if (!image.valid_image || image.machine != 0x8664 || image.image_size == 0 || !valid_sections(image) || loaded_image_base == 0 ||
      loaded_image_base % kSlotWordBytes != 0 || image.image_size > std::numeric_limits<std::uint64_t>::max() - loaded_image_base) {
    result.error = "A validated AMD64 image, valid sections and aligned non-overflowing loaded-image bounds are required.";
    return result;
  }
  if (requests.empty() || requests.size() > kMaximumSlotRequests) {
    result.error = "Request one to eight explicit static vtable slots and expected entry RVAs.";
    return result;
  }
  result.valid = true;
  result.entries.reserve(requests.size());
  for (const auto& request : requests) {
    result.entries.push_back(inspect_one(reader, image, decoder, loaded_image_base, request, result));
    if (!result.entries.back().error.empty()) {
      result.valid = false;
      result.error = "One or more requested slot prefixes could not be verified.";
    }
  }
  return result;
}

}
