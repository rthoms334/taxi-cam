#include "../../src/camera/aircraft_inventory.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <utility>

namespace {
using namespace taxi_camera::discovery;

unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}

constexpr std::uint64_t Base = 0x140000000;
constexpr std::uint64_t Container = 0x10000000000;
constexpr std::uint64_t Worlds = Container + 0x1000;
constexpr std::uint64_t UserArray = Container + 0x2000;
constexpr std::uint64_t UserControl = Container + 0x3000;
constexpr std::uint64_t User = Container + 0x4000;
constexpr std::uint64_t Facade = Container + 0x5000;
constexpr std::uint32_t Vtable = 0x1000;
constexpr std::uint32_t Method = 0x3000;
constexpr std::uint64_t AccessorObject = Container + 0x6000;
constexpr std::uint32_t ObjectVtable = 0x5000;
constexpr std::uint32_t ObjectMethod = 0x7000;
constexpr std::uint64_t Renderer = Container + 0x7000;
constexpr std::uint64_t OwnerControl = Container + 0x8000;
constexpr std::uint64_t Owner = Container + 0x9000;
constexpr std::uint64_t SelectedArray = Container + 0xa000;
constexpr std::uint64_t SelectedControl = Container + 0xb000;
constexpr std::uint64_t SelectedObject = Container + 0xc000;
constexpr std::uint32_t SelectedVtable = 0x9000;
constexpr std::uint32_t SelectedMethod = 0xb000;
constexpr std::uint64_t AircraftControl = Container + 0x1000000;
constexpr std::uint64_t Aircraft = Container + 0x1001000;
constexpr std::uint64_t ComponentCollection = Container + 0x1010000;
constexpr std::uint64_t ComponentArray = Container + 0x1011000;
constexpr std::uint64_t FirstComponent = Container + 0x1020000;
constexpr std::uint32_t AircraftVtable = 0xd000;
constexpr std::uint32_t ComponentVtable = 0xf000;
constexpr std::uint64_t CameraKeyArray = Container + 0x2000000;
constexpr std::uint64_t FirstCameraRecord = Container + 0x2100000;

// Construct the test bytes from canonical config strings, independently of the
// production byte arrays. GUID.ToByteArray reverses the first 4/2/2-byte fields.
std::array<std::uint8_t, 16> guid_bytes(const char* canonical) {
  std::array<std::uint8_t, 16> bytes{};
  unsigned nibble_count = 0;
  for (const char* p = canonical; *p != '\0'; ++p) {
    if (*p == '-')
      continue;
    const unsigned nibble = *p >= '0' && *p <= '9' ? unsigned(*p - '0') : unsigned(*p - 'A') + 10;
    require(nibble < 16 && nibble_count < 32, "Invalid synthetic canonical GUID");
    auto& byte = bytes[nibble_count / 2];
    byte = static_cast<std::uint8_t>((byte << 4) | nibble);
    ++nibble_count;
  }
  require(nibble_count == 32, "Synthetic GUID did not contain 16 bytes");
  std::reverse(bytes.begin(), bytes.begin() + 4);
  std::reverse(bytes.begin() + 4, bytes.begin() + 6);
  std::reverse(bytes.begin() + 6, bytes.begin() + 8);
  return bytes;
}

const auto TailKey = guid_bytes("F6B3984D-4D58-41C4-8BA8-32CD24CCC13B");
const auto GearKey = guid_bytes("89B25433-6454-43DF-9B91-BF338A906A19");

std::uint64_t world_control(unsigned index) {
  return Container + 0x10000 + index * 0x4000;
}
std::uint64_t world(unsigned index) {
  return world_control(index) + 0x1000;
}
std::uint64_t node_control(unsigned index) {
  return world_control(index) + 0x2000;
}
std::uint64_t node(unsigned index) {
  return world_control(index) + 0x3000;
}

struct SparseMemory {
  std::map<std::uint64_t, std::uint8_t> bytes;
  std::vector<std::pair<std::uint64_t, std::size_t>> reads;
  std::map<std::uint64_t, unsigned> occurrences;
  std::uint64_t failed_address = 0;
  std::uint64_t change_on_repeat = 0;
  std::uint64_t fail_on_repeat = 0;
  std::size_t attempted = 0;

  void integer(std::uint64_t address, std::uint64_t value, unsigned size = 8) {
    for (unsigned i = 0; i < size; ++i)
      bytes[address + i] = static_cast<std::uint8_t>(value >> (i * 8));
  }
  void handle(std::uint64_t address, std::uint64_t control, std::uint64_t payload, std::uint32_t generation = 7) {
    integer(address, control);
    integer(address + 8, generation);
    if (control != 0) {
      integer(control, payload);
      integer(control + 28, generation, 4);
    }
  }
  bool read(std::uint64_t address, void* output, std::size_t size) {
    attempted += size;
    reads.emplace_back(address, size);
    const auto occurrence = ++occurrences[address];
    if (address == failed_address || (address == fail_on_repeat && occurrence > 1))
      return false;
    auto* destination = static_cast<std::uint8_t*>(output);
    for (std::size_t i = 0; i < size; ++i) {
      const auto value = bytes.find(address + i);
      if (value == bytes.end())
        return false;
      destination[i] = value->second;
    }
    if (address == change_on_repeat && occurrence > 1)
      destination[0] ^= 1;
    return true;
  }
};

struct Image final : ImageReader {
  SparseMemory memory;
  std::uint32_t facade_vtable = Vtable;
  std::uint32_t accessor_vtable = ObjectVtable;
  std::uint32_t selected_vtable = SelectedVtable;
  bool accessor_slot_allowed = false;
  bool selected_slot_allowed = false;
  ReadWindow query(std::uint32_t, std::uint32_t) override { return {}; }
  bool read(std::uint32_t rva, void* output, std::size_t size) override {
    require(size == 8 &&
                (rva == kAircraftGlobalRva || rva == facade_vtable + kAircraftFacadeMethodOffset ||
                 (accessor_slot_allowed && rva == accessor_vtable + kAircraftAccessorMethodOffset) ||
                 (selected_slot_allowed && (rva == kAircraftRendererGlobalRva || rva == selected_vtable + kAircraftSelectedMethodOffset))),
            "Core accessed image data beyond the profile's fixed pointer words");
    return memory.read(rva, output, size);
  }
};

struct Objects final : AircraftObjectReader {
  SparseMemory memory;
  bool renderer_byte_allowed = false;
  bool read(std::uint64_t address, void* output, std::size_t size) override {
    require(size == 4 || size == 8 || size == 16 || (renderer_byte_allowed && size == 1 && address == Renderer + 2800),
            "An object read used an unapproved size or byte address");
    require(memory.attempted + size <= kAircraftObjectReadBudget, "Core exceeded its attempted object-read budget");
    return memory.read(address, output, size);
  }
};

Inventory metadata() {
  Inventory result;
  result.valid_image = true;
  result.machine = 0x8664;
  result.timestamp = 1787653788;
  result.image_size = 235963904;
  result.section_count = 14;
  result.sections = {
      {".rdata", Vtable, 0x1000, 0x40000040}, {".text", Method, 0x1000, 0x60000020}, {".data", kAircraftGlobalRva, 8, 0xc0000040}};
  return result;
}

struct Fixture {
  Image image;
  Objects objects;
  Inventory info = metadata();

  Fixture() {
    image.memory.integer(kAircraftGlobalRva, Container);
    image.memory.integer(Vtable + kAircraftFacadeMethodOffset, Base + Method);
    objects.memory.integer(Container + 20, 1, 4);
    objects.memory.integer(Container + 24, Worlds);
    add_world(0);
    objects.memory.handle(UserArray, UserControl, User);
    objects.memory.integer(User + 448, Facade);
    objects.memory.integer(Facade, Base + Vtable);
  }
  void add_world(unsigned index, std::int32_t viewport = 5, std::int32_t selector = 9) {
    auto& memory = objects.memory;
    memory.handle(Worlds + index * 16, world_control(index), world(index));
    memory.handle(world(index) + 40, node_control(index), node(index));
    memory.integer(node(index) + 120, 1, 4);
    memory.integer(node(index) + 124, static_cast<std::uint32_t>(selector), 4);
    if (selector <= 9) {
      memory.integer(node(index) + 80, static_cast<std::uint32_t>(viewport), 4);
    } else {
      const auto ids = node(index) + 0x400;
      memory.integer(node(index) + 80, ids);
      memory.integer(ids, static_cast<std::uint32_t>(viewport), 4);
    }
    memory.integer(world(index) + 736, 1, 4);
    memory.integer(world(index) + 784, UserArray);
  }
  void add_accessor_object() {
    image.facade_vtable = kAircraftExpectedFacadeVtableRva;
    image.accessor_slot_allowed = true;
    info.sections[0].rva = kAircraftExpectedFacadeVtableRva;
    info.sections[1].rva = kAircraftExpectedFacadeMethodRva;
    image.memory.integer(kAircraftExpectedFacadeVtableRva + kAircraftFacadeMethodOffset, Base + kAircraftExpectedFacadeMethodRva);
    objects.memory.integer(Facade, Base + kAircraftExpectedFacadeVtableRva);
    objects.memory.integer(Facade + kAircraftAccessorObjectOffset, AccessorObject);
    objects.memory.integer(AccessorObject, Base + ObjectVtable);
    info.sections.push_back({".rdata2", ObjectVtable, 0x1000, 0x40000040});
    info.sections.push_back({".text2", ObjectMethod, 0x1000, 0x60000020});
    image.memory.integer(ObjectVtable + kAircraftAccessorMethodOffset, Base + ObjectMethod);
  }
  void add_selected_object(std::uint8_t renderer_flag = 1, std::int32_t index = 2) {
    add_accessor_object();
    image.accessor_vtable = kAircraftExpectedControllerVtableRva;
    image.selected_slot_allowed = true;
    objects.renderer_byte_allowed = true;
    info.sections[3].rva = kAircraftExpectedControllerVtableRva;
    info.sections[4].rva = kAircraftExpectedControllerMethodRva;
    objects.memory.integer(AccessorObject, Base + kAircraftExpectedControllerVtableRva);
    image.memory.integer(kAircraftExpectedControllerVtableRva + kAircraftAccessorMethodOffset, Base + kAircraftExpectedControllerMethodRva);
    objects.memory.integer(AccessorObject + 676, 0, 4);
    objects.memory.integer(AccessorObject + 672, static_cast<std::uint32_t>(index), 4);
    image.memory.integer(kAircraftRendererGlobalRva, Renderer);
    info.sections.push_back({".rdata3", SelectedVtable, 0x1000, 0x40000040});
    info.sections.push_back({".text3", SelectedMethod, 0x1000, 0x60000020});
    info.sections.push_back({".renderer", kAircraftRendererGlobalRva, 8, 0xc0000040});
    objects.memory.integer(Renderer + 2800, renderer_flag, 1);
    objects.memory.handle(AccessorObject + 296, OwnerControl, Owner);
    objects.memory.integer(Owner + 752, SelectedArray);
    if (index >= 0 && index <= 63)
      objects.memory.handle(SelectedArray + (renderer_flag == 1 ? 0 : std::uint64_t(index) * 16), SelectedControl, SelectedObject);
    objects.memory.integer(SelectedObject, Base + SelectedVtable);
    image.memory.integer(SelectedVtable + kAircraftSelectedMethodOffset, Base + SelectedMethod);
  }
  void add_components(std::uint32_t count = 1, std::uint32_t matched_index = 0) {
    add_selected_object(0, 2);
    image.selected_vtable = kAircraftExpectedSelectedVtableRva;
    info.sections[5].rva = kAircraftExpectedSelectedVtableRva;
    info.sections[6].rva = kAircraftExpectedSelectedMethodRva;
    objects.memory.integer(SelectedObject, Base + kAircraftExpectedSelectedVtableRva);
    image.memory.integer(kAircraftExpectedSelectedVtableRva + kAircraftSelectedMethodOffset, Base + kAircraftExpectedSelectedMethodRva);
    objects.memory.handle(SelectedObject + 368, AircraftControl, Aircraft);
    objects.memory.integer(Aircraft, Base + AircraftVtable);
    objects.memory.integer(Aircraft + 19272, ComponentCollection);
    objects.memory.integer(ComponentCollection + 36, count, 4);
    objects.memory.integer(ComponentCollection + 40, ComponentArray);
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto component = FirstComponent + i * 0x1000;
      objects.memory.integer(ComponentArray + i * 8, component);
      objects.memory.integer(component + 32, i == matched_index ? 5 : 1, 4);
      objects.memory.integer(component, Base + ComponentVtable);
    }
    info.sections.push_back({".aircraft", AircraftVtable, 0x1000, 0x40000040});
    info.sections.push_back({".component", ComponentVtable, 0x1000, 0x40000040});
  }
  void add_camera_keys(std::uint32_t count, std::uint32_t component_count = 1, std::uint32_t component_index = 0) {
    add_components(component_count, component_index);
    const auto component = FirstComponent + component_index * 0x1000;
    info.sections[9].rva = kAircraftExpectedKeyComponentVtableRva;
    objects.memory.integer(component, Base + kAircraftExpectedKeyComponentVtableRva);
    objects.memory.integer(component + 108, count, 4);
    objects.memory.integer(component + 112, CameraKeyArray);
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto record = FirstCameraRecord + i * 0x1000;
      objects.memory.integer(CameraKeyArray + i * 8, record);
      objects.memory.integer(record + 72, 0);
      objects.memory.integer(record + 80, 0);
    }
  }
  void camera_key(std::uint32_t index, const std::array<std::uint8_t, 16>& key) {
    for (std::size_t i = 0; i < key.size(); ++i)
      objects.memory.bytes[FirstCameraRecord + index * 0x1000 + 72 + i] = key[i];
  }
  AircraftInventory run(std::uint64_t base = Base,
                        std::uint32_t expected = 0,
                        bool selected_object = false,
                        bool component = false,
                        bool camera_keys = false,
                        std::uint64_t* verified_source = nullptr,
                        std::uint64_t* verified_user = nullptr) {
    const auto result = inspect_aircraft_metadata(image, objects, info, base, expected, selected_object, component, camera_keys,
                                                  verified_source, verified_user);
    require(result.image_bytes == image.memory.attempted && result.image_bytes <= (selected_object ? 56u
                                                                                   : expected == 0 ? 24u
                                                                                                   : 32u),
            "Image read accounting mismatch");
    require(result.object_bytes == objects.memory.attempted && result.object_bytes <= 8192, "Object read accounting mismatch");
    return result;
  }
};

void valid_paths() {
  for (const auto selector : {std::numeric_limits<std::int32_t>::min(), -1, 0, 9, 10, 100}) {
    Fixture fixture;
    fixture.add_world(0, 23, selector);
    const auto result = fixture.run();
    require(result.valid && result.available && result.cached_present && result.selected && result.stage == "complete" &&
                result.error.empty() && result.world_count == 1 && result.worlds_examined == 1 && result.selected_world_index == 0 &&
                result.user_count == 1 && result.viewport_id == 23 && result.facade_vtable_rva == Vtable &&
                result.method_slot_rva == Vtable + kAircraftFacadeMethodOffset && result.method_rva == Method && result.read_failures == 0,
            "Valid first-world facade metadata was not resolved");
    for (const auto address : {Container + 20, Container + 24, Worlds, world_control(0), world_control(0) + 28, world(0) + 40,
                               node_control(0), node_control(0) + 28, UserArray, UserControl, UserControl + 28, User + 448, Facade}) {
      require(fixture.objects.memory.occurrences[address] == 2, "Selected chain field was not reread exactly once");
    }
  }
}

void build_refusal() {
  {
    Fixture fixture;
    ++fixture.info.timestamp;
    fixture.info.image_size += 4096;
    ++fixture.info.section_count;
    const auto result = fixture.run();
    require(result.valid && result.available, "Compatible aircraft layout was rejected solely for changed build metadata");
  }
  for (unsigned scenario = 0; scenario < 10; ++scenario) {
    Fixture fixture;
    auto base = Base;
    if (scenario == 0)
      fixture.info.valid_image = false;
    if (scenario == 1)
      fixture.info.machine = 0x14c;
    if (scenario == 2)
      fixture.info.section_count = 97;
    if (scenario == 3)
      fixture.info.image_size = 0;
    if (scenario == 4)
      fixture.info.section_count = 0;
    if (scenario == 5)
      fixture.info.sections[1].rva = Vtable + 4;
    if (scenario == 6)
      fixture.info.sections[0].size = 0xffffffff;
    if (scenario == 7)
      fixture.info.sections.clear();
    if (scenario == 8)
      base = 0;
    if (scenario == 9)
      base = std::numeric_limits<std::uint64_t>::max() - 1;
    const auto result = fixture.run(base);
    require(!result.valid && !result.available && !result.error.empty() && result.stage == "image_validation" &&
                fixture.image.memory.reads.empty() && fixture.objects.memory.reads.empty(),
            "Wrong build or malformed image metadata accessed memory");
  }
  for (const auto flags : {0u, 0x40000040u, 0xe0000040u, 0xc2000040u}) {
    Fixture fixture;
    fixture.info.sections[2].flags = flags;
    const auto result = fixture.run();
    require(!result.valid && result.stage == "cached_global" && fixture.image.memory.reads.empty(),
            "Cached-global storage did not require writable non-executable image data");
  }
}

void absence_and_selection() {
  {
    Fixture fixture;
    fixture.image.memory.integer(kAircraftGlobalRva, 0);
    const auto result = fixture.run();
    require(
        result.valid && !result.available && !result.cached_present && result.stage == "container_unavailable" && result.object_bytes == 0,
        "Null cached container was dereferenced or reported as a resolved facade");
  }
  for (const auto scenario : {0, 1, 2, 3, 4, 5}) {
    Fixture fixture;
    if (scenario == 0)
      fixture.objects.memory.integer(Container + 20, 0, 4);
    if (scenario == 1)
      fixture.objects.memory.integer(Container + 24, 0);
    if (scenario == 2)
      fixture.objects.memory.integer(world(0) + 736, 0, 4);
    if (scenario == 3)
      fixture.objects.memory.integer(world(0) + 784, 0);
    if (scenario == 4)
      fixture.objects.memory.integer(UserArray, 0);
    if (scenario == 5)
      fixture.objects.memory.integer(User + 448, 0);
    const auto result = fixture.run();
    require(result.valid && !result.available && result.error.empty() && result.image_bytes == 8,
            "Unavailable collection/user/facade was falsely resolved");
  }
  // First valid world has no user. A later world with a user must not be chosen.
  {
    Fixture fixture;
    fixture.objects.memory.integer(Container + 20, 2, 4);
    fixture.add_world(1, 9);
    fixture.objects.memory.integer(world(0) + 736, 0, 4);
    const auto result = fixture.run();
    require(result.valid && !result.available && result.selected && result.selected_world_index == 0 && result.worlds_examined == 1 &&
                fixture.objects.memory.occurrences[Worlds + 16] == 0,
            "Enumeration skipped an eligible world's unavailable user to choose another world");
  }
  // Null world, stale world, null node, stale node, empty node and negative ID
  // are skipped. First positive ID wins; no further world is touched.
  {
    Fixture fixture;
    fixture.objects.memory.integer(Container + 20, 8, 4);
    for (unsigned i = 1; i < 8; ++i)
      fixture.add_world(i, static_cast<std::int32_t>(i));
    fixture.objects.memory.integer(Worlds, 0);
    fixture.objects.memory.integer(world_control(1) + 28, 8, 4);
    fixture.objects.memory.integer(world(2) + 40, 0);
    fixture.objects.memory.integer(node_control(3) + 28, 8, 4);
    fixture.objects.memory.integer(node(4) + 120, 0, 4);
    fixture.objects.memory.integer(node(5) + 80, 0xffffffff, 4);
    const auto result = fixture.run();
    require(result.valid && result.available && result.selected_world_index == 6 && result.viewport_id == 6 &&
                result.worlds_examined == 7 && result.null_handles == 2 && result.stale_handles == 2 &&
                fixture.objects.memory.occurrences[Worlds + 7 * 16] == 0,
            "First-world selection did not distinguish null, stale, empty and negative predicates");
  }
  {
    Fixture fixture;
    fixture.objects.memory.integer(UserControl + 28, 8, 4);
    const auto result = fixture.run();
    require(result.valid && !result.available && result.selected && result.stale_handles == 1 && result.stage == "user_unavailable",
            "Stale selected user was treated as a callable facade");
  }
}

void bounds_and_read_failures() {
  for (const auto offset : {20u, 736u}) {
    for (const auto raw : {0xffffffffu, 65u, 0x7fffffffu}) {
      Fixture fixture;
      fixture.objects.memory.integer((offset == 20 ? Container : world(0)) + offset, raw, 4);
      const auto result = fixture.run();
      require(!result.valid && !result.error.empty() && !result.available && result.image_bytes == 8,
              "Negative or excessive signed collection count was accepted");
    }
  }
  for (const auto address : {kAircraftGlobalRva, Vtable + kAircraftFacadeMethodOffset}) {
    Fixture fixture;
    fixture.image.memory.failed_address = address;
    const auto result = fixture.run();
    require(!result.valid && result.read_failures == 1 && !result.available, "Image read failure was ignored");
  }
  for (const auto address : {Container + 20, Container + 24, Worlds, world_control(0) + 28, world_control(0), world(0) + 40,
                             node_control(0) + 28, node_control(0), node(0) + 120, node(0) + 124, node(0) + 80, world(0) + 736,
                             world(0) + 784, UserArray, UserControl + 28, UserControl, User + 448, Facade}) {
    Fixture fixture;
    fixture.objects.memory.failed_address = address;
    const auto result = fixture.run();
    require(!result.valid && result.read_failures == 1 && !result.available && !result.error.empty(),
            "A fixed object read failure was ignored");
  }
  for (unsigned scenario = 0; scenario < 5; ++scenario) {
    Fixture fixture;
    const auto near_end = std::numeric_limits<std::uint64_t>::max() - 2;
    if (scenario == 0)
      fixture.image.memory.integer(kAircraftGlobalRva, near_end);
    if (scenario == 1)
      fixture.objects.memory.integer(Container + 24, near_end);
    if (scenario == 2)
      fixture.objects.memory.integer(Worlds, near_end);
    if (scenario == 3)
      fixture.objects.memory.integer(User + 448, near_end);
    if (scenario == 4) {
      fixture.add_world(0, 5, 10);
      fixture.objects.memory.integer(node(0) + 80, near_end);
    }
    const auto result = fixture.run();
    require(!result.valid && result.read_failures == 0 && !result.error.empty(), "Overflowing fixed field reached a reader");
    require(std::none_of(fixture.objects.memory.reads.begin(), fixture.objects.memory.reads.end(),
                         [&](const auto& read) { return read.first >= near_end; }),
            "An overflowing address was read");
  }
}

void static_method_validation() {
  for (const auto flags : {0u, 0xc0000040u, 0x60000040u, 0x42000040u}) {
    Fixture fixture;
    fixture.info.sections[0].flags = flags;
    const auto result = fixture.run();
    require(!result.valid && !result.available && result.stage == "vtable_validation" && result.image_bytes == 8,
            "Writable/executable/unreadable/discardable facade vtable accepted");
  }
  for (const auto flags : {0u, 0x40000040u, 0xe0000020u, 0x62000020u}) {
    Fixture fixture;
    fixture.info.sections[1].flags = flags;
    const auto result = fixture.run();
    require(!result.valid && !result.available && result.stage == "method_target", "Invalid method code section accepted");
  }
  for (const auto address : {std::uint64_t{0}, Base - 1, Base + 235963904, std::numeric_limits<std::uint64_t>::max()}) {
    for (const bool invalid_vptr : {true, false}) {
      Fixture fixture;
      if (invalid_vptr)
        fixture.objects.memory.integer(Facade, address);
      else
        fixture.image.memory.integer(Vtable + kAircraftFacadeMethodOffset, address);
      const auto result = fixture.run();
      require(!result.valid && !result.available && !result.error.empty(), "Out-of-image vtable or method target was normalized");
    }
  }
  Fixture fixture;
  fixture.info.sections[0].size = kAircraftFacadeMethodOffset + 7;
  const auto result = fixture.run();
  require(!result.valid && result.stage == "vtable_validation" && result.image_bytes == 8,
          "Method slot crossed static vtable storage bounds");
}

void consistency_and_budget() {
  Fixture observed;
  const auto original = observed.run();
  require(original.valid && original.available, "Could not establish consistency fixture");
  for (const auto& [address, occurrences] : observed.objects.memory.occurrences) {
    require(occurrences == 2, "Successful snapshot left an object field outside its consistency recheck");
    Fixture changed;
    changed.objects.memory.change_on_repeat = address;
    const auto result = changed.run();
    require(
        !result.valid && !result.available && result.stage == "consistency_recheck" && result.read_failures == 0 && !result.error.empty(),
        "Changed selected object/control/generation field survived recheck");
  }
  for (const bool failed_read : {false, true}) {
    Fixture changed;
    if (failed_read)
      changed.image.memory.fail_on_repeat = kAircraftGlobalRva;
    else
      changed.image.memory.change_on_repeat = kAircraftGlobalRva;
    const auto result = changed.run();
    require(!result.valid && !result.available && result.stage == "cached_global_recheck" && result.image_bytes == 24 &&
                result.read_failures == (failed_read ? 1u : 0u) && !result.error.empty(),
            "Changed or unreadable cached-global recheck survived or lost failed-attempt accounting");
  }
  Fixture largest;
  largest.objects.memory.integer(Container + 20, 64, 4);
  for (unsigned i = 0; i < 64; ++i)
    largest.add_world(i, i == 63 ? 77 : -1, 10);
  largest.objects.memory.integer(world(63) + 736, 64, 4);
  const auto result = largest.run();
  require(result.valid && result.available && result.worlds_examined == 64 && result.selected_world_index == 63 &&
              result.user_count == 64 && result.object_bytes < 8192 && result.image_bytes == 24 &&
              largest.objects.memory.occurrences[UserArray + 16] == 0 && largest.objects.memory.occurrences[Worlds + 64 * 16] == 0,
          "Maximum world count exceeded budget, read later users or scanned beyond the bounded world array");
  std::printf("Maximum fixture: %u object bytes and %u image bytes, including consistency rereads.\n", result.object_bytes,
              result.image_bytes);
}

void accessor_extension() {
  {
    Fixture fixture;
    fixture.add_accessor_object();
    const auto result = fixture.run();
    require(result.valid && result.available && !result.object_present && result.object_vtable_rva == 0 && result.object_method_rva == 0 &&
                result.image_bytes == 24 && result.object_bytes == 272 &&
                fixture.objects.memory.occurrences[Facade + kAircraftAccessorObjectOffset] == 0 &&
                fixture.image.memory.occurrences[ObjectVtable + kAircraftAccessorMethodOffset] == 0,
            "Default mode gained accessor-object reads");
  }
  {
    Fixture fixture;
    fixture.add_accessor_object();
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva);
    require(result.valid && result.available && result.object_present && result.object_vtable_rva == ObjectVtable &&
                result.object_method_slot_rva == ObjectVtable + kAircraftAccessorMethodOffset && result.object_method_rva == ObjectMethod &&
                result.image_bytes == 32 && result.object_bytes == 304 && result.read_failures == 0 &&
                fixture.objects.memory.occurrences[Facade + kAircraftAccessorObjectOffset] == 2 &&
                fixture.objects.memory.occurrences[AccessorObject] == 2,
            "Fixed accessor metadata was not resolved and rechecked with exact budgets");
  }
  for (const auto expected : {1u, kAircraftExpectedFacadeVtableRva + 1, 0xffffffffu}) {
    Fixture fixture;
    fixture.add_accessor_object();
    const auto result = fixture.run(Base, expected);
    require(
        !result.valid && result.stage == "accessor_profile" && !result.error.empty() && result.image_bytes == 0 && result.object_bytes == 0,
        "An arbitrary facade profile accessed memory");
  }
  for (const bool wrong_method : {false, true}) {
    Fixture fixture;
    if (wrong_method) {
      fixture.add_accessor_object();
      fixture.image.memory.integer(kAircraftExpectedFacadeVtableRva + kAircraftFacadeMethodOffset,
                                   Base + kAircraftExpectedFacadeMethodRva + 1);
    }
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva);
    require(!result.valid && result.stage == "accessor_identity" && !result.object_present &&
                fixture.objects.memory.occurrences[Facade + kAircraftAccessorObjectOffset] == 0,
            "Mismatching facade table/method accessed its unverified object field");
  }
  {
    Fixture fixture;
    fixture.add_accessor_object();
    fixture.objects.memory.integer(Facade + kAircraftAccessorObjectOffset, 0);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva);
    require(result.valid && !result.available && !result.object_present && result.stage == "accessor_object_unavailable" &&
                result.method_rva == kAircraftExpectedFacadeMethodRva && result.object_method_rva == 0 && result.read_failures == 0,
            "Null intermediate accessor object was dereferenced or treated as a resolved method");
  }
  for (const auto address : {Facade + kAircraftAccessorObjectOffset, AccessorObject}) {
    for (const bool changes : {false, true}) {
      Fixture fixture;
      fixture.add_accessor_object();
      if (changes)
        fixture.objects.memory.change_on_repeat = address;
      else
        fixture.objects.memory.failed_address = address;
      const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva);
      require(!result.valid && !result.available && !result.error.empty() && result.read_failures == (changes ? 0u : 1u) &&
                  (!changes || result.stage == "consistency_recheck"),
              "Unreadable or changed accessor fields survived validation");
    }
  }
  for (const auto flags : {0xc0000040u, 0x60000040u, 0x42000040u}) {
    Fixture fixture;
    fixture.add_accessor_object();
    fixture.info.sections[3].flags = flags;
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva);
    require(!result.valid && result.stage == "accessor_vtable_validation" && result.image_bytes == 16,
            "Accessor vtable permitted writable, executable or discarded image storage");
  }
  for (unsigned scenario = 0; scenario < 6; ++scenario) {
    Fixture fixture;
    fixture.add_accessor_object();
    if (scenario == 0)
      fixture.objects.memory.integer(AccessorObject, Base - 1);
    if (scenario == 1)
      fixture.info.sections[3].size = kAircraftAccessorMethodOffset + 7;
    if (scenario == 2)
      fixture.image.memory.failed_address = ObjectVtable + kAircraftAccessorMethodOffset;
    if (scenario == 3)
      fixture.image.memory.integer(ObjectVtable + kAircraftAccessorMethodOffset, 0);
    if (scenario == 4)
      fixture.info.sections[4].flags = 0xe0000020;
    if (scenario == 5)
      fixture.objects.memory.integer(Facade + kAircraftAccessorObjectOffset, std::numeric_limits<std::uint64_t>::max() - 2);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva);
    require(!result.valid && !result.available && !result.error.empty() && result.read_failures == (scenario == 2 ? 1u : 0u),
            "Accessor slot/target bounds, read failure or pointer overflow were not refused");
  }
}

void selected_object_extension() {
  for (const auto flag : {0u, 1u, 2u, 255u}) {
    Fixture fixture;
    fixture.add_selected_object(static_cast<std::uint8_t>(flag), 63);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true);
    require(result.valid && result.available && result.selected_object_present && result.controller_validity == 0 &&
                result.selected_object_index == (flag == 1 ? 0 : 63) && result.selected_object_vtable_rva == SelectedVtable &&
                result.selected_object_method_slot_rva == SelectedVtable + kAircraftSelectedMethodOffset &&
                result.selected_object_method_rva == SelectedMethod && result.image_bytes == 56 &&
                result.object_bytes == (flag == 1 ? 458u : 466u) &&
                fixture.objects.memory.occurrences[AccessorObject + 672] == (flag == 1 ? 0u : 2u) &&
                fixture.image.memory.occurrences[kAircraftRendererGlobalRva] == 2,
            "Selected-object path did not follow exact renderer-byte/index behavior and metadata bounds");
    for (const auto address : {AccessorObject + 676, Renderer + 2800, AccessorObject + 296, OwnerControl, OwnerControl + 28, Owner + 752,
                               SelectedControl, SelectedControl + 28, SelectedObject})
      require(fixture.objects.memory.occurrences[address] == 2, "Selected-object chain field was not rechecked");
  }
  for (const auto expected : {0u, kAircraftExpectedFacadeVtableRva}) {
    Fixture fixture;
    fixture.add_selected_object();
    const auto result = fixture.run(Base, expected);
    require(result.valid && result.available && !result.selected_object_present && result.selected_object_method_rva == 0 &&
                fixture.objects.memory.occurrences[AccessorObject + 676] == 0 &&
                fixture.image.memory.occurrences[kAircraftRendererGlobalRva] == 0,
            "Disabled selected-object stage gained additional accesses");
  }
  {
    Fixture fixture;
    const auto result = fixture.run(Base, 0, true);
    require(!result.valid && result.stage == "accessor_profile" && result.image_bytes == 0 && result.object_bytes == 0,
            "Selected-object stage omitted its required fixed facade profile");
  }
  for (const bool wrong_method : {false, true}) {
    Fixture fixture;
    if (wrong_method) {
      fixture.add_selected_object();
      fixture.image.memory.integer(kAircraftExpectedControllerVtableRva + kAircraftAccessorMethodOffset,
                                   Base + kAircraftExpectedControllerMethodRva + 1);
    } else {
      fixture.add_accessor_object();
    }
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true);
    require(!result.valid && result.stage == "controller_identity" && fixture.objects.memory.occurrences[AccessorObject + 676] == 0 &&
                fixture.image.memory.occurrences[kAircraftRendererGlobalRva] == 0,
            "Unverified controller identity permitted extra state reads");
  }
  {
    Fixture fixture;
    fixture.add_selected_object();
    fixture.objects.memory.integer(AccessorObject + 676, 0xffffffff, 4);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true);
    require(result.valid && !result.available && result.stage == "controller_unavailable" && result.controller_validity == -1 &&
                fixture.image.memory.occurrences[kAircraftRendererGlobalRva] == 0,
            "Negative controller validity reached renderer state");
  }
  for (const auto index : {std::numeric_limits<std::int32_t>::min(), -1, 64, std::numeric_limits<std::int32_t>::max()}) {
    Fixture fixture;
    fixture.add_selected_object(0, index);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true);
    require(!result.valid && !result.available && result.stage == "controller_selected_index" &&
                result.error.find("inspection cap") != std::string::npos && fixture.objects.memory.occurrences[AccessorObject + 296] == 0,
            "Selected index exceeded inspection cap or was treated as an engine array count");
  }
  for (unsigned scenario = 0; scenario < 6; ++scenario) {
    Fixture fixture;
    fixture.add_selected_object();
    if (scenario == 0)
      fixture.image.memory.integer(kAircraftRendererGlobalRva, 0);
    if (scenario == 1)
      fixture.objects.memory.integer(AccessorObject + 296, 0);
    if (scenario == 2)
      fixture.objects.memory.integer(OwnerControl + 28, 8, 4);
    if (scenario == 3)
      fixture.objects.memory.integer(Owner + 752, 0);
    if (scenario == 4)
      fixture.objects.memory.integer(SelectedArray, 0);
    if (scenario == 5)
      fixture.objects.memory.integer(SelectedControl + 28, 8, 4);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true);
    require(result.valid && !result.available && !result.selected_object_present && result.selected_object_method_rva == 0 &&
                result.read_failures == 0,
            "Unavailable or stale selected-object path resolved a method");
  }
  for (const auto address : {AccessorObject + 676, Renderer + 2800, AccessorObject + 672, AccessorObject + 296, OwnerControl,
                             OwnerControl + 28, Owner + 752, SelectedArray + 32, SelectedControl, SelectedControl + 28, SelectedObject}) {
    for (const bool changes : {false, true}) {
      Fixture fixture;
      fixture.add_selected_object(0, 2);
      if (changes)
        fixture.objects.memory.change_on_repeat = address;
      else
        fixture.objects.memory.failed_address = address;
      const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true);
      require(!result.valid && !result.available && !result.error.empty() && result.read_failures == (changes ? 0u : 1u) &&
                  (!changes || result.stage == "consistency_recheck"),
              "Unreadable or changed selected-object chain survived validation");
    }
  }
  for (const bool fail : {false, true}) {
    Fixture fixture;
    fixture.add_selected_object();
    if (fail)
      fixture.image.memory.fail_on_repeat = kAircraftRendererGlobalRva;
    else
      fixture.image.memory.change_on_repeat = kAircraftRendererGlobalRva;
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true);
    require(!result.valid && !result.available && result.stage == "renderer_cached_global_recheck" && result.image_bytes == 56 &&
                result.read_failures == (fail ? 1u : 0u),
            "Cached renderer change/read failure survived its final recheck");
  }
  for (unsigned scenario = 0; scenario < 7; ++scenario) {
    Fixture fixture;
    fixture.add_selected_object();
    if (scenario == 0)
      fixture.info.sections[7].flags = 0x40000040;
    if (scenario == 1)
      fixture.info.sections[5].flags = 0xc0000040;
    if (scenario == 2)
      fixture.info.sections[5].size = kAircraftSelectedMethodOffset + 7;
    if (scenario == 3)
      fixture.info.sections[6].flags = 0x40000040;
    if (scenario == 4)
      fixture.image.memory.failed_address = SelectedVtable + kAircraftSelectedMethodOffset;
    if (scenario == 5)
      fixture.image.memory.integer(SelectedVtable + kAircraftSelectedMethodOffset, Base - 1);
    if (scenario == 6)
      fixture.objects.memory.integer(Owner + 752, std::numeric_limits<std::uint64_t>::max() - 2);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true);
    require(!result.valid && !result.available && !result.error.empty() && result.read_failures == (scenario == 4 ? 1u : 0u),
            "Selected-object image sections, method target or fixed array bounds were not enforced");
  }
  Fixture largest;
  largest.add_selected_object(0, 63);
  largest.objects.memory.integer(Container + 20, 64, 4);
  for (unsigned i = 0; i < 64; ++i)
    largest.add_world(i, i == 63 ? 77 : -1, 10);
  const auto result = largest.run(Base, kAircraftExpectedFacadeVtableRva, true);
  require(result.valid && result.available && result.object_bytes == 5270 && result.image_bytes == 56 &&
              result.selected_object_index == 63 && largest.objects.memory.occurrences[SelectedArray + 62 * 16] == 0 &&
              largest.objects.memory.occurrences[SelectedArray + 64 * 16] == 0,
          "Largest selected-object fixture exceeded the budget or read other array entries");
  std::printf("Extended maximum fixture: %u object bytes and %u image bytes, including consistency rereads.\n", result.object_bytes,
              result.image_bytes);
}

void component_extension() {
  {
    Fixture fixture;
    fixture.add_components();
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true);
    require(result.valid && result.available && result.aircraft_present && result.component_present && result.component_count == 1 &&
                result.selected_component_index == 0 && result.aircraft_vtable_rva == AircraftVtable &&
                result.component_vtable_rva == ComponentVtable && result.object_bytes == 618 && result.image_bytes == 56,
            "Fixed aircraft/component metadata did not resolve with exact bounded reads");
  }
  for (unsigned profile = 0; profile < 3; ++profile) {
    Fixture fixture;
    fixture.add_components();
    const auto result = fixture.run(Base, profile == 0 ? 0 : kAircraftExpectedFacadeVtableRva, profile == 2);
    require(result.valid && result.available && !result.aircraft_present && !result.component_present && result.component_vtable_rva == 0 &&
                fixture.objects.memory.occurrences[SelectedObject + 368] == 0,
            "Disabled component stage gained aircraft or component accesses");
  }
  {
    Fixture fixture;
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, false, true);
    require(!result.valid && result.stage == "accessor_profile" && result.image_bytes == 0 && result.object_bytes == 0,
            "Component stage omitted its required selected-object profile");
  }
  for (const bool wrong_method : {false, true}) {
    Fixture fixture;
    if (wrong_method) {
      fixture.add_components();
      fixture.image.memory.integer(kAircraftExpectedSelectedVtableRva + kAircraftSelectedMethodOffset,
                                   Base + kAircraftExpectedSelectedMethodRva + 1);
    } else {
      fixture.add_selected_object();
    }
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true);
    require(!result.valid && result.stage == "component_source_identity" && fixture.objects.memory.occurrences[SelectedObject + 368] == 0,
            "Unverified selected-object identity permitted aircraft-handle access");
  }
  {
    Fixture fixture;
    fixture.add_components(4, 2);
    fixture.objects.memory.integer(ComponentArray, 0);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true);
    require(result.valid && result.available && result.selected_component_index == 2 && result.component_count == 4 &&
                fixture.objects.memory.occurrences[ComponentArray] == 2 &&
                fixture.objects.memory.occurrences[FirstComponent + 0x1000 + 32] == 2 &&
                fixture.objects.memory.occurrences[ComponentArray + 24] == 0 && fixture.objects.memory.occurrences[FirstComponent] == 0 &&
                fixture.objects.memory.occurrences[FirstComponent + 0x1000] == 0,
            "Component selection failed to skip null/type mismatch, retain prior choices or stop at first type5");
  }
  for (unsigned scenario = 0; scenario < 7; ++scenario) {
    Fixture fixture;
    fixture.add_components();
    if (scenario == 0)
      fixture.objects.memory.integer(SelectedObject + 368, 0);
    if (scenario == 1)
      fixture.objects.memory.integer(AircraftControl + 28, 8, 4);
    if (scenario == 2)
      fixture.objects.memory.integer(AircraftControl, 0);
    if (scenario == 3)
      fixture.objects.memory.integer(Aircraft + 19272, 0);
    if (scenario == 4)
      fixture.objects.memory.integer(ComponentCollection + 36, 0, 4);
    if (scenario == 5)
      fixture.objects.memory.integer(ComponentCollection + 40, 0);
    if (scenario == 6)
      fixture.objects.memory.integer(FirstComponent + 32, 6, 4);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true);
    require(result.valid && !result.available && !result.component_present && result.component_vtable_rva == 0 && result.read_failures == 0,
            "Unavailable or stale aircraft/component reference was treated as complete");
  }
  for (const auto raw_count : {0xffffffffu, 65u, 0x7fffffffu}) {
    Fixture fixture;
    fixture.add_components();
    fixture.objects.memory.integer(ComponentCollection + 36, raw_count, 4);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true);
    require(!result.valid && !result.available && result.stage == "component_count" &&
                fixture.objects.memory.occurrences[ComponentCollection + 40] == 0,
            "Invalid signed component count was used to access an array");
  }
  for (const auto address : {SelectedObject + 368, AircraftControl + 28, AircraftControl, Aircraft, Aircraft + 19272,
                             ComponentCollection + 36, ComponentCollection + 40, ComponentArray, FirstComponent + 32, ComponentArray + 8,
                             FirstComponent + 0x1000 + 32, FirstComponent + 0x1000}) {
    for (const bool changes : {false, true}) {
      Fixture fixture;
      fixture.add_components(2, 1);
      if (changes)
        fixture.objects.memory.change_on_repeat = address;
      else
        fixture.objects.memory.failed_address = address;
      const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true);
      require(!result.valid && !result.available && !result.error.empty() && result.read_failures == (changes ? 0u : 1u) &&
                  (!changes || result.stage == "consistency_recheck"),
              "Unreadable/changed aircraft or prior component choice survived consistency validation");
    }
  }
  for (unsigned scenario = 0; scenario < 6; ++scenario) {
    Fixture fixture;
    fixture.add_components();
    if (scenario == 0)
      fixture.info.sections[8].flags = 0xc0000040;
    if (scenario == 1)
      fixture.objects.memory.integer(Aircraft, Base - 1);
    if (scenario == 2)
      fixture.info.sections[9].flags = 0x60000040;
    if (scenario == 3)
      fixture.objects.memory.integer(FirstComponent, Base + 235963904);
    if (scenario == 4)
      fixture.objects.memory.integer(Aircraft + 19272, std::numeric_limits<std::uint64_t>::max() - 2);
    if (scenario == 5)
      fixture.objects.memory.integer(ComponentCollection + 40, std::numeric_limits<std::uint64_t>::max() - 2);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true);
    require(!result.valid && !result.available && !result.error.empty() && result.read_failures == 0,
            "Aircraft/component static vptr or collection address bounds were not enforced");
  }
  Fixture largest;
  largest.add_components(64, 63);
  largest.objects.memory.integer(Container + 20, 64, 4);
  for (unsigned i = 0; i < 64; ++i)
    largest.add_world(i, i == 63 ? 77 : -1, 10);
  const auto result = largest.run(Base, kAircraftExpectedFacadeVtableRva, true, true);
  require(result.valid && result.available && result.worlds_examined == 64 && result.selected_component_index == 63 &&
              result.object_bytes == 6934 && result.image_bytes == 56 && largest.objects.memory.occurrences[ComponentArray + 64 * 8] == 0,
          "Maximum world/component traversal exceeded its read/trace cap or touched a later component");
  std::printf("Component maximum fixture: %u object bytes and %u image bytes, including all prior component choices.\n",
              result.object_bytes, result.image_bytes);
}

void camera_key_extension() {
  {
    Fixture fixture;
    fixture.add_camera_keys(2);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true);
    require(result.valid && result.available && !result.camera_keys_inspected && result.camera_key_count == 0 &&
                result.first_tail_match_index == -1 && result.first_gear_match_index == -1 && result.object_bytes == 618 &&
                fixture.objects.memory.occurrences[FirstComponent + 108] == 0 &&
                fixture.objects.memory.occurrences[FirstComponent + 112] == 0,
            "Disabled camera-key extension gained new reads or changed defaults");
  }
  for (unsigned profile = 0; profile < 3; ++profile) {
    Fixture fixture;
    const auto result = fixture.run(Base, profile == 0 ? 0 : kAircraftExpectedFacadeVtableRva, profile == 2, false, true);
    require(!result.valid && !result.camera_keys_inspected && result.stage == "accessor_profile" && result.object_bytes == 0 &&
                result.image_bytes == 0,
            "Camera-key extension read fields without its required component profile");
  }
  {
    Fixture fixture;
    fixture.add_components();
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true, true);
    require(!result.valid && !result.camera_keys_inspected && result.stage == "camera_key_component_identity" &&
                fixture.objects.memory.occurrences[FirstComponent + 108] == 0,
            "Wrong component vtable allowed camera-key field reads");
  }
  for (const auto count : {0u, 1u, 64u}) {
    Fixture fixture;
    fixture.add_camera_keys(count);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true, true);
    require(result.valid && result.available && result.camera_keys_inspected && result.camera_key_count == count &&
                result.tail_matches == 0 && result.gear_matches == 0 && result.first_tail_match_index == -1 &&
                result.first_gear_match_index == -1 && result.image_bytes == 56 &&
                result.object_bytes == 626 + (count == 0 ? 0 : 16 + count * 48),
            "Empty, unmatched or maximum key collection did not complete within exact read bounds");
    require(fixture.objects.memory.occurrences[FirstComponent + 112] == (count == 0 ? 0u : 2u) &&
                fixture.objects.memory.occurrences[CameraKeyArray + count * 8] == 0,
            "Key enumeration touched an empty array or read beyond its count");
    if (count == 64)
      std::printf("Camera-key maximum small graph: %u object bytes and %u image bytes, including all key rechecks.\n", result.object_bytes,
                  result.image_bytes);
  }
  for (const bool reverse : {false, true}) {
    Fixture fixture;
    fixture.add_camera_keys(7);
    fixture.camera_key(1, reverse ? GearKey : TailKey);
    fixture.camera_key(3, reverse ? TailKey : GearKey);
    fixture.camera_key(4, TailKey);
    fixture.camera_key(5, TailKey);
    fixture.camera_key(6, GearKey);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true, true);
    require(result.valid && result.available && result.camera_keys_inspected && result.camera_key_count == 7 && result.tail_matches == 3 &&
                result.gear_matches == 2 && result.first_tail_match_index == (reverse ? 3 : 1) &&
                result.first_gear_match_index == (reverse ? 1 : 3),
            "Fixed config keys, duplicates or first-match ordering were not compared correctly");
    for (unsigned i = 0; i < 7; ++i)
      require(fixture.objects.memory.occurrences[CameraKeyArray + i * 8] == 2 &&
                  fixture.objects.memory.occurrences[FirstCameraRecord + i * 0x1000 + 72] == 2,
              "A key or record pointer was omitted from the final consistency recheck");
  }
  // A swapped canonical byte order and a one-byte near-match are not the fixed
  // candidates. No normalization or heuristic GUID matching is performed.
  {
    Fixture fixture;
    fixture.add_camera_keys(2);
    auto wrong_order = TailKey;
    std::reverse(wrong_order.begin(), wrong_order.begin() + 4);
    auto near_match = GearKey;
    near_match[15] ^= 1;
    fixture.camera_key(0, wrong_order);
    fixture.camera_key(1, near_match);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true, true);
    require(result.valid && result.camera_keys_inspected && result.tail_matches == 0 && result.gear_matches == 0,
            "Key comparison accepted a different byte order or a partial match");
  }
  for (const auto raw_count : {65u, 0x80000000u, 0xffffffffu}) {
    Fixture fixture;
    fixture.add_camera_keys(1);
    fixture.objects.memory.integer(FirstComponent + 108, raw_count, 4);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true, true);
    require(!result.valid && !result.available && !result.camera_keys_inspected && result.stage == "camera_key_count" &&
                fixture.objects.memory.occurrences[FirstComponent + 112] == 0,
            "An out-of-range unsigned key count permitted array access");
  }
  for (const bool null_array : {false, true}) {
    Fixture fixture;
    fixture.add_camera_keys(3);
    fixture.camera_key(0, TailKey);
    fixture.objects.memory.integer(null_array ? FirstComponent + 112 : CameraKeyArray + 8, 0);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true, true);
    require(!result.valid && !result.available && !result.camera_keys_inspected && !result.error.empty() &&
                result.stage == (null_array ? "camera_key_array" : "camera_key_record") && result.read_failures == 0 &&
                fixture.objects.memory.occurrences[CameraKeyArray + 16] == 0,
            "Null camera-key storage was skipped or reported as a complete inventory");
  }
  for (const auto address : {FirstComponent, FirstComponent + 108, FirstComponent + 112, CameraKeyArray, FirstCameraRecord + 72,
                             CameraKeyArray + 8, FirstCameraRecord + 0x1000 + 72}) {
    for (const bool changes : {false, true}) {
      Fixture fixture;
      fixture.add_camera_keys(2);
      fixture.camera_key(0, TailKey);
      fixture.camera_key(1, GearKey);
      if (changes)
        fixture.objects.memory.change_on_repeat = address;
      else
        fixture.objects.memory.failed_address = address;
      const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true, true);
      require(!result.valid && !result.available && !result.camera_keys_inspected && !result.error.empty() &&
                  result.read_failures == (changes ? 0u : 1u) && (!changes || result.stage == "consistency_recheck"),
              "Unreadable or changed component vptr, key count, record pointer or key was reported as complete");
    }
  }
  for (const bool record_overflow : {false, true}) {
    Fixture fixture;
    fixture.add_camera_keys(1);
    fixture.objects.memory.integer(record_overflow ? CameraKeyArray : FirstComponent + 112, std::numeric_limits<std::uint64_t>::max() - 2);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true, true);
    require(!result.valid && !result.camera_keys_inspected && result.read_failures == 0 && !result.error.empty(),
            "Overflowing key array or record offset reached the object reader");
  }
  {
    Fixture fixture;
    fixture.add_camera_keys(64, 64, 63);
    fixture.objects.memory.integer(Container + 20, 64, 4);
    for (unsigned i = 0; i < 64; ++i)
      fixture.add_world(i, i == 63 ? 77 : -1, 10);
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true, true);
    require(!result.valid && !result.available && !result.camera_keys_inspected && result.object_bytes <= 8192 &&
                result.object_bytes > 8176 && result.error.find("budget") != std::string::npos,
            "Combined maximum world/component/key graph exceeded the hard attempted-read cap or claimed completeness");
    std::printf("Combined maximum graph refused at %u attempted object bytes.\n", result.object_bytes);
  }
}

void borrowed_aircraft_output() {
  for (const bool keys : {false, true}) {
    Fixture baseline;
    Fixture fixture;
    if (keys) {
      baseline.add_camera_keys(2);
      fixture.add_camera_keys(2);
    } else {
      baseline.add_components();
      fixture.add_components();
    }
    const auto before = baseline.run(Base, kAircraftExpectedFacadeVtableRva, true, true, keys);
    std::uint64_t address = 0xdeadbeef;
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true, keys, &address);
    require(before.valid && result.valid && result.available && address == Aircraft && result.object_bytes == before.object_bytes &&
                result.image_bytes == before.image_bytes && fixture.objects.memory.reads == baseline.objects.memory.reads &&
                fixture.image.memory.reads == baseline.image.memory.reads,
            "Borrowed aircraft output added reads or did not publish the already verified aircraft");
  }
  for (unsigned early_stage = 0; early_stage < 3; ++early_stage) {
    Fixture fixture;
    std::uint64_t address = 0xdeadbeef;
    const auto result =
        fixture.run(Base, early_stage == 0 ? 0 : kAircraftExpectedFacadeVtableRva, early_stage == 2, false, false, &address);
    require(!result.valid && address == 0 && result.stage == "accessor_profile" && result.image_bytes == 0 && result.object_bytes == 0,
            "An earlier metadata stage exposed an aircraft address or read beyond the required profile");
  }
  for (unsigned scenario = 0; scenario < 10; ++scenario) {
    Fixture fixture;
    fixture.add_components();
    if (scenario == 0)
      fixture.info.section_count = 97;
    if (scenario == 1)
      fixture.objects.memory.integer(SelectedObject + 368, 0);
    if (scenario == 2)
      fixture.objects.memory.integer(AircraftControl + 28, 8, 4);
    if (scenario == 3)
      fixture.objects.memory.integer(Aircraft + 19272, 0);
    if (scenario == 4)
      fixture.objects.memory.integer(FirstComponent + 32, 6, 4);
    if (scenario == 5)
      fixture.objects.memory.failed_address = Aircraft + 19272;
    if (scenario == 6)
      fixture.objects.memory.change_on_repeat = Aircraft;
    if (scenario == 7)
      fixture.image.memory.change_on_repeat = kAircraftGlobalRva;
    if (scenario == 8)
      fixture.image.memory.change_on_repeat = kAircraftRendererGlobalRva;
    if (scenario == 9)
      fixture.objects.memory.fail_on_repeat = FirstComponent;
    std::uint64_t address = 0xdeadbeef;
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true, false, &address);
    require((!result.valid || !result.available) && address == 0,
            "Failed/unavailable or changed metadata published or retained a borrowed aircraft address");
  }
  {
    Fixture fixture;
    fixture.add_camera_keys(2);
    fixture.objects.memory.change_on_repeat = FirstCameraRecord + 72;
    std::uint64_t address = 0xdeadbeef;
    const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true, true, &address);
    require(!result.valid && address == 0, "A later camera-key consistency failure published the earlier aircraft address");
  }
}

void borrowed_user_output() {
  Fixture baseline;
  baseline.add_components();
  const auto before = baseline.run(Base, kAircraftExpectedFacadeVtableRva, true, true);
  Fixture fixture;
  fixture.add_components();
  std::uint64_t source = 0, user = 0;
  const auto result = fixture.run(Base, kAircraftExpectedFacadeVtableRva, true, true, false, &source, &user);
  require(result.valid && result.available && source == Aircraft && user == User, "full graph publishes active user and source");
  require(result.object_bytes == before.object_bytes && result.image_bytes == before.image_bytes, "user output adds no target reads");
  for (unsigned scenario = 0; scenario < 5; ++scenario) {
    Fixture failed;
    failed.add_components();
    if (scenario == 0)
      failed.objects.memory.change_on_repeat = UserArray;
    if (scenario == 1)
      failed.objects.memory.change_on_repeat = UserControl;
    if (scenario == 2)
      failed.objects.memory.fail_on_repeat = FirstComponent;
    if (scenario == 3)
      failed.objects.memory.integer(ComponentCollection + 36, 0, 4);
    std::uint64_t output = 0xdeadbeef;
    const auto state = failed.run(Base, kAircraftExpectedFacadeVtableRva, true, scenario != 4, false, nullptr, &output);
    require((!state.valid || !state.available) && output == 0, "unavailable/changed/incomplete graph clears active user output");
  }
}
}  // namespace

int main() {
  valid_paths();
  build_refusal();
  absence_and_selection();
  bounds_and_read_failures();
  static_method_validation();
  consistency_and_budget();
  accessor_extension();
  selected_object_extension();
  component_extension();
  camera_key_extension();
  borrowed_aircraft_output();
  borrowed_user_output();
  std::printf("PASS: %u bounded aircraft-facade metadata checks. Synthetic readers only.\n", checks);
  return 0;
}
