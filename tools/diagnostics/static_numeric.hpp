#pragma once

#include "../../src/camera/image_inventory.hpp"

namespace taxi_camera::discovery {

inline constexpr std::uint32_t kMaximumNumericRequests = 8;
inline constexpr std::uint32_t kMaximumScalarsPerNumericRequest = 3;
inline constexpr std::uint32_t kMaximumNumericReadBytes = 256;
inline constexpr std::uint32_t kLargestNumericBatchBytes =
    kMaximumNumericRequests * kMaximumScalarsPerNumericRequest * static_cast<std::uint32_t>(sizeof(double));

enum class StaticNumericKind { float32, float64 };

struct StaticNumericRequest {
  std::uint32_t rva = 0;
  StaticNumericKind kind = StaticNumericKind::float64;
  std::uint32_t count = 1;
};

struct StaticNumericRecord {
  std::uint32_t rva = 0;
  StaticNumericKind kind = StaticNumericKind::float64;
  std::uint32_t count = 0;
  std::vector<double> values;
  std::string error;
  std::uint32_t bytes = 0;
};

struct StaticNumericInventory {
  bool valid = false;
  std::string error;
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
  std::vector<StaticNumericRecord> records;
};

StaticNumericInventory inspect_static_numeric(ImageReader& reader,
                                              const Inventory& image,
                                              const std::vector<StaticNumericRequest>& requests);

}
