#include "owned_view.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace taxi_camera::engine_camera {
namespace {

constexpr std::uint32_t kReadBudget = 8192;

bool pointer_range(std::uint64_t address, std::uint64_t offset, std::uint32_t size, bool require_aligned = true) noexcept {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  return address != 0 && (!require_aligned || address % 8 == 0) && offset <= maximum - address && size <= maximum - (address + offset);
}

std::uint32_t u32(const std::uint8_t* bytes) noexcept {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index)
    value |= std::uint32_t(bytes[index]) << (index * 8);
  return value;
}

std::uint64_t u64(const std::uint8_t* bytes) noexcept {
  return std::uint64_t(u32(bytes)) | (std::uint64_t(u32(bytes + 4)) << 32);
}

template <typename Result>
bool fail(Result& result, OwnedViewStatus status, const char* error) noexcept {
  result.status = status;
  result.error = error;
  return false;
}

struct Observation {
  std::uint64_t address = 0;
  std::uint32_t size = 0;
  std::array<std::uint8_t, 16> bytes{};
};

template <typename Result>
class BoundedReader {
 public:
  BoundedReader(MemoryReader& reader, Result& result) noexcept : reader_(reader), result_(result) {}

  bool field(std::uint64_t address, std::uint64_t offset, std::uint32_t size, std::uint8_t* output, bool require_aligned = true) noexcept {
    if (!pointer_range(address, offset, size, require_aligned))
      return fail(result_, OwnedViewStatus::invalid_pointer, "A required field has a null, misaligned or overflowing pointer.");
    if (count_ == observations_.size())
      return fail(result_, OwnedViewStatus::read_budget_exhausted, "The bounded observation count was exhausted.");
    if (!read(address + offset, size, output))
      return false;
    auto& observation = observations_[count_++];
    observation.address = address + offset;
    observation.size = size;
    std::copy_n(output, size, observation.bytes.begin());
    return true;
  }

  bool word(std::uint64_t address, std::uint64_t offset, std::uint64_t& output, bool require_aligned = true) noexcept {
    std::array<std::uint8_t, 8> bytes{};
    if (!field(address, offset, bytes.size(), bytes.data(), require_aligned))
      return false;
    output = u64(bytes.data());
    return true;
  }

  // Null/stale/null-payload references are available observations with output=0.
  // A nonzero payload is range-checked without dereferencing its contents here.
  bool handle(std::uint64_t address, std::uint64_t offset, std::uint64_t& output) noexcept {
    output = 0;
    std::array<std::uint8_t, 16> bytes{};
    if (!field(address, offset, bytes.size(), bytes.data()))
      return false;
    const auto control = u64(bytes.data());
    const auto generation = u32(bytes.data() + 8);
    if (control == 0)
      return true;
    // The captured generation resolver uses ordinary MOV loads. Controls alone
    // may be byte aligned: preserve their exact addresses, including low bits.
    if (!field(control, 28, 4, bytes.data(), false))
      return false;
    if (u32(bytes.data()) != generation)
      return true;
    if (!word(control, 0, output, false))
      return false;
    if (output != 0 && !pointer_range(output, 0, 8))
      return fail(result_, OwnedViewStatus::invalid_pointer, "A generation-valid handle contains a malformed payload pointer.");
    return true;
  }

  bool recheck() noexcept {
    for (std::size_t index = 0; index < count_; ++index) {
      const auto& observation = observations_[index];
      std::array<std::uint8_t, 16> bytes{};
      if (!read(observation.address, observation.size, bytes.data()))
        return false;
      if (!std::equal(bytes.begin(), bytes.begin() + observation.size, observation.bytes.begin()))
        return fail(result_, OwnedViewStatus::changed, "An observed field changed during the complete trace recheck.");
    }
    return true;
  }

 private:
  bool read(std::uint64_t address, std::uint32_t size, std::uint8_t* output) noexcept {
    if (size == 0 || size > 16 || size > kReadBudget - result_.read_bytes)
      return fail(result_, OwnedViewStatus::read_budget_exhausted, "The bounded attempted-read allowance was exhausted.");
    result_.read_bytes += size;
    if (reader_.read(address, output, size))
      return true;
    ++result_.read_failures;
    return fail(result_, OwnedViewStatus::read_failed, "A required field could not be read exactly.");
  }

  MemoryReader& reader_;
  Result& result_;
  std::array<Observation, 32> observations_{};
  std::size_t count_ = 0;
};

bool valid_pool(const ViewPoolSnapshot& pool) noexcept {
  if (!pool.valid || pool.status != ViewPoolStatus::complete || pool.slots_examined != 8 || pool.read_failures != 0 ||
      !pointer_range(pool.array_address, 0, 64))
    return false;
  for (std::uint32_t index = 0; index < pool.slots.size(); ++index) {
    const auto& slot = pool.slots[index];
    if (slot.index != index || !pointer_range(slot.view_address, 0, 8))
      return false;
    for (std::uint32_t previous = 0; previous < index; ++previous) {
      if (slot.view_address == pool.slots[previous].view_address)
        return false;
    }
  }
  return true;
}

bool output_resource(BoundedReader<OwnedViewSnapshot>& source,
                     OwnedViewSnapshot& result,
                     std::uint64_t entry,
                     std::uint64_t view,
                     std::uint64_t& address,
                     std::array<std::int32_t, 2>& output_dimensions) noexcept {
  std::uint64_t entry_material = 0;
  std::uint64_t view_material = 0;
  if (!source.handle(entry, 80, entry_material) || !source.handle(view, 144, view_material))
    return false;
  if (entry_material != view_material)
    return fail(result, OwnedViewStatus::material_mismatch, "The entry and view do not resolve to the same output material.");
  if (entry_material == 0)
    return true;
  std::uint64_t bitmap = 0;
  if (!source.handle(entry_material, 520, bitmap))
    return false;
  if (bitmap == 0)
    return true;
  std::uint64_t record = 0;
  if (!source.word(bitmap, 88, record))
    return false;
  if (record == 0)
    return true;
  std::uint64_t wrapper = 0;
  if (!source.word(record, 16, wrapper))
    return false;
  if (wrapper == 0)
    return true;
  std::uint64_t resource = 0;
  if (!source.word(wrapper, 168, resource))
    return false;
  if (resource != 0 && !pointer_range(resource, 0, 8))
    return fail(result, OwnedViewStatus::invalid_pointer, "The optional resource member is malformed.");
  if (resource != 0) {
    std::array<std::uint8_t, 8> bytes{};
    // Captured66809728 compares Bitmap40/44 with view32/36 before its output
    // replacement branch. Observe these exact fields, never dereference the
    // opaque resource member or infer completed allocation from these values.
    if (!source.field(bitmap, 40, bytes.size(), bytes.data()))
      return false;
    output_dimensions = {std::bit_cast<std::int32_t>(u32(bytes.data())), std::bit_cast<std::int32_t>(u32(bytes.data() + 4))};
  }
  address = resource;
  return true;
}

}  // namespace

OwnedViewCloseSnapshot inspect_owned_view_for_close(MemoryReader& reader,
                                                    std::uint64_t entry_address,
                                                    std::uint64_t expected_id,
                                                    const ViewPoolSnapshot& pool) noexcept {
  OwnedViewCloseSnapshot result;
  if (!expected_id || !pointer_range(entry_address, 0, 8)) {
    fail(result, OwnedViewStatus::invalid_request, "Gate closure requires a nonzero owned ID and aligned entry.");
    return result;
  }
  if (!valid_pool(pool)) {
    fail(result, OwnedViewStatus::invalid_pool, "Gate closure requires the complete current pool proof.");
    return result;
  }
  BoundedReader source(reader, result);
  std::uint64_t key = 0, payload_id = 0;
  std::array<std::uint8_t, 16> bytes{};
  if (!source.word(entry_address, 0, key) || !source.word(entry_address, 16, payload_id))
    return result;
  if (key != expected_id || payload_id != expected_id) {
    fail(result, OwnedViewStatus::id_mismatch, "The entry key or payload ID changed before closure.");
    return result;
  }
  if (!source.field(entry_address, 8, 1, bytes.data()))
    return result;
  if (bytes[0] != 1) {
    fail(result, bytes[0] == 0 ? OwnedViewStatus::pending : OwnedViewStatus::invalid_ready_byte,
         "Gate closure requires a ready owned entry.");
    return result;
  }
  if (!source.field(entry_address, 24, 4, bytes.data()))
    return result;
  if (u32(bytes.data()) != 2) {
    fail(result, OwnedViewStatus::invalid_request, "Gate closure requires an independent-pose mode2 entry.");
    return result;
  }
  if (!source.field(entry_address, 76, 4, bytes.data()))
    return result;
  const auto index = std::bit_cast<std::int32_t>(u32(bytes.data()));
  if (index < 0 || index >= 8) {
    fail(result, OwnedViewStatus::invalid_view_index, "The entry index is outside the captured eight-view pool.");
    return result;
  }
  std::uint64_t view = 0;
  if (!source.word(pool.array_address, std::uint64_t(index) * 8, view))
    return result;
  if (view != pool.slots[index].view_address) {
    fail(result, OwnedViewStatus::pool_changed, "The selected view changed since the complete pool inspection.");
    return result;
  }
  std::uint64_t node = 0, view_node = 0;
  if (!source.handle(entry_address, 96, node) || !source.handle(view, 104, view_node))
    return result;
  if (!node || !view_node) {
    fail(result, OwnedViewStatus::node_unavailable, "The owned entry/view association is unavailable.");
    return result;
  }
  if (node != view_node) {
    fail(result, OwnedViewStatus::node_mismatch, "The entry and view Node identities differ.");
    return result;
  }
  std::array<std::array<std::int32_t, 2>, 3> dimensions{};
  for (std::uint32_t pair = 0; pair < dimensions.size(); ++pair) {
    if (!source.field(view, 16 + pair * 8, 8, bytes.data()))
      return result;
    dimensions[pair] = {std::bit_cast<std::int32_t>(u32(bytes.data())), std::bit_cast<std::int32_t>(u32(bytes.data() + 4))};
  }
  if (!source.field(view, 48, 16, bytes.data()))
    return result;
  const std::array<std::uint64_t, 2> flags{u64(bytes.data()), u64(bytes.data() + 8)};
  if (!source.recheck())
    return result;
  result.complete = true;
  result.status = OwnedViewStatus::ready;
  result.view_index = index;
  result.view_address = view;
  result.dimensions = dimensions;
  result.flags = flags;
  return result;
}
OwnedViewSnapshot inspect_owned_view(MemoryReader& reader,
                                     std::uint64_t entry_address,
                                     std::uint64_t expected_id,
                                     const ViewPoolSnapshot& pool) noexcept {
  OwnedViewSnapshot result;
  if (expected_id == 0 || !pointer_range(entry_address, 0, 8)) {
    fail(result, OwnedViewStatus::invalid_request, "Owned-entry inspection requires a nonzero ID and aligned entry address.");
    return result;
  }
  if (!valid_pool(pool)) {
    fail(result, OwnedViewStatus::invalid_pool, "The supplied pool is not a complete, bounded snapshot of eight distinct views.");
    return result;
  }
  BoundedReader source(reader, result);
  std::uint64_t key = 0;
  std::uint64_t payload_id = 0;
  std::array<std::uint8_t, 16> bytes{};
  if (!source.word(entry_address, 0, key) || !source.word(entry_address, 16, payload_id))
    return result;
  if (key != expected_id || payload_id != expected_id) {
    fail(result, OwnedViewStatus::id_mismatch, "The entry key or payload ID differs from the expected owned ID.");
    return result;
  }
  if (!source.field(entry_address, 8, 1, bytes.data()))
    return result;
  if (bytes[0] == 0) {
    if (source.recheck()) {
      result.complete = true;
      result.status = OwnedViewStatus::pending;
    }
    return result;
  }
  if (bytes[0] != 1) {
    fail(result, OwnedViewStatus::invalid_ready_byte, "The entry ready byte is neither zero nor one.");
    return result;
  }
  if (!source.field(entry_address, 24, 4, bytes.data()))
    return result;
  const auto mode = u32(bytes.data());
  if (!source.field(entry_address, 76, 4, bytes.data()))
    return result;
  const auto index = std::bit_cast<std::int32_t>(u32(bytes.data()));
  if (index < 0 || index >= 8) {
    fail(result, OwnedViewStatus::invalid_view_index, "The ready entry does not identify one of the eight pool views.");
    return result;
  }
  std::uint64_t view = 0;
  if (!source.word(pool.array_address, std::uint64_t(index) * 8, view))
    return result;
  if (view != pool.slots[index].view_address) {
    fail(result, OwnedViewStatus::pool_changed, "The selected pool pointer no longer matches the supplied snapshot.");
    return result;
  }
  std::uint64_t node = 0;
  std::uint64_t view_node = 0;
  if (!source.handle(entry_address, 96, node) || !source.handle(view, 104, view_node))
    return result;
  if (node == 0 || view_node == 0) {
    fail(result, OwnedViewStatus::node_unavailable, "A ready entry has an unavailable entry or view Node reference.");
    return result;
  }
  if (node != view_node) {
    fail(result, OwnedViewStatus::node_mismatch, "The entry and view Node references resolve to different objects.");
    return result;
  }
  std::uint64_t camera = 0;
  if (!source.word(node, 256, camera) || !source.field(camera, 160, 2, bytes.data()))
    return result;
  if ((std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8)) != 7) {
    fail(result, OwnedViewStatus::wrong_camera_type, "The Node payload does not have the captured Camera type value seven.");
    return result;
  }
  if (!source.field(camera, 1616, 4, bytes.data()))
    return result;
  const float fov = std::bit_cast<float>(u32(bytes.data()));
  if (!std::isfinite(fov) || fov <= 0) {
    fail(result, OwnedViewStatus::invalid_fov, "The Camera FOV is not finite and positive.");
    return result;
  }
  std::array<std::array<std::int32_t, 2>, 3> dimensions{};
  for (std::uint32_t pair = 0; pair < dimensions.size(); ++pair) {
    if (!source.field(view, 16 + pair * 8, 8, bytes.data()))
      return result;
    dimensions[pair] = {std::bit_cast<std::int32_t>(u32(bytes.data())), std::bit_cast<std::int32_t>(u32(bytes.data() + 4))};
  }
  if (!source.field(view, 48, 16, bytes.data()))
    return result;
  const std::array<std::uint64_t, 2> flags{u64(bytes.data()), u64(bytes.data() + 8)};
  std::uint64_t resource_address = 0;
  std::array<std::int32_t, 2> output_dimensions{};
  if (!output_resource(source, result, entry_address, view, resource_address, output_dimensions) || !source.recheck())
    return result;
  result.complete = true;
  result.ready = true;
  result.status = OwnedViewStatus::ready;
  result.view_index = index;
  result.mode = mode;
  result.view_address = view;
  result.node_address = node;
  result.camera_address = camera;
  result.fov = fov;
  result.dimensions = dimensions;
  result.output_dimensions = output_dimensions;
  result.flags = flags;
  result.resource_present = resource_address != 0;
  result.resource_address = resource_address;
  return result;
}

}  // namespace taxi_camera::engine_camera
