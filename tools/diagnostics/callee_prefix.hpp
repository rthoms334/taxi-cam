#pragma once

#include "reference_inventory.hpp"

namespace taxi_camera::discovery {

inline constexpr std::uint32_t kMaximumPrefixRequests = 8;
inline constexpr std::uint32_t kTargetPrefixBytes = 64;
inline constexpr std::uint32_t kCallProofBytes = 5;
inline constexpr std::uint32_t kLongestBranchProofBytes = 6;
inline constexpr std::uint32_t kMaximumTargetPrefixReadBytes = kMaximumPrefixRequests * kTargetPrefixBytes;
inline constexpr std::uint32_t kMaximumCallProofReadBytes = kMaximumPrefixRequests * kCallProofBytes;
inline constexpr std::uint32_t kMaximumBranchProofReadBytes = kMaximumPrefixRequests * kLongestBranchProofBytes;

struct CalleePrefixRequest {
  std::uint32_t call_rva = 0;
  std::uint32_t target_rva = 0;
};

struct CalleePrefix {
  std::uint32_t proof_call_rva = 0;
  std::uint32_t entry_rva = 0;
  std::uint32_t attempted_bytes = 0;
  std::uint32_t decoded_bytes = 0;
  bool ret_observed = false;
  std::string stop_reason;
  std::string error;
  std::vector<InstructionMetadata> instructions;
};

struct CalleePrefixInventory {
  bool valid = false;
  std::string error;
  std::uint32_t read_bytes = 0;
  std::uint32_t proof_bytes = 0;
  std::uint32_t read_failures = 0;
  std::vector<CalleePrefix> entries;
};

CalleePrefixInventory inspect_callee_prefixes(ImageReader& reader,
                                              const Inventory& image,
                                              InstructionDecoder& decoder,
                                              const FunctionInventory& proof,
                                              const std::vector<CalleePrefixRequest>& requests);

struct BranchPrefixRequest {
  std::uint32_t proof_branch_rva = 0;
  std::uint32_t entry_rva = 0;
};

struct BranchPrefix {
  std::uint32_t proof_branch_rva = 0;
  std::uint32_t entry_rva = 0;
  std::uint32_t attempted_bytes = 0;
  std::uint32_t decoded_bytes = 0;
  bool ret_observed = false;
  std::string stop_reason;
  std::string error;
  std::vector<InstructionMetadata> instructions;
};

struct BranchPrefixInventory {
  bool valid = false;
  std::string error;
  std::uint32_t read_bytes = 0;
  std::uint32_t proof_bytes = 0;
  std::uint32_t read_failures = 0;
  std::vector<BranchPrefix> entries;
};

BranchPrefixInventory inspect_branch_prefixes(ImageReader& reader,
                                              const Inventory& image,
                                              InstructionDecoder& decoder,
                                              const CalleePrefixInventory& proof,
                                              const std::vector<BranchPrefixRequest>& requests);

BranchPrefixInventory inspect_branch_prefixes(ImageReader& reader,
                                              const Inventory& image,
                                              InstructionDecoder& decoder,
                                              const BranchPrefixInventory& proof,
                                              const std::vector<BranchPrefixRequest>& requests);

}
