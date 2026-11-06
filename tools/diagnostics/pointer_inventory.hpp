#pragma once

#include "../../src/camera/image_inventory.hpp"

namespace taxi_camera::discovery {

inline constexpr std::uint32_t kPointerWordBytes = 8;
inline constexpr std::uint32_t kMaximumPointerTableEntries = 32;
inline constexpr std::uint32_t kDefaultPointerTableEntries = 24;
inline constexpr std::uint32_t kMaximumPointerTableReadBytes = (kMaximumPointerTableEntries + 1) * kPointerWordBytes;
inline constexpr std::int32_t kPrecedingWordIndex = -1;

enum class PointerCategory { null_pointer, external, image_data, image_code };

const char* pointer_category_name(PointerCategory category);

struct PointerEntry {
  std::int32_t index = 0;
  std::uint32_t source_rva = 0;
  std::uint32_t target_rva = 0;
  PointerCategory category = PointerCategory::null_pointer;
};

struct PointerInventory {
  bool valid_table = false;
  std::string error;
  std::uint32_t table_rva = 0;
  std::uint32_t entry_limit = 0;
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
  bool entry_limit_reached = false;
  bool stopped_on_non_code = false;
  bool preceding_available = false;
  PointerEntry preceding;
  std::vector<PointerEntry> entries;
};

PointerInventory inspect_pointer_table(ImageReader& reader,
                                       const Inventory& image,
                                       std::uint64_t loaded_image_base,
                                       std::uint32_t table_rva,
                                       std::uint32_t max_entries = kDefaultPointerTableEntries,
                                       bool include_previous = true);

}
