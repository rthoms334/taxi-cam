#pragma once

#include "../../src/camera/image_inventory.hpp"

namespace taxi_camera::discovery {

inline constexpr std::uint32_t kMaximumImportMetadataReadBytes = 131072;
inline constexpr std::uint32_t kMaximumImportDescriptors = 256;
inline constexpr std::uint32_t kMaximumLookupThunksPerDescriptor = 2048;
inline constexpr std::uint32_t kMaximumImportNameBytesIncludingNul = 128;
inline constexpr std::uint32_t kImportNameHintBytes = 2;

struct ImportSlotInventory {
  bool valid = false;
  bool available = false;
  std::string status;
  std::string error;
  std::string module;
  std::string symbol;
  std::uint32_t iat_slot_rva = 0;
  std::uint32_t descriptor_rva = 0;
  std::uint32_t lookup_thunk_rva = 0;
  std::uint32_t hint_name_rva = 0;
  bool by_ordinal = false;
  std::uint16_t ordinal = 0;
  std::uint32_t descriptors_read = 0;
  std::uint32_t thunks_read = 0;
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
};

ImportSlotInventory inspect_import_slot(ImageReader& reader, const Inventory& image, std::uint32_t iat_slot_rva);

}
