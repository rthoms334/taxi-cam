#include "aircraft_inventory.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace taxi_camera::discovery {
namespace {

constexpr std::uint32_t kReadable = 0x40000000;
constexpr std::uint32_t kWritable = 0x80000000;
constexpr std::uint32_t kExecutable = 0x20000000;
constexpr std::uint32_t kDiscardable = 0x02000000;
// GUID.ToByteArray-style fixed candidates, not a general GUID/pose decoder.
constexpr std::array<std::uint8_t, 16> kTailKey = {0x4d, 0x98, 0xb3, 0xf6, 0x58, 0x4d, 0xc4, 0x41,
                                                   0x8b, 0xa8, 0x32, 0xcd, 0x24, 0xcc, 0xc1, 0x3b};
constexpr std::array<std::uint8_t, 16> kGearKey = {0x33, 0x54, 0xb2, 0x89, 0x54, 0x64, 0xdf, 0x43,
                                                   0x9b, 0x91, 0xbf, 0x33, 0x8a, 0x90, 0x6a, 0x19};

std::uint64_t u64(const std::uint8_t* bytes) {
  std::uint64_t result = 0;
  for (unsigned i = 0; i < 8; ++i)
    result |= std::uint64_t(bytes[i]) << (i * 8);
  return result;
}

std::uint32_t u32(const std::uint8_t* bytes) {
  std::uint32_t result = 0;
  for (unsigned i = 0; i < 4; ++i)
    result |= std::uint32_t(bytes[i]) << (i * 8);
  return result;
}

bool valid_sections(const Inventory& image) {
  if (image.sections.empty() || image.sections.size() > 96)
    return false;
  for (std::size_t i = 0; i < image.sections.size(); ++i) {
    const auto& section = image.sections[i];
    if (section.rva > image.image_size || section.size > image.image_size - section.rva)
      return false;
    for (std::size_t j = 0; j < i; ++j) {
      const auto& other = image.sections[j];
      if (section.size != 0 && other.size != 0 && section.rva < std::uint64_t(other.rva) + other.size &&
          other.rva < std::uint64_t(section.rva) + section.size)
        return false;
    }
  }
  return true;
}

bool section_range(const Inventory& image, std::uint32_t rva, std::uint32_t size, std::uint32_t required, std::uint32_t excluded) {
  if (rva >= image.image_size || size == 0 || size > image.image_size - rva)
    return false;
  return std::any_of(image.sections.begin(), image.sections.end(), [&](const ImageSection& section) {
    return (section.flags & required) == required && (section.flags & excluded) == 0 && rva >= section.rva &&
           rva - section.rva < section.size && size <= section.size - (rva - section.rva);
  });
}

bool normalize(const Inventory& image, std::uint64_t base, std::uint64_t address, std::uint32_t& rva) {
  if (address < base || address - base >= image.image_size)
    return false;
  rva = static_cast<std::uint32_t>(address - base);
  return true;
}

struct Observation {
  std::uint64_t address = 0;
  std::uint32_t size = 0;
  std::array<std::uint8_t, 16> bytes{};
};

struct Trace {
  std::array<Observation, 512> fields{};
  std::size_t size = 0;
};

class BoundedReader {
 public:
  BoundedReader(AircraftObjectReader& source, AircraftInventory& result) : source_(source), result_(result) {}

  bool field(std::uint64_t object, std::uint64_t offset, std::uint32_t size, std::uint8_t* output, Trace* trace = nullptr) {
    if (object == 0 || offset > std::numeric_limits<std::uint64_t>::max() - object || size == 0 || size > 16 ||
        size > std::numeric_limits<std::uint64_t>::max() - (object + offset)) {
      result_.error = "A fixed object field has a null or overflowing address.";
      return false;
    }
    if (size > kAircraftObjectReadBudget - result_.object_bytes || (trace != nullptr && trace->size == trace->fields.size())) {
      result_.error = "The bounded object-read or consistency-check budget was exhausted.";
      return false;
    }
    const auto address = object + offset;
    result_.object_bytes += size;
    if (!source_.read(address, output, size)) {
      ++result_.read_failures;
      result_.error = "A required fixed object field could not be read exactly.";
      return false;
    }
    if (trace != nullptr) {
      auto& observation = trace->fields[trace->size++];
      observation.address = address;
      observation.size = size;
      std::copy_n(output, size, observation.bytes.begin());
    }
    return true;
  }

  bool word(std::uint64_t object, std::uint64_t offset, std::uint64_t& value, Trace* trace = nullptr) {
    std::array<std::uint8_t, 8> bytes{};
    if (!field(object, offset, bytes.size(), bytes.data(), trace))
      return false;
    value = u64(bytes.data());
    return true;
  }

  bool dword(std::uint64_t object, std::uint64_t offset, std::uint32_t& value, Trace* trace = nullptr) {
    std::array<std::uint8_t, 4> bytes{};
    if (!field(object, offset, bytes.size(), bytes.data(), trace))
      return false;
    value = u32(bytes.data());
    return true;
  }

  // A readable null/stale handle is a valid unavailable reference; payload=0.
  // Read failures are distinct and must stop first-world selection.
  bool handle(std::uint64_t object, std::uint64_t offset, std::uint64_t& payload, Trace& trace) {
    payload = 0;
    std::array<std::uint8_t, 16> bytes{};
    if (!field(object, offset, bytes.size(), bytes.data(), &trace))
      return false;
    const auto control = u64(bytes.data());
    if (control == 0) {
      ++result_.null_handles;
      return true;
    }
    std::uint32_t generation = 0;
    if (!dword(control, 28, generation, &trace))
      return false;
    if (generation != u32(bytes.data() + 8)) {
      ++result_.stale_handles;
      return true;
    }
    if (!word(control, 0, payload, &trace))
      return false;
    if (payload == 0)
      ++result_.null_handles;
    return true;
  }

  bool recheck(const Trace& trace) {
    for (std::size_t i = 0; i < trace.size; ++i) {
      const auto& field_value = trace.fields[i];
      std::array<std::uint8_t, 16> bytes{};
      if (!field(field_value.address, 0, field_value.size, bytes.data()))
        return false;
      if (!std::equal(bytes.begin(), bytes.begin() + field_value.size, field_value.bytes.begin())) {
        result_.error = "A field in the selected object chain changed during the bounded capture.";
        return false;
      }
    }
    return true;
  }

 private:
  AircraftObjectReader& source_;
  AircraftInventory& result_;
};

bool count(BoundedReader& source,
           std::uint64_t object,
           std::uint64_t offset,
           std::int32_t& value,
           Trace& trace,
           AircraftInventory& result) {
  std::uint32_t raw = 0;
  if (!source.dword(object, offset, raw, &trace))
    return false;
  value = std::bit_cast<std::int32_t>(raw);
  if (value < 0 || value > 64) {
    result.error = "A fixed signed collection count is outside the permitted range 0..64.";
    return false;
  }
  return true;
}

AircraftInventory unavailable(AircraftInventory result, const char* stage) {
  result.valid = true;
  result.stage = stage;
  return result;
}

}  // namespace

AircraftInventory inspect_aircraft_metadata(ImageReader& reader,
                                            AircraftObjectReader& objects,
                                            const Inventory& image,
                                            std::uint64_t loaded_image_base,
                                            std::uint32_t expected_facade_vtable,
                                            bool inspect_selected_object,
                                            bool inspect_component,
                                            bool inspect_camera_keys,
                                            std::uint64_t* verified_source,
                                            std::uint64_t* verified_user) {
  if (verified_source != nullptr)
    *verified_source = 0;
  if (verified_user != nullptr)
    *verified_user = 0;
  AircraftInventory result;
  std::uint64_t aircraft_for_output = 0;
  if ((expected_facade_vtable != 0 && expected_facade_vtable != kAircraftExpectedFacadeVtableRva) ||
      (inspect_selected_object && expected_facade_vtable != kAircraftExpectedFacadeVtableRva) ||
      (inspect_component && !inspect_selected_object) || (inspect_camera_keys && !inspect_component) ||
      ((verified_source != nullptr || verified_user != nullptr) && !inspect_component)) {
    result.stage = "accessor_profile";
    result.error = "The accessor extension permits only the fixed captured facade vtable identity.";
    return result;
  }
  if (!image.valid_image || image.machine != 0x8664 || image.image_size == 0 || image.image_size > 0x80000000u ||
      image.section_count == 0 || image.section_count > 96 || image.sections.size() > image.section_count || !valid_sections(image) ||
      loaded_image_base == 0 || image.image_size > std::numeric_limits<std::uint64_t>::max() - loaded_image_base) {
    result.error = "Validated AMD64 section metadata and non-overflowing loaded-image bounds are required.";
    return result;
  }

  result.stage = "cached_global";
  if (!section_range(image, kAircraftGlobalRva, 8, kReadable | kWritable, kExecutable | kDiscardable)) {
    result.error = "The fixed world-container pointer is outside readable, writable, non-executable image data.";
    return result;
  }
  std::array<std::uint8_t, 8> bytes{};
  result.image_bytes = 8;
  if (!reader.read(kAircraftGlobalRva, bytes.data(), bytes.size())) {
    ++result.read_failures;
    result.error = "The fixed world-container pointer word is unreadable.";
    return result;
  }
  const auto container = u64(bytes.data());
  if (container == 0)
    return unavailable(result, "container_unavailable");
  result.cached_present = true;

  BoundedReader source(objects, result);
  Trace container_trace;
  result.stage = "world_count";
  if (!count(source, container, 20, result.world_count, container_trace, result))
    return result;
  if (result.world_count == 0)
    return unavailable(result, "no_worlds");
  std::uint64_t world_array = 0;
  result.stage = "world_array";
  if (!source.word(container, 24, world_array, &container_trace))
    return result;
  if (world_array == 0)
    return unavailable(result, "world_array_unavailable");

  Trace selected_trace;
  std::uint64_t selected_world = 0;
  for (std::int32_t index = 0; index < result.world_count; ++index) {
    ++result.worlds_examined;
    auto candidate = container_trace;
    std::uint64_t world = 0;
    result.stage = "world_handle";
    if (!source.handle(world_array, std::uint64_t(index) * 16, world, candidate))
      return result;
    if (world == 0)
      continue;
    std::uint64_t node = 0;
    result.stage = "viewport_node_handle";
    if (!source.handle(world, 40, node, candidate))
      return result;
    if (node == 0)
      continue;
    std::uint32_t viewport_count = 0;
    result.stage = "viewport_count";
    if (!source.dword(node, 120, viewport_count, &candidate))
      return result;
    if (viewport_count == 0)
      continue;
    std::uint32_t selector = 0;
    result.stage = "viewport_storage";
    if (!source.dword(node, 124, selector, &candidate))
      return result;
    std::uint32_t viewport = 0;
    result.stage = "viewport_id";
    if (std::bit_cast<std::int32_t>(selector) <= 9) {
      if (!source.dword(node, 80, viewport, &candidate))
        return result;
    } else {
      std::uint64_t viewport_array = 0;
      if (!source.word(node, 80, viewport_array, &candidate))
        return result;
      if (viewport_array == 0) {
        result.error = "A non-inline viewport ID requires a nonnull array pointer.";
        return result;
      }
      if (!source.dword(viewport_array, 0, viewport, &candidate))
        return result;
    }
    const auto viewport_id = std::bit_cast<std::int32_t>(viewport);
    if (viewport_id < 0)
      continue;
    selected_world = world;
    selected_trace = candidate;
    result.selected = true;
    result.selected_world_index = index;
    result.viewport_id = viewport_id;
    break;
  }
  if (!result.selected)
    return unavailable(result, "no_eligible_world");

  result.stage = "user_count";
  if (!count(source, selected_world, 736, result.user_count, selected_trace, result))
    return result;
  if (result.user_count == 0)
    return unavailable(result, "no_users");
  std::uint64_t user_array = 0;
  result.stage = "user_array";
  if (!source.word(selected_world, 784, user_array, &selected_trace))
    return result;
  if (user_array == 0)
    return unavailable(result, "user_array_unavailable");
  std::uint64_t user = 0;
  result.stage = "user_handle";
  if (!source.handle(user_array, 0, user, selected_trace))
    return result;
  if (user == 0)
    return unavailable(result, "user_unavailable");
  std::uint64_t facade = 0;
  result.stage = "facade_pointer";
  if (!source.word(user, 448, facade, &selected_trace))
    return result;
  if (facade == 0)
    return unavailable(result, "facade_unavailable");
  std::uint64_t vptr = 0;
  result.stage = "facade_vptr";
  if (!source.word(facade, 0, vptr, &selected_trace))
    return result;

  result.stage = "vtable_validation";
  if (!normalize(image, loaded_image_base, vptr, result.facade_vtable_rva) ||
      !section_range(image, result.facade_vtable_rva, kAircraftFacadeMethodOffset + 8, kReadable, kWritable | kExecutable | kDiscardable)) {
    result.error = "The facade vtable and fixed slot do not fit read-only, non-executable main-image data.";
    return result;
  }
  result.method_slot_rva = result.facade_vtable_rva + kAircraftFacadeMethodOffset;
  result.stage = "method_slot";
  result.image_bytes += 8;
  if (!reader.read(result.method_slot_rva, bytes.data(), bytes.size())) {
    ++result.read_failures;
    result.error = "The fixed facade method pointer word is unreadable.";
    return result;
  }
  result.stage = "method_target";
  if (!normalize(image, loaded_image_base, u64(bytes.data()), result.method_rva) ||
      !section_range(image, result.method_rva, 1, kReadable | kExecutable, kWritable | kDiscardable)) {
    result.error = "The facade method target is outside read-only, executable main-image code.";
    return result;
  }

  std::uint64_t renderer_for_recheck = 0;
  if (expected_facade_vtable != 0) {
    result.stage = "accessor_identity";
    if (result.facade_vtable_rva != kAircraftExpectedFacadeVtableRva || result.method_rva != kAircraftExpectedFacadeMethodRva) {
      result.error = "The resolved facade vtable and method do not match the captured accessor extension.";
      return result;
    }
    std::uint64_t object = 0;
    result.stage = "accessor_object_pointer";
    if (!source.word(facade, kAircraftAccessorObjectOffset, object, &selected_trace))
      return result;
    if (object == 0)
      return unavailable(result, "accessor_object_unavailable");
    result.object_present = true;
    std::uint64_t object_vptr = 0;
    result.stage = "accessor_object_vptr";
    if (!source.word(object, 0, object_vptr, &selected_trace))
      return result;
    result.stage = "accessor_vtable_validation";
    if (!normalize(image, loaded_image_base, object_vptr, result.object_vtable_rva) ||
        !section_range(image, result.object_vtable_rva, kAircraftAccessorMethodOffset + 8, kReadable,
                       kWritable | kExecutable | kDiscardable)) {
      result.error = "The accessor object vtable and fixed slot do not fit read-only, non-executable main-image data.";
      return result;
    }
    result.object_method_slot_rva = result.object_vtable_rva + kAircraftAccessorMethodOffset;
    result.stage = "accessor_method_slot";
    result.image_bytes += 8;
    if (!reader.read(result.object_method_slot_rva, bytes.data(), bytes.size())) {
      ++result.read_failures;
      result.error = "The fixed accessor method pointer word is unreadable.";
      return result;
    }
    result.stage = "accessor_method_target";
    if (!normalize(image, loaded_image_base, u64(bytes.data()), result.object_method_rva) ||
        !section_range(image, result.object_method_rva, 1, kReadable | kExecutable, kWritable | kDiscardable)) {
      result.error = "The accessor method target is outside read-only, executable main-image code.";
      return result;
    }

    if (inspect_selected_object) {
      result.stage = "controller_identity";
      if (result.object_vtable_rva != kAircraftExpectedControllerVtableRva ||
          result.object_method_rva != kAircraftExpectedControllerMethodRva) {
        result.error = "The controller vtable and method do not match the captured selected-object path.";
        return result;
      }
      std::uint32_t validity = 0;
      result.stage = "controller_validity";
      if (!source.dword(object, 676, validity, &selected_trace))
        return result;
      result.controller_validity = std::bit_cast<std::int32_t>(validity);
      if (result.controller_validity < 0)
        return unavailable(result, "controller_unavailable");

      result.stage = "renderer_cached_global";
      if (!section_range(image, kAircraftRendererGlobalRva, 8, kReadable | kWritable, kExecutable | kDiscardable)) {
        result.error = "The fixed renderer cache is outside readable, writable, non-executable main-image data.";
        return result;
      }
      result.image_bytes += 8;
      if (!reader.read(kAircraftRendererGlobalRva, bytes.data(), bytes.size())) {
        ++result.read_failures;
        result.error = "The fixed cached-renderer pointer word is unreadable.";
        return result;
      }
      renderer_for_recheck = u64(bytes.data());
      if (renderer_for_recheck == 0)
        return unavailable(result, "renderer_unavailable");
      std::uint8_t renderer_flag = 0;
      result.stage = "renderer_selection_flag";
      if (!source.field(renderer_for_recheck, 2800, 1, &renderer_flag, &selected_trace))
        return result;
      result.selected_object_index = 0;
      if (renderer_flag != 1) {
        std::uint32_t index = 0;
        result.stage = "controller_selected_index";
        if (!source.dword(object, 672, index, &selected_trace))
          return result;
        result.selected_object_index = std::bit_cast<std::int32_t>(index);
      }
      if (result.selected_object_index < 0 || result.selected_object_index > 63) {
        result.error = "The selected index exceeds the inspection cap 0..63; this cap is not a recovered engine array count.";
        return result;
      }

      std::uint64_t owner = 0;
      result.stage = "controller_owner_handle";
      if (!source.handle(object, 296, owner, selected_trace))
        return result;
      if (owner == 0)
        return unavailable(result, "controller_owner_unavailable");
      std::uint64_t selected_array = 0;
      result.stage = "selected_object_array";
      if (!source.word(owner, 752, selected_array, &selected_trace))
        return result;
      if (selected_array == 0)
        return unavailable(result, "selected_object_array_unavailable");
      std::uint64_t selected_object = 0;
      result.stage = "selected_object_handle";
      if (!source.handle(selected_array, std::uint64_t(result.selected_object_index) * 16, selected_object, selected_trace))
        return result;
      if (selected_object == 0)
        return unavailable(result, "selected_object_unavailable");
      result.selected_object_present = true;
      std::uint64_t selected_vptr = 0;
      result.stage = "selected_object_vptr";
      if (!source.word(selected_object, 0, selected_vptr, &selected_trace))
        return result;
      result.stage = "selected_object_vtable_validation";
      if (!normalize(image, loaded_image_base, selected_vptr, result.selected_object_vtable_rva) ||
          !section_range(image, result.selected_object_vtable_rva, kAircraftSelectedMethodOffset + 8, kReadable,
                         kWritable | kExecutable | kDiscardable)) {
        result.error = "The selected object's vtable and fixed slot do not fit read-only, non-executable main-image data.";
        return result;
      }
      result.selected_object_method_slot_rva = result.selected_object_vtable_rva + kAircraftSelectedMethodOffset;
      result.stage = "selected_object_method_slot";
      result.image_bytes += 8;
      if (!reader.read(result.selected_object_method_slot_rva, bytes.data(), bytes.size())) {
        ++result.read_failures;
        result.error = "The selected object's fixed method pointer word is unreadable.";
        return result;
      }
      result.stage = "selected_object_method_target";
      if (!normalize(image, loaded_image_base, u64(bytes.data()), result.selected_object_method_rva) ||
          !section_range(image, result.selected_object_method_rva, 1, kReadable | kExecutable, kWritable | kDiscardable)) {
        result.error = "The selected object's method target is outside read-only, executable main-image code.";
        return result;
      }

      if (inspect_component) {
        result.stage = "component_source_identity";
        if (result.selected_object_vtable_rva != kAircraftExpectedSelectedVtableRva ||
            result.selected_object_method_rva != kAircraftExpectedSelectedMethodRva) {
          result.error = "The selected object vtable and method do not match the captured aircraft-handle accessor.";
          return result;
        }
        std::uint64_t aircraft = 0;
        result.stage = "aircraft_handle";
        if (!source.handle(selected_object, 368, aircraft, selected_trace))
          return result;
        if (aircraft == 0)
          return unavailable(result, "aircraft_unavailable");
        aircraft_for_output = aircraft;
        result.aircraft_present = true;
        std::uint64_t aircraft_vptr = 0;
        result.stage = "aircraft_vptr";
        if (!source.word(aircraft, 0, aircraft_vptr, &selected_trace))
          return result;
        if (!normalize(image, loaded_image_base, aircraft_vptr, result.aircraft_vtable_rva) ||
            !section_range(image, result.aircraft_vtable_rva, 8, kReadable, kWritable | kExecutable | kDiscardable)) {
          result.error = "The aircraft vptr is outside read-only, non-executable main-image data.";
          return result;
        }
        std::uint64_t collection = 0;
        result.stage = "component_collection";
        if (!source.word(aircraft, 19272, collection, &selected_trace))
          return result;
        if (collection == 0)
          return unavailable(result, "component_collection_unavailable");
        result.stage = "component_count";
        if (!count(source, collection, 36, result.component_count, selected_trace, result))
          return result;
        if (result.component_count == 0)
          return unavailable(result, "no_components");
        std::uint64_t component_array = 0;
        result.stage = "component_array";
        if (!source.word(collection, 40, component_array, &selected_trace))
          return result;
        if (component_array == 0)
          return unavailable(result, "component_array_unavailable");

        std::uint64_t component = 0;
        for (std::int32_t index = 0; index < result.component_count; ++index) {
          std::uint64_t candidate = 0;
          result.stage = "component_pointer";
          if (!source.word(component_array, std::uint64_t(index) * 8, candidate, &selected_trace))
            return result;
          if (candidate == 0)
            continue;
          std::uint32_t type = 0;
          result.stage = "component_type";
          if (!source.dword(candidate, 32, type, &selected_trace))
            return result;
          if (type == 5) {
            component = candidate;
            result.selected_component_index = index;
            break;
          }
        }
        if (component == 0)
          return unavailable(result, "component_unavailable");
        result.component_present = true;
        std::uint64_t component_vptr = 0;
        result.stage = "component_vptr";
        if (!source.word(component, 0, component_vptr, &selected_trace))
          return result;
        if (!normalize(image, loaded_image_base, component_vptr, result.component_vtable_rva) ||
            !section_range(image, result.component_vtable_rva, 8, kReadable, kWritable | kExecutable | kDiscardable)) {
          result.error = "The matched component vptr is outside read-only, non-executable main-image data.";
          return result;
        }

        if (inspect_camera_keys) {
          result.stage = "camera_key_component_identity";
          if (result.component_vtable_rva != kAircraftExpectedKeyComponentVtableRva) {
            result.error = "The resolved component vtable does not match the fixed zero-adjustment key-lookup profile.";
            return result;
          }
          result.stage = "camera_key_count";
          if (!source.dword(component, 108, result.camera_key_count, &selected_trace))
            return result;
          if (result.camera_key_count > 64) {
            result.error = "The unsigned camera-key count exceeds the bounded inspection range 0..64.";
            return result;
          }
          if (result.camera_key_count != 0) {
            std::uint64_t key_array = 0;
            result.stage = "camera_key_array";
            if (!source.word(component, 112, key_array, &selected_trace))
              return result;
            if (key_array == 0) {
              result.error = "A nonempty camera-key collection has a null pointer array; completeness is refused.";
              return result;
            }
            for (std::uint32_t index = 0; index < result.camera_key_count; ++index) {
              std::uint64_t record = 0;
              result.stage = "camera_key_record";
              if (!source.word(key_array, std::uint64_t(index) * 8, record, &selected_trace))
                return result;
              if (record == 0) {
                result.error = "A camera-key record pointer is null; complete enumeration is refused.";
                return result;
              }
              std::array<std::uint8_t, 16> key{};
              result.stage = "camera_key_bytes";
              if (!source.field(record, 72, key.size(), key.data(), &selected_trace))
                return result;
              if (key == kTailKey) {
                ++result.tail_matches;
                if (result.first_tail_match_index < 0)
                  result.first_tail_match_index = static_cast<std::int32_t>(index);
              }
              if (key == kGearKey) {
                ++result.gear_matches;
                if (result.first_gear_match_index < 0)
                  result.first_gear_match_index = static_cast<std::int32_t>(index);
              }
            }
          }
        }
      }
    }
  }

  result.stage = "consistency_recheck";
  if (!source.recheck(selected_trace))
    return result;
  result.stage = "cached_global_recheck";
  result.image_bytes += 8;
  if (!reader.read(kAircraftGlobalRva, bytes.data(), bytes.size())) {
    ++result.read_failures;
    result.error = "The cached world-container pointer could not be reread exactly.";
    return result;
  }
  if (u64(bytes.data()) != container) {
    result.error = "The cached world-container pointer changed during the bounded capture.";
    return result;
  }
  if (inspect_selected_object) {
    result.stage = "renderer_cached_global_recheck";
    result.image_bytes += 8;
    if (!reader.read(kAircraftRendererGlobalRva, bytes.data(), bytes.size())) {
      ++result.read_failures;
      result.error = "The cached-renderer pointer could not be reread exactly.";
      return result;
    }
    if (u64(bytes.data()) != renderer_for_recheck) {
      result.error = "The cached-renderer pointer changed during the bounded capture.";
      return result;
    }
  }
  result.valid = true;
  result.available = true;
  result.camera_keys_inspected = inspect_camera_keys;
  result.stage = "complete";
  if (verified_source != nullptr)
    *verified_source = aircraft_for_output;
  if (verified_user != nullptr)
    *verified_user = user;
  return result;
}

}  // namespace taxi_camera::discovery
