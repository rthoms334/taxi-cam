#pragma once

#include "../../src/camera/image_inventory.hpp"

namespace taxi_camera::discovery {

inline constexpr std::uint32_t kMaximumExplicitTargets = 8;
inline constexpr std::uint64_t kMaximumScannedCodeBytes = 128ull * 1024 * 1024;
inline constexpr std::uint64_t kMaximumDecodedCodeBytes = 8ull * 1024 * 1024;
inline constexpr std::uint32_t kMaximumCodeReferences = 32;
inline constexpr std::uint32_t kMaximumReferenceCandidates = 1024;
inline constexpr std::uint32_t kMaximumCandidateFunctionBytes = 64 * 1024;
inline constexpr std::uint32_t kDefaultFunctionPrefixBytes = 4096;
inline constexpr std::uint32_t kMaximumFunctionPrefixBytes = 16384;
inline constexpr std::uint32_t kFunctionContextBudgetBytes = 32768;

struct LiteralTarget {
  std::string label;
  std::uint32_t rva;
  std::string expected_text;
};

struct CodeTarget {
  std::string label;
  std::uint32_t rva;
};

class InstructionDecoder {
 public:
  virtual ~InstructionDecoder() = default;
  virtual std::size_t decode(std::uint8_t* bytes, std::size_t available, std::uint32_t rva, std::string& instruction) = 0;
};

struct InstructionMetadata {
  std::uint32_t rva = 0;
  std::string text;
  std::uint32_t direct_call_target_rva = 0;
};

struct CodeReference {
  std::string target_label;
  std::uint32_t target_rva = 0;
  std::uint32_t function_begin_rva = 0;
  std::uint32_t function_end_rva = 0;
  std::uint32_t instruction_rva = 0;
  std::vector<InstructionMetadata> context;
};

struct ReferenceInventory {
  bool valid_targets = false;
  std::string error;
  std::uint64_t code_bytes = 0;
  std::uint64_t decoded_bytes = 0;
  std::uint32_t candidate_count = 0;
  std::uint32_t missing_function_bounds = 0;
  std::uint32_t failed_boundary_checks = 0;
  std::uint32_t read_failures = 0;
  std::uint32_t runtime_function_count = 0;
  bool code_limit_reached = false;
  bool decode_limit_reached = false;
  bool reference_limit_reached = false;
  std::vector<CodeReference> references;
};

ReferenceInventory find_literal_references(ImageReader& reader,
                                           const Inventory& image,
                                           InstructionDecoder& decoder,
                                           const std::vector<LiteralTarget>& targets);

ReferenceInventory find_direct_call_references(ImageReader& reader,
                                               const Inventory& image,
                                               InstructionDecoder& decoder,
                                               const std::vector<LiteralTarget>& required_literals,
                                               const std::vector<CodeTarget>& targets);

struct FunctionContext {
  std::uint32_t requested_rva = 0;
  std::uint32_t function_begin_rva = 0;
  std::uint32_t function_end_rva = 0;
  std::uint32_t bytes_read = 0;
  std::uint32_t decoded_bytes = 0;
  bool truncated = false;
  bool decode_failed = false;
  bool requested_boundary_found = false;
  std::string error;
  std::vector<InstructionMetadata> instructions;
};

struct FunctionInventory {
  bool valid_targets = false;
  std::string error;
  std::uint64_t code_bytes = 0;
  std::uint64_t decoded_bytes = 0;
  std::uint32_t runtime_function_count = 0;
  std::uint32_t read_failures = 0;
  std::uint32_t missing_function_bounds = 0;
  bool partial = false;
  std::vector<FunctionContext> functions;
};

FunctionInventory inspect_function_contexts(ImageReader& reader,
                                            const Inventory& image,
                                            InstructionDecoder& decoder,
                                            const std::vector<LiteralTarget>& required_literals,
                                            const std::vector<std::uint32_t>& instruction_rvas,
                                            std::uint32_t prefix_limit = kDefaultFunctionPrefixBytes);

}
