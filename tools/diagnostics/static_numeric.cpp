#include "static_numeric.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace taxi_camera::discovery {
namespace {

constexpr std::uint32_t kReadable = 0x40000000;
constexpr std::uint32_t kExcluded = 0x80000000 | 0x20000000 | 0x02000000;
static_assert(sizeof(float) == 4 && sizeof(double) == 8 && std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559);

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

bool allowed_range(const Inventory& image, std::uint32_t rva, std::uint32_t size) {
  if (rva > image.image_size || size > image.image_size - rva) {
    return false;
  }
  for (const auto& section : image.sections) {
    if ((section.flags & kReadable) != 0 && (section.flags & kExcluded) == 0 && rva >= section.rva && rva - section.rva <= section.size &&
        size <= section.size - (rva - section.rva)) {
      return true;
    }
  }
  return false;
}

std::uint64_t little_endian(const std::uint8_t* bytes, std::uint32_t size) {
  std::uint64_t value = 0;
  for (std::uint32_t index = 0; index < size; ++index) {
    value |= std::uint64_t(bytes[index]) << (index * 8);
  }
  return value;
}

StaticNumericRecord inspect_request(ImageReader& reader,
                                    const Inventory& image,
                                    const StaticNumericRequest& request,
                                    StaticNumericInventory& result) {
  StaticNumericRecord record;
  record.rva = request.rva;
  record.kind = request.kind;
  record.count = request.count;
  if ((request.kind != StaticNumericKind::float32 && request.kind != StaticNumericKind::float64) || request.count == 0 ||
      request.count > kMaximumScalarsPerNumericRequest) {
    record.error = "Request one to three float32 or float64 scalars.";
    return record;
  }
  const std::uint32_t width = request.kind == StaticNumericKind::float32 ? 4 : 8;
  const auto size = width * request.count;
  if (!allowed_range(image, request.rva, size)) {
    record.error = "The complete numeric range is not in one readable, non-writable, non-executable, non-discardable image section.";
    return record;
  }
  if (size > kMaximumNumericReadBytes - result.read_bytes) {
    record.error = "The static numeric read allowance is exhausted.";
    return record;
  }
  result.read_bytes += size;
  record.bytes = size;
  std::array<std::uint8_t, kMaximumScalarsPerNumericRequest * sizeof(double)> buffer{};
  std::uint32_t offset = 0;
  while (offset < size) {
    const auto rva = request.rva + offset;
    const auto window = reader.query(rva, size - offset);
    if (!window.readable || window.size == 0 || window.size > size - offset) {
      ++result.read_failures;
      record.error = "A numeric query window is unreadable or invalid.";
      return record;
    }
    if (!reader.read(rva, buffer.data() + offset, window.size)) {
      ++result.read_failures;
      record.error = "The numeric range could not be read exactly.";
      return record;
    }
    offset += window.size;
  }

  std::vector<double> values;
  values.reserve(request.count);
  for (std::uint32_t index = 0; index < request.count; ++index) {
    const auto word = little_endian(buffer.data() + index * width, width);
    const double value = request.kind == StaticNumericKind::float32
                             ? static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(word)))
                             : std::bit_cast<double>(word);
    if (!std::isfinite(value)) {
      record.error = "A requested scalar is non-finite.";
      return record;
    }
    values.push_back(value);
  }
  record.values = std::move(values);
  return record;
}

}

StaticNumericInventory inspect_static_numeric(ImageReader& reader,
                                              const Inventory& image,
                                              const std::vector<StaticNumericRequest>& requests) {
  StaticNumericInventory result;
  if (!image.valid_image || image.machine != 0x8664 || image.image_size == 0 || !valid_sections(image)) {
    result.error = "Invalid loaded-image or section metadata.";
    return result;
  }
  if (requests.empty() || requests.size() > kMaximumNumericRequests) {
    result.error = "Request one to eight explicit numeric ranges.";
    return result;
  }
  result.valid = true;
  result.records.reserve(requests.size());
  for (const auto& request : requests) {
    result.records.push_back(inspect_request(reader, image, request, result));
    if (!result.records.back().error.empty()) {
      result.valid = false;
      result.error = "One or more requested numeric ranges could not be verified.";
    }
  }
  return result;
}

}
