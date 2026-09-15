#include "../../src/camera/manager_inspection.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
using namespace taxi_camera;
using namespace native_camera;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}
constexpr std::uint64_t Base = 0x140000000, Owner = 0x200000, Renderer = 0x300000, Manager = 0x400000, Control = 0x500003;
struct Field {
  bool image;
  std::uint64_t address;
  std::size_t size;
  std::array<unsigned char, 16> bytes{};
};
struct Fixture : discovery::ImageReader, engine_camera::MemoryReader {
  std::vector<Field> fields;
  std::size_t next = 0, fail_at = SIZE_MAX, change_at = SIZE_MAX, change_byte = 0;
  template <typename Value>
  void add(bool image, std::uint64_t address, Value value) {
    Field field{image, address, sizeof(value)};
    std::memcpy(field.bytes.data(), &value, sizeof(value));
    fields.push_back(field);
  }
  explicit Fixture(std::uint32_t generation = 7) {
    struct Handle {
      std::uint64_t control;
      std::uint32_t generation, extra;
    };
    for (unsigned pass = 0; pass < 2; ++pass) {
      add(true, kManagerOwnerGlobal, Owner);
      add(true, kManagerRendererGlobal, Renderer);
      add(false, Owner + 2496, Manager);
      add(false, Owner + 2480, Handle{Control, generation, 23});
      add(false, Control + 28, generation);
      add(false, Control, Manager);
      add(false, Manager, Base + kManagerVtable);
    }
  }
  discovery::ReadWindow query(std::uint32_t, std::uint32_t) override {
    require(false, "The pure guard must only request its exact fixed fields");
    return {};
  }
  bool read(std::uint32_t address, void* output, std::size_t size) override { return capture(true, address, output, size); }
  bool read(std::uint64_t address, void* output, std::size_t size) override { return capture(false, address, output, size); }
  bool capture(bool image, std::uint64_t address, void* output, std::size_t size) {
    require(next < fields.size(), "Manager guard exceeded its complete trace");
    const auto index = next++;
    const auto& field = fields[index];
    require(image == field.image && address == field.address && size == field.size, "Manager guard followed an unapproved field");
    if (index == fail_at)
      return false;
    std::memcpy(output, field.bytes.data(), size);
    if (index == change_at)
      static_cast<unsigned char*>(output)[change_byte] ^= 1;
    return true;
  }
  ManagerInspection run(std::uint64_t retained = Control, std::uint64_t receiver = Manager) {
    return inspect_manager_identity(*this, *this, Base, receiver, retained);
  }
};
void refusals() {
  for (const auto first_field : {2u, 4u, 5u, 6u}) {
    Fixture fixture;
    fixture.change_at = first_field;
    const auto result = fixture.run();
    require(!result && result.status == ManagerInspectionStatus::identity_refused && !result.temporary(),
            "Actual manager, generation, payload or vtable disagreement became retryable");
    require(fixture.next == first_field + 1, "Guard followed a chain after its first identity disagreement");
    require(result.manager == 0 && result.control == 0, "Refused identity exposed usable pointers");
  }
  Fixture retained;
  require(retained.run(Control + 1).status == ManagerInspectionStatus::identity_refused && retained.next == 14,
          "A changed retained control bypassed complete trace and ownership checks");
  Fixture null_control;
  std::fill_n(null_control.fields[3].bytes.begin(), 8, 0);
  require(null_control.run().status == ManagerInspectionStatus::identity_refused && null_control.next == 4,
          "Null weak control was followed or treated as temporary");
  Fixture receiver;
  require(receiver.run(Control, Manager + 8).status == ManagerInspectionStatus::identity_refused && receiver.next == 3,
          "An unrelated update receiver passed the current manager guard");
  Fixture owner_overflow;
  const auto address = UINT64_MAX - 16;
  std::memcpy(owner_overflow.fields[0].bytes.data(), &address, 8);
  require(owner_overflow.run().status == ManagerInspectionStatus::identity_refused && owner_overflow.next == 2,
          "Overflowing owner fields were followed");
}
}  // namespace

int main() {
  Fixture valid;
  const auto result = valid.run();
  require(result && valid.next == 14 && result.manager == Manager && result.renderer == Renderer && result.control == Control &&
              result.generation == 8,
          "Complete manager identity was not preserved");
  Fixture first_creation;
  require(bool(first_creation.run(0)), "Unowned first creation was incorrectly refused");
  Fixture largest_generation(UINT32_MAX);
  require(largest_generation.run().generation == std::uint64_t(UINT32_MAX) + 1, "Manager generation token wrapped");
  for (std::size_t failed = 0; failed < 14; ++failed) {
    Fixture fixture;
    fixture.fail_at = failed;
    const auto unavailable = fixture.run();
    require(!unavailable && unavailable.temporary() && fixture.next == failed + 1,
            "An unreadable field became fatal or caused further chain traversal");
    require(unavailable.manager == 0 && unavailable.control == 0 && std::strstr(unavailable.detail, "read"),
            "Unavailable inspection exposed a pointer or omitted the failing read detail");
  }
  for (std::size_t changed = 7; changed < 14; ++changed) {
    Fixture fixture;
    fixture.change_at = changed;
    const auto unavailable = fixture.run();
    require(!unavailable && unavailable.temporary() && fixture.next == changed + 1 && std::strstr(unavailable.detail, "changed"),
            "A changing full reread was consumed or misclassified");
  }
  Fixture extra_changed;
  extra_changed.change_at = 10;
  extra_changed.change_byte = 15;
  require(extra_changed.run().temporary(), "The complete weak-handle reread omitted its extra bytes");
  for (const auto absent : {0u, 1u}) {
    Fixture fixture;
    fixture.fields[absent].bytes.fill(0);
    require(fixture.run().temporary() && fixture.next == absent + 1, "Absent service was followed or permanently latched");
  }
  refusals();
  std::printf("Manager inspection: PASS %u checks\n", checks);
}
