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
  std::int32_t span = -1;  // Served from this span, or -1 for its own read.
};

// One exact read of an object's known extent: from the first to the end of the
// last field this trace may observe in that object. Fields inside it are served
// from that read, so neighbouring fields cost one ReadProcessMemory instead of
// one each. The recheck rereads the span once, fresh, and compares only the
// bytes each field observed.
constexpr std::uint32_t kSpanBytes = 144;
struct Span {
  std::uint64_t address = 0;
  std::uint32_t size = 0;
  std::array<std::uint8_t, kSpanBytes> bytes{};
};

template <typename Result>
class BoundedReader {
 public:
  BoundedReader(MemoryReader& reader, Result& result) noexcept : reader_(reader), result_(result) {}

  // Reads [address+begin, address+end) now. Call only with the extent of fields
  // this trace goes on to observe in the same object.
  bool span(std::uint64_t address, std::uint64_t begin, std::uint64_t end, bool require_aligned = true) noexcept {
    if (end <= begin || end - begin > kSpanBytes ||
        !pointer_range(address, begin, static_cast<std::uint32_t>(end - begin), require_aligned))
      return fail(result_, OwnedViewStatus::invalid_pointer, "A required object span has a null, misaligned or overflowing pointer.");
    for (std::size_t index = 0; index < span_count_; ++index)
      if (address + begin >= spans_[index].address && end - begin <= spans_[index].size &&
          address + begin - spans_[index].address <= spans_[index].size - (end - begin))
        return true;
    if (span_count_ == spans_.size() || order_count_ == order_.size())
      return fail(result_, OwnedViewStatus::read_budget_exhausted, "The bounded observation count was exhausted.");
    auto& value = spans_[span_count_];
    value.address = address + begin;
    value.size = static_cast<std::uint32_t>(end - begin);
    if (!read_exact(value.address, value.size, value.bytes.data()))
      return false;
    order_[order_count_++] = -1 - static_cast<std::int32_t>(span_count_++);
    return true;
  }

  bool field(std::uint64_t address, std::uint64_t offset, std::uint32_t size, std::uint8_t* output, bool require_aligned = true) noexcept {
    if (!pointer_range(address, offset, size, require_aligned))
      return fail(result_, OwnedViewStatus::invalid_pointer, "A required field has a null, misaligned or overflowing pointer.");
    if (count_ == observations_.size() || order_count_ == order_.size())
      return fail(result_, OwnedViewStatus::read_budget_exhausted, "The bounded observation count was exhausted.");
    if (size == 0 || size > 16)
      return fail(result_, OwnedViewStatus::read_budget_exhausted, "The bounded attempted-read allowance was exhausted.");
    const auto at = address + offset;
    std::int32_t served = -1;
    for (std::size_t index = 0; index < span_count_; ++index)
      if (at >= spans_[index].address && size <= spans_[index].size && at - spans_[index].address <= spans_[index].size - size) {
        served = static_cast<std::int32_t>(index);
        break;
      }
    if (served >= 0) {
      std::copy_n(spans_[static_cast<std::size_t>(served)].bytes.begin() + (at - spans_[static_cast<std::size_t>(served)].address), size,
                  output);
    } else {
      if (!read_exact(at, size, output))
        return false;
      order_[order_count_++] = static_cast<std::int32_t>(count_);
    }
    auto& observation = observations_[count_++];
    observation.address = at;
    observation.size = size;
    observation.span = served;
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
    if (!span(control, 0, 32, false) || !field(control, 28, 4, bytes.data(), false))
      return false;
    if (u32(bytes.data()) != generation)
      return true;
    if (!word(control, 0, output, false))
      return false;
    if (output != 0 && !pointer_range(output, 0, 8))
      return fail(result_, OwnedViewStatus::invalid_pointer, "A generation-valid handle contains a malformed payload pointer.");
    return true;
  }

  // Repeats every read of the trace in its original order. Each observed field
  // is compared as soon as its own read or its span has been reread.
  bool recheck() noexcept {
    for (std::size_t step = 0; step < order_count_; ++step) {
      const auto item = order_[step];
      if (item < 0) {
        const auto index = static_cast<std::int32_t>(-1 - item);
        const auto& value = spans_[static_cast<std::size_t>(index)];
        std::array<std::uint8_t, kSpanBytes> fresh;
        if (!read_exact(value.address, value.size, fresh.data()))
          return false;
        for (std::size_t observed = 0; observed < count_; ++observed) {
          const auto& observation = observations_[observed];
          if (observation.span != index)
            continue;
          const auto* bytes = fresh.data() + (observation.address - value.address);
          if (!std::equal(bytes, bytes + observation.size, observation.bytes.begin()))
            return fail(result_, OwnedViewStatus::changed, "An observed field changed during the complete trace recheck.");
        }
        continue;
      }
      const auto& observation = observations_[static_cast<std::size_t>(item)];
      std::array<std::uint8_t, 16> bytes{};
      if (!read_exact(observation.address, observation.size, bytes.data()))
        return false;
      if (!std::equal(bytes.begin(), bytes.begin() + observation.size, observation.bytes.begin()))
        return fail(result_, OwnedViewStatus::changed, "An observed field changed during the complete trace recheck.");
    }
    return true;
  }

 private:
  static constexpr std::size_t kMaxSpans = 16;

  bool read_exact(std::uint64_t address, std::uint32_t size, std::uint8_t* output) noexcept {
    if (size == 0 || size > kReadBudget - result_.read_bytes)
      return fail(result_, OwnedViewStatus::read_budget_exhausted, "The bounded attempted-read allowance was exhausted.");
    result_.read_bytes += size;
    if (reader_.read(address, output, size))
      return true;
    ++result_.read_failures;
    return fail(result_, OwnedViewStatus::read_failed, "A required field could not be read exactly.");
  }

  MemoryReader& reader_;
  Result& result_;
  std::array<Observation, 64> observations_{};
  std::size_t count_ = 0;
  std::array<Span, kMaxSpans> spans_{};
  std::size_t span_count_ = 0;
  // Pass-one reads in order: an observation index, or -1 - span index.
  std::array<std::int32_t, 64 + kMaxSpans> order_{};
  std::size_t order_count_ = 0;
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

// The captured SetRenderTargets replay (1.9.12.0 +0x3E70BE0 and the +0x3E51C5D
// family) binds a texture T = Bitmap+88 through
//   record = [T+0x48] ? [[[T+0x48]+8] + 8*index] : [T+0x40]
// and increments the bound-target count only for a non-null record, while the
// binder (+0x3E5BFA0) walks the first `count` slots without a null check.
// Observe exactly that resolution for subresource index 0. A null result is an
// available observation, not a failure; malformed pointers refuse the snapshot.
bool render_target_record(BoundedReader<OwnedViewSnapshot>& source, std::uint64_t texture, bool& present) noexcept {
  present = false;
  std::uint64_t table = 0;
  if (!source.span(texture, 0x40, 0x50) || !source.word(texture, 0x48, table))
    return false;
  if (table != 0) {
    std::uint64_t entries = 0;
    if (!source.word(table, 8, entries))
      return false;
    if (entries == 0)
      return true;
    std::uint64_t entry = 0;
    if (!source.word(entries, 0, entry))
      return false;
    present = entry != 0;
    return true;
  }
  std::uint64_t direct = 0;
  if (!source.word(texture, 0x40, direct))
    return false;
  present = direct != 0;
  return true;
}

// Diagnostic observation of the two other material slots the captured output
// routine manages (add-diffuse 664, depth-stencil 712): Bitmap, texture and
// render-target record presence only. Nothing here changes camera readiness.
bool output_slot(BoundedReader<OwnedViewSnapshot>& source,
                 std::uint64_t material,
                 std::uint64_t offset,
                 OutputSlotObservation& slot) noexcept {
  slot = {};
  std::uint64_t bitmap = 0;
  if (!source.handle(material, offset, bitmap))
    return false;
  if (bitmap == 0)
    return true;
  slot.bitmap = true;
  std::uint64_t texture = 0;
  if (!source.word(bitmap, 88, texture))
    return false;
  if (texture == 0)
    return true;
  slot.texture = true;
  return render_target_record(source, texture, slot.render_target_record);
}

bool output_resource(BoundedReader<OwnedViewSnapshot>& source,
                     OwnedViewSnapshot& result,
                     std::uint64_t entry,
                     std::uint64_t view,
                     std::uint64_t& address,
                     std::array<std::int32_t, 2>& output_dimensions,
                     std::array<OutputSlotObservation, 3>& slots,
                     bool diagnostic_slots) noexcept {
  slots = {};
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
  slots[0].bitmap = true;
  std::uint64_t record = 0;
  if (!source.word(bitmap, 88, record))
    return false;
  if (record == 0)
    return true;
  slots[0].texture = true;
  if (!source.span(record, 16, 0x50) || !render_target_record(source, record, slots[0].render_target_record))
    return false;
  if (diagnostic_slots && (!source.span(entry_material, 664, 728) || !output_slot(source, entry_material, 664, slots[1]) ||
                           !output_slot(source, entry_material, 712, slots[2])))
    return false;
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
  if (!source.span(entry_address, 0, 24) || !source.word(entry_address, 0, key) || !source.word(entry_address, 16, payload_id))
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
  if (!source.span(entry_address, 24, 112) || !source.field(entry_address, 24, 4, bytes.data()))
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
  if (!source.span(view, 16, 120) || !source.handle(entry_address, 96, node) || !source.handle(view, 104, view_node))
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
                                     const ViewPoolSnapshot& pool,
                                     bool diagnostic_slots) noexcept {
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
  if (!source.span(entry_address, 0, 24) || !source.word(entry_address, 0, key) || !source.word(entry_address, 16, payload_id))
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
  if (!source.span(entry_address, 24, 112) || !source.field(entry_address, 24, 4, bytes.data()))
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
  if (!source.span(view, 16, 160) || !source.handle(entry_address, 96, node) || !source.handle(view, 104, view_node))
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
  std::array<OutputSlotObservation, 3> output_slots{};
  if (!output_resource(source, result, entry_address, view, resource_address, output_dimensions, output_slots, diagnostic_slots) ||
      !source.recheck())
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
  result.output_slots = output_slots;
  result.diagnostic_slots_observed = diagnostic_slots;
  return result;
}

}  // namespace taxi_camera::engine_camera
