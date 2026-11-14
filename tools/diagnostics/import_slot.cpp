#include "import_slot.hpp"

#include <array>
#include <utility>

namespace taxi_camera::discovery {
namespace {

constexpr std::uint32_t kReadable = 0x40000000;
constexpr std::uint32_t kExecutable = 0x20000000;
constexpr std::uint16_t kAmd64Machine = 0x8664;
constexpr std::uint32_t kMaximumSections = 96;
constexpr std::uint32_t kMaximumImageSize = 0x80000000u;
constexpr std::uint32_t kThunkBytes = 8;
constexpr std::uint32_t kImportDescriptorBytes = 20;
constexpr std::uint32_t kDescriptorOriginalFirstThunkOffset = 0;
constexpr std::uint32_t kDescriptorTimeDateStampOffset = 4;
constexpr std::uint32_t kDescriptorForwarderChainOffset = 8;
constexpr std::uint32_t kDescriptorNameOffset = 12;
constexpr std::uint32_t kDescriptorFirstThunkOffset = 16;
constexpr std::uint64_t kOrdinalFlag = 1ull << 63;
constexpr std::uint64_t kOrdinalReservedBits = 0x7fffffffffff0000ull;
constexpr std::uint64_t kMaximumHintNameRva = 0x7fffffffull;
constexpr std::uint32_t kHintAndFirstNameByte = kImportNameHintBytes + 1;
constexpr std::uint32_t kDosHeaderBytes = 64;
constexpr std::uint16_t kDosSignature = 0x5a4d;
constexpr std::uint32_t kDosNewHeaderOffsetField = 0x3c;
constexpr std::uint32_t kMaximumNewHeaderOffset = 1024 * 1024;
constexpr std::uint32_t kPeSignatureAndCoffHeaderBytes = 24;
constexpr std::uint32_t kPeSignature = 0x4550;
constexpr std::uint32_t kCoffMachineOffset = 4;
constexpr std::uint32_t kCoffSectionCountOffset = 6;
constexpr std::uint32_t kCoffTimestampOffset = 8;
constexpr std::uint32_t kCoffOptionalHeaderSizeOffset = 20;
constexpr std::uint32_t kMaximumOptionalHeaderBytes = 4096;
constexpr std::uint32_t kSectionHeaderBytes = 40;
constexpr std::uint16_t kPe32PlusMagic = 0x20b;
constexpr std::uint32_t kOptionalImageSizeOffset = 56;
constexpr std::uint32_t kOptionalHeadersSizeOffset = 60;
constexpr std::uint32_t kOptionalChecksumOffset = 64;
constexpr std::uint32_t kOptionalDataDirectoryCountOffset = 108;
constexpr std::uint32_t kOptionalDataDirectoriesOffset = 112;
constexpr std::uint32_t kDataDirectoryBytes = 8;
constexpr std::uint32_t kImportDirectoryIndex = 1;
constexpr std::uint32_t kIatDirectoryIndex = 12;

constexpr std::uint32_t data_directory_offset(std::uint32_t index) {
  return kOptionalDataDirectoriesOffset + index * kDataDirectoryBytes;
}

std::uint16_t u16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(bytes[0] | (std::uint16_t(bytes[1]) << 8));
}

std::uint32_t u32(const std::uint8_t* bytes) {
  return bytes[0] | (std::uint32_t(bytes[1]) << 8) | (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

std::uint64_t u64(const std::uint8_t* bytes) {
  return u32(bytes) | (std::uint64_t(u32(bytes + 4)) << 32);
}

bool within(std::uint64_t rva, std::uint64_t size, std::uint64_t begin, std::uint64_t extent) {
  return rva >= begin && rva - begin <= extent && size <= extent - (rva - begin);
}

bool overlap(std::uint64_t a, std::uint64_t a_size, std::uint64_t b, std::uint64_t b_size) {
  return a_size != 0 && b_size != 0 && a < b + b_size && b < a + a_size;
}

class Parser {
 public:
  Parser(ImageReader& reader, const Inventory& image, std::uint32_t slot) : reader_(reader), image_(image) { result_.iat_slot_rva = slot; }

  ImportSlotInventory run() {
    if (!validate_image() || !headers()) {
      return std::move(result_);
    }
    if (import_rva_ == 0 && import_size_ == 0) {
      unavailable("no_import_directory");
      return std::move(result_);
    }
    if (iat_rva_ == 0 && iat_size_ == 0) {
      unavailable("iat_extent_unavailable");
      return std::move(result_);
    }
    if (iat_rva_ == 0 || iat_size_ < kThunkBytes || iat_size_ % kThunkBytes != 0 || !data_section(iat_rva_, iat_size_)) {
      fail("The declared IAT extent is invalid or not readable non-executable image data.");
      return std::move(result_);
    }
    if (!within(result_.iat_slot_rva, kThunkBytes, iat_rva_, iat_size_) || (result_.iat_slot_rva - iat_rva_) % kThunkBytes != 0) {
      unavailable("slot_outside_iat");
      return std::move(result_);
    }
    if (import_rva_ == 0 || import_size_ < kImportDescriptorBytes || !data_section(import_rva_, import_size_) ||
        overlap(import_rva_, import_size_, iat_rva_, iat_size_)) {
      fail("The import directory is invalid or overlaps the excluded IAT.");
      return std::move(result_);
    }

    bool missing_lookup = false;
    bool thunk_limit = false;
    for (std::uint32_t descriptor_index = 0; descriptor_index < kMaximumImportDescriptors; ++descriptor_index) {
      const auto position = std::uint64_t(import_rva_) + descriptor_index * kImportDescriptorBytes;
      if (!within(position, kImportDescriptorBytes, import_rva_, import_size_)) {
        fail("The import directory ends before a null descriptor or matching slot.");
        return std::move(result_);
      }
      std::array<std::uint8_t, kImportDescriptorBytes> descriptor{};
      if (!data(position, descriptor.data(), descriptor.size())) {
        return std::move(result_);
      }
      ++result_.descriptors_read;
      const auto lookup = u32(descriptor.data() + kDescriptorOriginalFirstThunkOffset);
      const auto module = u32(descriptor.data() + kDescriptorNameOffset);
      const auto first = u32(descriptor.data() + kDescriptorFirstThunkOffset);
      if (lookup == 0 && u32(descriptor.data() + kDescriptorTimeDateStampOffset) == 0 &&
          u32(descriptor.data() + kDescriptorForwarderChainOffset) == 0 && module == 0 && first == 0) {
        unavailable(missing_lookup ? "lookup_table_absent" : thunk_limit ? "thunk_limit_reached" : "slot_not_found");
        return std::move(result_);
      }
      if (module == 0 || first == 0 || !within(first, kThunkBytes, iat_rva_, iat_size_) || (first - iat_rva_) % kThunkBytes != 0) {
        fail("An import descriptor has invalid module or IAT metadata.");
        return std::move(result_);
      }
      if (result_.iat_slot_rva < first) {
        continue;
      }
      const auto requested_index = (result_.iat_slot_rva - first) / kThunkBytes;
      if (requested_index >= kMaximumLookupThunksPerDescriptor) {
        thunk_limit = true;
        continue;
      }
      if (lookup == 0) {
        missing_lookup = true;
        continue;
      }
      for (std::uint32_t index = 0; index <= requested_index; ++index) {
        const auto thunk_rva = std::uint64_t(lookup) + std::uint64_t(index) * kThunkBytes;
        std::array<std::uint8_t, kThunkBytes> thunk{};
        if (!data(thunk_rva, thunk.data(), thunk.size())) {
          return std::move(result_);
        }
        ++result_.thunks_read;
        const auto value = u64(thunk.data());
        if (value == 0) {
          break;
        }
        const bool ordinal = (value & kOrdinalFlag) != 0;
        if ((ordinal && (value & kOrdinalReservedBits) != 0) || (!ordinal && value > kMaximumHintNameRva)) {
          fail("A lookup thunk has nonzero reserved bits; bound IAT addresses are not name RVAs.");
          return std::move(result_);
        }
        if (index != requested_index) {
          continue;
        }
        result_.descriptor_rva = static_cast<std::uint32_t>(position);
        result_.lookup_thunk_rva = static_cast<std::uint32_t>(thunk_rva);
        if (!literal(module, result_.module)) {
          return std::move(result_);
        }
        if (ordinal) {
          result_.by_ordinal = true;
          result_.ordinal = static_cast<std::uint16_t>(value);
          unavailable("ordinal_only");
          return std::move(result_);
        }
        result_.hint_name_rva = static_cast<std::uint32_t>(value);
        if (!allowed_data(value, kHintAndFirstNameByte) || !literal(value + kImportNameHintBytes, result_.symbol)) {
          return std::move(result_);
        }
        result_.valid = true;
        result_.available = true;
        result_.status = "named_import";
        return std::move(result_);
      }
    }
    unavailable("descriptor_limit_reached");
    return std::move(result_);
  }

 private:
  bool fail(const char* message) {
    result_.valid = false;
    result_.available = false;
    result_.status = "invalid_metadata";
    result_.error = message;
    return false;
  }

  void unavailable(const char* status) {
    result_.valid = true;
    result_.available = false;
    result_.status = status;
  }

  bool validate_image() {
    if (!image_.valid_image || image_.machine != kAmd64Machine || image_.image_size == 0 || image_.image_size > kMaximumImageSize ||
        image_.sections.empty() || image_.sections.size() > image_.section_count || image_.section_count == 0 ||
        image_.section_count > kMaximumSections || !within(result_.iat_slot_rva, kThunkBytes, 0, image_.image_size)) {
      return fail("Invalid AMD64 loaded-image or requested slot metadata.");
    }
    for (std::size_t index = 0; index < image_.sections.size(); ++index) {
      const auto& section = image_.sections[index];
      if (!within(section.rva, section.size, 0, image_.image_size)) {
        return fail("An image section extends outside the validated image.");
      }
      if (section.size != 0 && section.rva < first_section_rva_) {
        first_section_rva_ = section.rva;
      }
      for (std::size_t previous = 0; previous < index; ++previous) {
        const auto& other = image_.sections[previous];
        if (overlap(section.rva, section.size, other.rva, other.size)) {
          return fail("Overlapping image sections are unsupported.");
        }
      }
    }
    return true;
  }

  const ImageSection* data_section(std::uint64_t rva, std::uint64_t size) const {
    if (size == 0 || !within(rva, size, 0, image_.image_size)) {
      return nullptr;
    }
    for (const auto& section : image_.sections) {
      if ((section.flags & kReadable) != 0 && (section.flags & kExecutable) == 0 && within(rva, size, section.rva, section.size)) {
        return &section;
      }
    }
    return nullptr;
  }

  bool allowed_data(std::uint64_t rva, std::uint32_t size) {
    if (!data_section(rva, size) || overlap(rva, size, iat_rva_, iat_size_)) {
      return fail("Import metadata is outside readable non-executable image data or overlaps the excluded IAT.");
    }
    return true;
  }

  bool exact(std::uint64_t rva, void* output, std::uint32_t size) {
    if (overlap(rva, size, result_.iat_slot_rva, kThunkBytes)) {
      return fail("A metadata request overlaps the excluded requested IAT slot.");
    }
    if (!within(rva, size, 0, image_.image_size)) {
      return fail("A metadata request is outside the validated image.");
    }
    if (size > kMaximumImportMetadataReadBytes - result_.read_bytes) {
      result_.status = "byte_limit_reached";
      result_.error = "The import metadata read allowance is exhausted.";
      return false;
    }
    result_.read_bytes += size;
    std::uint32_t offset = 0;
    while (offset < size) {
      const auto next = static_cast<std::uint32_t>(rva + offset);
      const auto window = reader_.query(next, size - offset);
      if (!window.readable || window.size == 0 || window.size > size - offset) {
        ++result_.read_failures;
        return fail("An import metadata query window is unreadable or invalid.");
      }
      if (!reader_.read(next, static_cast<std::uint8_t*>(output) + offset, window.size)) {
        ++result_.read_failures;
        return fail("Import metadata could not be read exactly.");
      }
      offset += window.size;
    }
    return true;
  }

  bool data(std::uint64_t rva, void* output, std::uint32_t size) { return allowed_data(rva, size) && exact(rva, output, size); }

  bool header(std::uint64_t rva, void* output, std::uint32_t size) {
    if (!within(rva, size, 0, first_section_rva_)) {
      return fail("A PE header request reaches a declared image section.");
    }
    return exact(rva, output, size);
  }

  bool literal(std::uint64_t rva, std::string& output) {
    const auto* section = data_section(rva, 1);
    if (!section) {
      return fail("An import name begins outside readable non-executable image data.");
    }
    std::string value;
    for (std::uint32_t index = 0; index < kMaximumImportNameBytesIncludingNul; ++index) {
      if (!within(rva + index, 1, section->rva, section->size)) {
        return fail("An import name crosses its image section before NUL.");
      }
      std::uint8_t byte = 0;
      if (!data(rva + index, &byte, 1)) {
        return false;
      }
      if (byte == 0) {
        if (value.empty()) {
          return fail("An import name is empty.");
        }
        output = std::move(value);
        return true;
      }
      if (byte < 32 || byte > 126) {
        return fail("An import name is not printable ASCII.");
      }
      value.push_back(static_cast<char>(byte));
    }
    return fail("An import name is not NUL-terminated within 128 bytes.");
  }

  bool headers() {
    std::array<std::uint8_t, kDosHeaderBytes> dos{};
    if (!header(0, dos.data(), dos.size())) {
      return false;
    }
    const auto pe = u32(dos.data() + kDosNewHeaderOffsetField);
    if (u16(dos.data()) != kDosSignature || pe < kDosHeaderBytes || pe > kMaximumNewHeaderOffset) {
      return fail("Invalid DOS signature or PE header offset.");
    }
    std::array<std::uint8_t, kPeSignatureAndCoffHeaderBytes> coff{};
    if (!header(pe, coff.data(), coff.size())) {
      return false;
    }
    const auto optional_size = u16(coff.data() + kCoffOptionalHeaderSizeOffset);
    if (u32(coff.data()) != kPeSignature || u16(coff.data() + kCoffMachineOffset) != image_.machine ||
        u16(coff.data() + kCoffSectionCountOffset) != image_.section_count || u32(coff.data() + kCoffTimestampOffset) != image_.timestamp ||
        optional_size < kOptionalDataDirectoriesOffset || optional_size > kMaximumOptionalHeaderBytes) {
      return fail("The PE header no longer matches the validated image or has invalid bounds.");
    }
    const auto optional_rva = std::uint64_t(pe) + kPeSignatureAndCoffHeaderBytes;
    std::array<std::uint8_t, kOptionalDataDirectoriesOffset> optional{};
    if (!header(optional_rva, optional.data(), optional.size())) {
      return false;
    }
    const auto header_size = u32(optional.data() + kOptionalHeadersSizeOffset);
    const auto directories = u32(optional.data() + kOptionalDataDirectoryCountOffset);
    if (u16(optional.data()) != kPe32PlusMagic || u32(optional.data() + kOptionalImageSizeOffset) != image_.image_size ||
        u32(optional.data() + kOptionalChecksumOffset) != image_.checksum || header_size == 0 || header_size > image_.image_size ||
        !within(optional_rva, std::uint64_t(optional_size) + image_.section_count * kSectionHeaderBytes, 0, header_size) ||
        directories > (optional_size - kOptionalDataDirectoriesOffset) / kDataDirectoryBytes) {
      return fail("The PE32+ optional header has invalid identity, directory or section-table bounds.");
    }
    for (const auto& section : image_.sections) {
      if (section.size != 0 && section.rva < header_size) {
        return fail("A declared image section overlaps the PE headers.");
      }
    }
    std::array<std::uint8_t, kDataDirectoryBytes> directory{};
    if (directories > kImportDirectoryIndex) {
      if (!header(optional_rva + data_directory_offset(kImportDirectoryIndex), directory.data(), directory.size())) {
        return false;
      }
      import_rva_ = u32(directory.data());
      import_size_ = u32(directory.data() + 4);
    }
    if (directories > kIatDirectoryIndex) {
      if (!header(optional_rva + data_directory_offset(kIatDirectoryIndex), directory.data(), directory.size())) {
        return false;
      }
      iat_rva_ = u32(directory.data());
      iat_size_ = u32(directory.data() + 4);
    }
    return true;
  }

  ImageReader& reader_;
  const Inventory& image_;
  ImportSlotInventory result_;
  std::uint32_t first_section_rva_ = 0xffffffff;
  std::uint32_t import_rva_ = 0;
  std::uint32_t import_size_ = 0;
  std::uint32_t iat_rva_ = 0;
  std::uint32_t iat_size_ = 0;
};

}

ImportSlotInventory inspect_import_slot(ImageReader& reader, const Inventory& image, std::uint32_t iat_slot_rva) {
  return Parser(reader, image, iat_slot_rva).run();
}

}
