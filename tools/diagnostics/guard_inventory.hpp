#pragma once

#include "../../src/camera/image_inventory.hpp"

namespace taxi_camera::discovery {

inline constexpr std::uint32_t kLoadConfigGuardPrefixBytes = 148;

struct GuardInventory {
  bool present = false;
  bool valid = false;
  std::string error;
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
  std::uint32_t declared_size = 0;
  std::uint32_t guard_flags = 0;
  bool guard_cf_characteristic = false;
  bool cf_instrumented = false;
  bool function_table_present = false;
  bool dispatch_slot_present = false;
  std::uint32_t dispatch_slot_rva = 0;
};

GuardInventory inspect_guard_metadata(ImageReader& reader, const Inventory& image, std::uint64_t loaded_image_base);

}
