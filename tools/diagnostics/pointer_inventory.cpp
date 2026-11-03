#include "pointer_inventory.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace taxi_camera::discovery {
namespace {

constexpr std::uint32_t kReadable = 0x40000000;
constexpr std::uint32_t kWritable = 0x80000000;
constexpr std::uint32_t kExecutable = 0x20000000;
constexpr std::uint32_t kDiscardable = 0x02000000;

bool contains(const ImageSection& section, std::uint32_t rva, std::uint32_t size) {
  return rva >= section.rva && rva - section.rva <= section.size && size <= section.size - (rva - section.rva);
}

bool eligible_section(const ImageSection& section, bool code) {
  return (section.flags & kReadable) != 0 && (section.flags & (kWritable | kDiscardable)) == 0 &&
         ((section.flags & kExecutable) != 0) == code;
}

bool valid_sections(const Inventory& image) {
  if (image.sections.empty() || image.sections.size() > 96) {
    return false;
  }
  for (std::size_t index = 0; index < image.sections.size(); ++index) {
    const auto& section = image.sections[index];
    if (section.rva > image.image_size || section.size > image.image_size - section.rva) {
      return false;
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      const auto& other = image.sections[previous];
      if (section.size != 0 && other.size != 0 && section.rva < std::uint64_t(other.rva) + other.size &&
          other.rva < std::uint64_t(section.rva) + section.size) {
        return false;
      }
    }
  }
  return true;
}

bool readable_window(ImageReader& reader, std::uint32_t rva, std::uint32_t size) {
  for (std::uint32_t done = 0; done < size;) {
    const auto window = reader.query(rva + done, size - done);
    if (!window.readable || window.size == 0 || window.size > size - done) {
      return false;
    }
    done += window.size;
  }
  return true;
}

bool read_word(ImageReader& reader, std::uint32_t rva, PointerInventory& result, std::uint64_t& value) {
  if (!readable_window(reader, rva, kPointerWordBytes)) {
    ++result.read_failures;
    return false;
  }
  std::array<std::uint8_t, kPointerWordBytes> bytes{};
  result.read_bytes += kPointerWordBytes;
  if (!reader.read(rva, bytes.data(), bytes.size())) {
    ++result.read_failures;
    return false;
  }
  value = 0;
  for (std::uint32_t index = 0; index < kPointerWordBytes; ++index) {
    value |= std::uint64_t(bytes[index]) << (index * 8);
  }
  return true;
}

bool proven_readable_code(ImageReader& reader, const Inventory& image, std::uint32_t rva) {
  for (const auto& section : image.sections) {
    if (eligible_section(section, true) && contains(section, rva, 1) && readable_window(reader, rva, 1)) {
      return true;
    }
  }
  return false;
}

PointerEntry classify(ImageReader& reader,
                      const Inventory& image,
                      std::uint64_t base,
                      std::int32_t index,
                      std::uint32_t source_rva,
                      std::uint64_t value) {
  PointerEntry entry;
  entry.index = index;
  entry.source_rva = source_rva;
  if (value == 0) {
    return entry;
  }
  if (value < base || value - base >= image.image_size) {
    entry.category = PointerCategory::external;
    return entry;
  }
  entry.target_rva = static_cast<std::uint32_t>(value - base);
  entry.category = proven_readable_code(reader, image, entry.target_rva) ? PointerCategory::image_code : PointerCategory::image_data;
  return entry;
}

}

const char* pointer_category_name(PointerCategory category) {
  switch (category) {
    case PointerCategory::null_pointer:
      return "null";
    case PointerCategory::external:
      return "external";
    case PointerCategory::image_data:
      return "image_data";
    case PointerCategory::image_code:
      return "image_code";
  }
  return "unknown";
}

PointerInventory inspect_pointer_table(ImageReader& reader,
                                       const Inventory& image,
                                       std::uint64_t loaded_image_base,
                                       std::uint32_t table_rva,
                                       std::uint32_t max_entries,
                                       bool include_previous) {
  PointerInventory result;
  result.table_rva = table_rva;
  result.entry_limit = std::min(max_entries, kMaximumPointerTableEntries);
  if (!image.valid_image || image.machine != 0x8664 || image.image_size == 0 ||
      loaded_image_base > std::numeric_limits<std::uint64_t>::max() - image.image_size || !valid_sections(image)) {
    result.error = "Invalid loaded-image metadata or image-base bounds.";
    return result;
  }
  if (result.entry_limit == 0) {
    result.error = "The pointer entry limit must be nonzero.";
    return result;
  }
  const ImageSection* table_section = nullptr;
  for (const auto& section : image.sections) {
    if (eligible_section(section, false) && contains(section, table_rva, kPointerWordBytes)) {
      table_section = &section;
      break;
    }
  }
  if (!table_section) {
    result.error = "The table word is not wholly inside readable, non-writable static data.";
    return result;
  }
  result.valid_table = true;
  if (include_previous && table_rva >= kPointerWordBytes && contains(*table_section, table_rva - kPointerWordBytes, kPointerWordBytes)) {
    std::uint64_t value = 0;
    if (read_word(reader, table_rva - kPointerWordBytes, result, value)) {
      result.preceding_available = true;
      result.preceding = classify(reader, image, loaded_image_base, kPrecedingWordIndex, table_rva - kPointerWordBytes, value);
    }
  }
  for (std::uint32_t index = 0; index < result.entry_limit; ++index) {
    const auto offset = std::uint64_t(table_rva) + std::uint64_t(index) * kPointerWordBytes;
    if (offset > std::numeric_limits<std::uint32_t>::max() ||
        !contains(*table_section, static_cast<std::uint32_t>(offset), kPointerWordBytes)) {
      result.error = "The next table word crosses the static-data section boundary.";
      return result;
    }
    const auto source_rva = static_cast<std::uint32_t>(offset);
    std::uint64_t value = 0;
    if (!read_word(reader, source_rva, result, value)) {
      result.error = "A table word could not be read exactly.";
      return result;
    }
    result.entries.push_back(classify(reader, image, loaded_image_base, static_cast<std::int32_t>(index), source_rva, value));
    if (result.entries.back().category != PointerCategory::image_code) {
      result.stopped_on_non_code = true;
      return result;
    }
  }
  result.entry_limit_reached = true;
  return result;
}

}
