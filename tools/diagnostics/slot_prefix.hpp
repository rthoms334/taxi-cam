#pragma once

#include "reference_inventory.hpp"

namespace taxi_camera::discovery {

inline constexpr std::uint32_t kMaximumSlotRequests = 8;
inline constexpr std::uint32_t kSlotWordBytes = 8;
inline constexpr std::uint32_t kSlotTargetPrefixBytes = 64;
inline constexpr std::uint32_t kMaximumSlotTargetReadBytes = kMaximumSlotRequests * kSlotTargetPrefixBytes;
inline constexpr std::uint32_t kMaximumSlotProofReadBytes = kMaximumSlotRequests * kSlotWordBytes;

struct SlotPrefixRequest {
  std::uint32_t slot_rva = 0;
  std::uint32_t entry_rva = 0;
};

struct SlotPrefix {
  std::uint32_t proof_slot_rva = 0;
  std::uint32_t entry_rva = 0;
  std::uint32_t attempted_bytes = 0;
  std::uint32_t decoded_bytes = 0;
  bool ret_observed = false;
  std::string stop_reason;
  std::string error;
  std::vector<InstructionMetadata> instructions;
};

struct SlotPrefixInventory {
  bool valid = false;
  std::string error;
  std::uint32_t read_bytes = 0;
  std::uint32_t proof_bytes = 0;
  std::uint32_t read_failures = 0;
  std::vector<SlotPrefix> entries;
};

SlotPrefixInventory inspect_slot_prefixes(ImageReader& reader,
                                          const Inventory& image,
                                          InstructionDecoder& decoder,
                                          std::uint64_t loaded_image_base,
                                          const std::vector<SlotPrefixRequest>& requests);

}
