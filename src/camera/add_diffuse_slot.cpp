#include "add_diffuse_slot.hpp"

#include <array>
#include <cstring>
#include <limits>

namespace taxi_camera::engine_camera {
namespace {

constexpr std::uint32_t kBudget = 4096;
// VIEWPORT_MATERIAL_ADD_DIFFUSE VP%d lives at this material-handle offset.
constexpr std::uint32_t kAddDiffuseOffset = 664;

bool range(std::uint64_t address, std::uint64_t offset, std::uint32_t size, bool aligned) noexcept {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  return address != 0 && (!aligned || address % 8 == 0) && offset <= maximum - address && size <= maximum - (address + offset);
}

std::uint32_t u32(const void* bytes) noexcept {
  std::uint32_t value = 0;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

std::uint64_t u64(const void* bytes) noexcept {
  std::uint64_t value = 0;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

struct Observation {
  std::uint64_t address = 0;
  std::uint32_t size = 0;
  std::array<std::uint8_t, 16> bytes{};
};

class Reader {
 public:
  Reader(MemoryReader& source, AddDiffuseSlot& result) noexcept : source_(source), result_(result) {}

  bool fail(const char* error) noexcept {
    result_.error = error;
    return false;
  }

  bool field(std::uint64_t address, std::uint32_t offset, void* output, std::uint32_t size, bool control = false) noexcept {
    if (!range(address, offset, size, !control) || size == 0 || size > 16)
      return fail("field_bounds");
    if (count_ == observations_.size())
      return fail("observation_limit");
    if (!read(address + offset, output, size))
      return false;
    auto& item = observations_[count_++];
    item.address = address + offset;
    item.size = size;
    std::memcpy(item.bytes.data(), output, size);
    return true;
  }

  bool word(std::uint64_t address, std::uint32_t offset, std::uint64_t& output, bool control = false) noexcept {
    return field(address, offset, &output, 8, control);
  }

  bool handle(std::uint64_t address, std::uint32_t offset, std::uint64_t& payload, bool& stale) noexcept {
    std::array<std::uint8_t, 16> bytes{};
    payload = 0;
    stale = false;
    if (!field(address, offset, bytes.data(), 16))
      return false;
    const auto control = u64(bytes.data());
    const auto generation = u32(bytes.data() + 8);
    if (!control)
      return true;
    std::uint32_t current = 0;
    if (!field(control, 28, &current, 4, true))
      return false;
    if (current != generation) {
      stale = true;
      return true;
    }
    if (!word(control, 0, payload, true))
      return false;
    if (payload && !range(payload, 0, 8, true))
      return fail("payload_pointer");
    return true;
  }

  bool recheck() noexcept {
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t index = 0; index < count_; ++index) {
      const auto& item = observations_[index];
      if (!read(item.address, bytes.data(), item.size))
        return false;
      if (std::memcmp(bytes.data(), item.bytes.data(), item.size) != 0)
        return fail("changed");
    }
    return true;
  }

 private:
  bool read(std::uint64_t address, void* output, std::uint32_t size) noexcept {
    if (size > kBudget - result_bytes_)
      return fail("read_budget");
    result_bytes_ += size;
    if (source_.read(address, output, size))
      return true;
    return fail("exact_read_failed");
  }

  MemoryReader& source_;
  AddDiffuseSlot& result_;
  std::uint32_t result_bytes_ = 0;
  std::array<Observation, 24> observations_{};
  std::size_t count_ = 0;
};

}  // namespace

AddDiffuseSlot inspect_add_diffuse_slot(MemoryReader& memory, std::uint64_t view) noexcept {
  AddDiffuseSlot result;
  Reader source(memory, result);
  if (!range(view, 0, 8, true)) {
    result.error = "view_pointer";
    return result;
  }
  std::uint64_t material = 0;
  bool stale = false;
  if (!source.handle(view, 144, material, stale))
    return result;
  std::uint64_t flags = 0;
  // Sample only. Activation can change; this word is not part of the recheck
  // and is never written back. Bit 49 is the observed slot 9 allocation gate.
  std::array<std::uint8_t, 8> flag_bytes{};
  if (!range(view, 48, 8, true) || !memory.read(view + 48, flag_bytes.data(), flag_bytes.size())) {
    result.error = "flag_read_failed";
    return result;
  }
  std::memcpy(&flags, flag_bytes.data(), sizeof(flags));
  result.bit49 = (flags & (1ull << 49)) != 0;
  if (!material || stale) {
    if (!source.recheck())
      return result;
    result.complete = true;
    return result;
  }
  std::uint64_t bitmap = 0;
  if (!source.handle(material, kAddDiffuseOffset, bitmap, stale))
    return result;
  if (!bitmap || stale) {
    if (!source.recheck())
      return result;
    result.complete = true;
    return result;
  }
  result.present = true;
  std::uint32_t bitmap_width = 0;
  std::uint32_t bitmap_height = 0;
  if (!source.field(bitmap, 40, &bitmap_width, 4) || !source.field(bitmap, 44, &bitmap_height, 4))
    return result;
  std::uint64_t record = 0;
  if (!source.word(bitmap, 88, record))
    return result;
  if (!record || record == std::numeric_limits<std::uint64_t>::max()) {
    result.width = bitmap_width;
    result.height = bitmap_height;
    if (!source.recheck())
      return result;
    result.complete = true;
    return result;
  }
  std::uint64_t wrapper = 0;
  if (!source.word(record, 16, wrapper))
    return result;
  if (!wrapper) {
    result.width = bitmap_width;
    result.height = bitmap_height;
    if (!source.recheck())
      return result;
    result.complete = true;
    return result;
  }
  std::array<std::uint8_t, 56> descriptor{};
  for (std::uint32_t offset = 0; offset < descriptor.size(); offset += 8)
    if (!source.field(wrapper, 96 + offset, descriptor.data() + offset, 8))
      return result;
  std::uint64_t width = 0;
  std::memcpy(&width, descriptor.data() + 16, sizeof(width));
  std::uint32_t height = 0;
  std::uint32_t format = 0;
  std::memcpy(&height, descriptor.data() + 24, sizeof(height));
  std::memcpy(&result.layers, descriptor.data() + 28, sizeof(result.layers));
  std::memcpy(&result.mips, descriptor.data() + 30, sizeof(result.mips));
  std::memcpy(&format, descriptor.data() + 32, sizeof(format));
  if (width > std::numeric_limits<std::uint32_t>::max()) {
    source.fail("width_overflow");
    return result;
  }
  result.width = static_cast<std::uint32_t>(width);
  result.height = height;
  result.format = format;
  std::uint64_t resource = 0;
  if (!source.word(wrapper, 168, resource))
    return result;
  if (resource && resource % 8 != 0) {
    source.fail("resource_alignment");
    return result;
  }
  if (!source.recheck())
    return result;
  result.resource_address = resource;
  result.resource_present = resource != 0;
  result.complete = true;
  result.error = "";
  return result;
}

}  // namespace taxi_camera::engine_camera
