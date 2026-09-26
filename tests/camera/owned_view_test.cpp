#include "../../src/camera/owned_view.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace taxi_camera::engine_camera;
constexpr std::uint64_t kEntry = 0x100000;
constexpr std::uint64_t kArray = 0x200000;
constexpr std::uint64_t kView = 0x300000;
constexpr std::uint64_t kNode = 0x400000;
constexpr std::uint64_t kCamera = 0x500000;
constexpr std::uint64_t kMaterial = 0x600000;
constexpr std::uint64_t kBitmap = 0x700000;
constexpr std::uint64_t kRecord = 0x800000;
constexpr std::uint64_t kWrapper = 0x900000;
constexpr std::uint64_t kResource = 0xa00000;
constexpr std::uint64_t kRtRecord = 0xe00000;
constexpr std::uint64_t kRtTable = 0xf00000;
constexpr std::uint64_t kRtEntries = 0x1000000;
constexpr std::uint64_t kControl = 0xb00000;
constexpr std::uint64_t kId = 0x1122334455667788;
std::uint32_t checks = 0;

void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

struct Reader final : MemoryReader {
  std::map<std::uint64_t, std::uint8_t> bytes;
  std::vector<std::pair<std::uint64_t, std::size_t>> reads;
  std::vector<std::pair<std::uint64_t, std::size_t>> permitted;
  // Engine fields the trace may observe. Object spans also read the bytes
  // between them, which default to zero and are never compared.
  std::vector<std::pair<std::uint64_t, std::size_t>> fields;
  std::size_t fail_call = 0;
  std::size_t changed_call = 0;
  std::size_t changed_byte = 0;

  bool read(std::uint64_t address, void* destination, std::size_t size) override {
    reads.emplace_back(address, size);
    if (reads.size() == fail_call)
      return false;
    if (reads.size() == changed_call)
      bytes[address + changed_byte] ^= 1;
    auto* output = static_cast<std::uint8_t*>(destination);
    for (std::size_t index = 0; index < size; ++index) {
      const auto found = bytes.find(address + index);
      if (found == bytes.end())
        return false;
      output[index] = found->second;
    }
    return true;
  }

  void word(std::uint64_t address, std::uint64_t value, std::uint32_t size = 8) {
    for (std::uint32_t index = 0; index < size; ++index)
      bytes[address + index] = static_cast<std::uint8_t>(value >> (index * 8));
    permitted.emplace_back(address, size);
    fields.emplace_back(address, size);
  }

  // One object's span. Bytes no field has set read as zero.
  void region(std::uint64_t address, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index)
      bytes.try_emplace(address + index, 0);
    permitted.emplace_back(address, size);
  }

  bool observed(std::uint64_t address) const {
    return std::any_of(fields.begin(), fields.end(),
                       [address](const auto& field) { return address >= field.first && address - field.first < field.second; });
  }

  void handle(std::uint64_t address, unsigned control_index, std::uint64_t payload) {
    const auto control = kControl + control_index * 0x100;
    word(address, control);
    word(address + 8, 29, 4);
    word(address + 12, 41, 4);
    permitted.emplace_back(address, 16);
    fields.emplace_back(address, 16);
    word(control + 28, 29, 4);
    word(control, payload);
    region(control, 32);
  }
};

struct Fixture {
  Reader reader;
  ViewPoolSnapshot pool;
  std::uint64_t entry = kEntry;
  std::uint64_t id = kId;

  Fixture() {
    pool.valid = true;
    pool.status = ViewPoolStatus::complete;
    pool.slots_examined = 8;
    pool.array_address = kArray;
    for (std::uint32_t index = 0; index < 8; ++index) {
      pool.slots[index].index = index;
      pool.slots[index].view_address = kView + index * 0x1000;
      pool.slots[index].association = ViewAssociation::occupied;
      pool.slots[index].association_valid = true;
      reader.word(kArray + index * 8, pool.slots[index].view_address);
      const auto view = pool.slots[index].view_address;
      for (std::uint32_t pair = 0; pair < 3; ++pair) {
        reader.word(view + 16 + pair * 8, 768 + pair * 100, 4);
        reader.word(view + 20 + pair * 8, 763 + pair * 100, 4);
        reader.permitted.emplace_back(view + 16 + pair * 8, 8);
      }
      reader.word(view + 48, 0x1234567890abcdefull);
      reader.word(view + 56, 0xfedcba0987654321ull);
      reader.permitted.emplace_back(view + 48, 16);
      reader.region(view + 16, 104);  // Close trace: dimensions through the Node handle.
      reader.region(view + 16, 144);  // Full trace: through the material handle.
    }
    reader.word(kEntry, kId);
    reader.word(kEntry + 16, kId);
    reader.word(kEntry + 24, 2, 4);
    reader.word(kEntry + 8, 1, 1);
    reader.word(kEntry + 76, 0, 4);
    reader.handle(kEntry + 96, 0, kNode);
    reader.handle(kView + 104, 1, kNode);
    reader.word(kNode + 256, kCamera);
    reader.word(kCamera + 160, 7, 2);
    reader.word(kCamera + 1616, std::bit_cast<std::uint32_t>(1.25f), 4);
    reader.handle(kEntry + 80, 2, kMaterial);
    reader.handle(kView + 144, 3, kMaterial);
    reader.handle(kMaterial + 520, 4, kBitmap);
    reader.word(kBitmap + 40, 736, 4);
    reader.word(kBitmap + 44, 251, 4);
    reader.permitted.emplace_back(kBitmap + 40, 8);
    reader.word(kBitmap + 88, kRecord);
    reader.word(kRecord + 16, kWrapper);
    reader.word(kWrapper + 168, kResource);
    // Render-target record resolution as the captured replay performs it: no
    // per-subresource table, so the direct record at T+0x40 is used.
    reader.word(kRecord + 0x48, 0);
    reader.word(kRecord + 0x40, kRtRecord);
    // Add-diffuse (664) and depth-stencil (712) slots are absent by default.
    null_handle(kMaterial + 664);
    null_handle(kMaterial + 712);
    reader.region(kEntry, 24);       // Key, ready byte and payload ID.
    reader.region(kEntry + 24, 88);  // Mode through the Node handle.
    reader.region(kMaterial + 664, 64);
    reader.region(kRecord + 16, 64);  // Wrapper through the record table.
  }
  void null_handle(std::uint64_t address) {
    reader.word(address, 0);
    reader.word(address + 8, 0);
    reader.permitted.emplace_back(address, 16);
    reader.fields.emplace_back(address, 16);
  }
  // Route the diffuse record through a per-subresource table instead.
  void table_record(std::uint64_t entry0) {
    reader.word(kRecord + 0x48, kRtTable);
    reader.word(kRtTable + 8, kRtEntries);
    reader.word(kRtEntries, entry0);
  }

  OwnedViewCloseSnapshot run_close() {
    reader.reads.clear();
    const auto result = inspect_owned_view_for_close(reader, entry, id, pool);
    std::uint32_t total = 0;
    for (const auto& read : reader.reads) {
      require(std::find(reader.permitted.begin(), reader.permitted.end(), read) != reader.permitted.end(),
              "Close reader followed an unlisted field");
      require(read.first <= std::numeric_limits<std::uint64_t>::max() - read.second, "Close reader received an overflowing range");
      total += static_cast<std::uint32_t>(read.second);
    }
    require(total == result.read_bytes && total <= 8192 && result.read_failures <= 1, "Close reader budget/failure accounting changed");
    if (result.complete) {
      require(result.status == OwnedViewStatus::ready && result.read_failures == 0 && !*result.error && reader.reads.size() % 2 == 0,
              "Close result completed without an error-free trace");
      const auto half = reader.reads.size() / 2;
      require(std::equal(reader.reads.begin(), reader.reads.begin() + half, reader.reads.begin() + half),
              "Close result omitted a full exact trace reread");
    } else {
      require(result.view_address == 0 && result.view_index == -1 && result.flags == std::array<std::uint64_t, 2>{} &&
                  result.dimensions == std::array<std::array<std::int32_t, 2>, 3>{},
              "Refused close result published native authority");
    }
    return result;
  }
  bool diagnostic_slots = true;
  OwnedViewSnapshot run() {
    reader.reads.clear();
    const auto result = inspect_owned_view(reader, entry, id, pool, diagnostic_slots);
    std::uint32_t total = 0;
    for (const auto& read : reader.reads) {
      require(std::find(reader.permitted.begin(), reader.permitted.end(), read) != reader.permitted.end(),
              "Reader accessed an unlisted field or followed the final resource pointer");
      require(read.first <= std::numeric_limits<std::uint64_t>::max() - read.second, "Reader received an overflowing range");
      total += static_cast<std::uint32_t>(read.second);
    }
    require(total == result.read_bytes && total <= 8192 && result.read_failures <= 1, "Attempted-byte/failure allowance is incorrect");
    if (result.complete) {
      require(reader.reads.size() % 2 == 0 && result.read_failures == 0 && result.error[0] == 0,
              "Completed inspection retained an error or incomplete recheck");
      const auto half = reader.reads.size() / 2;
      require(std::equal(reader.reads.begin(), reader.reads.begin() + half, reader.reads.begin() + half),
              "A successful snapshot omitted or reordered its consistency trace");
      require(result.ready == (result.status == OwnedViewStatus::ready), "Ready status disagrees with ready flag");
    }
    if (!result.resource_present)
      require(result.output_dimensions == std::array<std::int32_t, 2>{}, "Unavailable output published bitmap dimensions");
    if (!result.ready) {
      require(result.view_address == 0 && result.node_address == 0 && result.camera_address == 0 && result.resource_address == 0 &&
                  result.fov == 0 && !result.resource_present && result.view_index == -1 && result.mode == 0 &&
                  result.dimensions == std::array<std::array<std::int32_t, 2>, 3>{} && result.flags == std::array<std::uint64_t, 2>{},
              "Unusable or pending result published borrowed addresses or output metadata");
    }
    return result;
  }
};

void successful_and_pending() {
  Fixture full;
  const auto complete = full.run();
  require(complete.complete && complete.ready && complete.resource_present && complete.resource_address == kResource &&
              complete.view_address == kView && complete.node_address == kNode && complete.camera_address == kCamera &&
              complete.view_index == 0 && complete.mode == 2 && complete.fov == 1.25f,
          "Full owned-view chain was not observed exactly");
  require(complete.read_bytes == 1212 && full.reader.reads.size() == 36, "Full trace read extent changed unexpectedly");
  require(complete.dimensions == std::array<std::array<std::int32_t, 2>, 3>{{{768, 763}, {868, 863}, {968, 963}}} &&
              complete.flags == std::array<std::uint64_t, 2>{0x1234567890abcdefull, 0xfedcba0987654321ull},
          "Dimension pair order, signed scalar decoding or exact64-bit flag values changed");
  require(complete.output_dimensions == std::array<std::int32_t, 2>{736, 251},
          "Bitmap dimensions were confused with inherited view dimensions");
  require(std::count(full.reader.reads.begin(), full.reader.reads.end(), std::pair<std::uint64_t, std::size_t>{kBitmap + 40, 8}) == 2,
          "Bitmap dimensions are not included in the complete trace reread");
  Fixture shared_controls;
  shared_controls.reader.handle(kView + 104, 0, kNode);
  shared_controls.reader.handle(kView + 144, 2, kMaterial);
  require(shared_controls.run().ready, "Entry/view handles sharing the same control record were rejected");
  for (std::uint32_t index = 0; index < 8; ++index) {
    Fixture test;
    const auto selected = test.pool.slots[index].view_address;
    test.reader.word(kEntry + 76, index, 4);
    test.reader.handle(selected + 104, 1, kNode);
    test.reader.handle(selected + 144, 3, kMaterial);
    const auto result = test.run();
    require(result.ready && result.view_index == static_cast<std::int32_t>(index) && result.view_address == selected,
            "A valid view-pool index selected the wrong view");
  }
  Fixture pending;
  pending.reader.word(kEntry + 8, 0, 1);
  const auto result = pending.run();
  require(result.complete && !result.ready && result.status == OwnedViewStatus::pending && result.read_bytes == 48 &&
              pending.reader.reads.size() == 2 &&
              std::all_of(pending.reader.reads.begin(), pending.reader.reads.end(),
                          [](const auto& read) { return read == std::pair<std::uint64_t, std::size_t>{kEntry, 24}; }),
          "Pending entry followed uninitialized setup fields");
}

void absent_output() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    Fixture test;
    for (const auto address : {kEntry + 80, kView + 144}) {
      if (mode == 0)
        test.reader.word(address, 0);
    }
    for (const auto control : {kControl + 0x200, kControl + 0x300}) {
      if (mode == 1)
        test.reader.word(control + 28, 30, 4);
      if (mode == 2)
        test.reader.word(control, 0);
    }
    const auto result = test.run();
    require(result.ready && !result.resource_present, "Absent/stale material references prevented camera readiness");
    require(std::none_of(test.reader.reads.begin(), test.reader.reads.end(),
                         [](const auto& read) {
                           return read.first == kMaterial + 520 || read.first == kBitmap + 88 || read.first == kRecord + 16 ||
                                  read.first == kWrapper + 168;
                         }),
            "Unavailable material was followed into downstream output objects");
  }
  for (const auto address : {kMaterial + 520, kControl + 0x400, kBitmap + 88, kRecord + 16, kWrapper + 168}) {
    Fixture test;
    test.reader.word(address, 0);
    const auto result = test.run();
    require(result.ready && !result.resource_present, "A missing optional output link prevented camera readiness");
  }
  Fixture stale;
  stale.reader.word(kControl + 0x400 + 28, 30, 4);
  require(stale.run().ready, "A stale optional bitmap reference prevented camera readiness");
  require(std::none_of(stale.reader.reads.begin(), stale.reader.reads.end(), [](const auto& read) { return read.first == kBitmap + 88; }),
          "Stale bitmap generation permitted following its payload");
}

void render_target_records() {
  // Direct record at T+0x40: the diffuse slot reports Bitmap, texture and record.
  Fixture direct;
  auto result = direct.run();
  require(result.ready && result.resource_present && result.output_slots[0].bitmap && result.output_slots[0].texture &&
              result.output_slots[0].render_target_record && result.output_slots[0].mask() == 7,
          "Direct render-target record was not observed");
  require(result.output_slots[1].mask() == 0 && result.output_slots[2].mask() == 0, "Absent slots reported an output");
  // Per-subresource table: [T+0x48]->[+8]->[0] decides, the direct field is not read.
  Fixture table;
  table.table_record(kRtRecord);
  table.reader.word(kRecord + 0x40, 0);
  result = table.run();
  require(result.ready && result.output_slots[0].render_target_record, "Table-resolved render-target record was not observed");
  require(std::none_of(table.reader.reads.begin(), table.reader.reads.end(), [](const auto& read) { return read.first == kRecord + 0x40; }),
          "The direct record decided presence although a subresource table exists");
  Fixture empty_table;
  empty_table.table_record(0);
  result = empty_table.run();
  require(result.ready && result.resource_present && !result.output_slots[0].render_target_record,
          "A null table entry was reported as a render-target record");
  Fixture no_entries;
  no_entries.reader.word(kRecord + 0x48, kRtTable);
  no_entries.reader.word(kRtTable + 8, 0);
  result = no_entries.run();
  require(result.ready && !result.output_slots[0].render_target_record, "A table without entries was reported as a record");
  // The retained fault state: resource present, pane-sized Bitmap, no record.
  Fixture recordless;
  recordless.reader.word(kRecord + 0x40, 0);
  result = recordless.run();
  require(result.ready && result.resource_present && result.output_dimensions == std::array<std::int32_t, 2>{736, 251} &&
              result.output_slots[0].texture && !result.output_slots[0].render_target_record && result.output_slots[0].mask() == 3,
          "A texture without a render-target record was not reported as such");
  // A malformed table pointer refuses the snapshot rather than guessing.
  Fixture malformed;
  malformed.reader.word(kRecord + 0x48, kRtTable + 4);
  result = malformed.run();
  require(!result.complete && result.status == OwnedViewStatus::invalid_pointer, "A misaligned record table was followed");
  // Other slots: Bitmap only, Bitmap plus texture, and a complete record.
  Fixture others;
  others.reader.handle(kMaterial + 664, 5, kBitmap + 0x1000);
  others.reader.word(kBitmap + 0x1000 + 88, 0);
  others.reader.handle(kMaterial + 712, 6, kBitmap + 0x2000);
  others.reader.word(kBitmap + 0x2000 + 88, kRecord + 0x1000);
  others.reader.word(kRecord + 0x1000 + 0x48, 0);
  others.reader.word(kRecord + 0x1000 + 0x40, kRtRecord + 0x1000);
  others.reader.region(kRecord + 0x1000 + 0x40, 16);
  result = others.run();
  require(result.ready && result.output_slots[1].mask() == 1 && result.output_slots[2].mask() == 7 && result.diagnostic_slots_observed,
          "Add-diffuse and depth-stencil slot observations were wrong");
  // Pulses may skip the diagnostic slots: the diffuse slot, output and every
  // readiness field are still traced and reread; slots 1/2 stay unobserved.
  const auto full_reads = others.reader.reads.size();
  others.diagnostic_slots = false;
  const auto skipped = others.run();
  require(skipped.ready && skipped.resource_present && skipped.resource_address == result.resource_address &&
              skipped.output_dimensions == result.output_dimensions && skipped.output_slots[0].mask() == 7 &&
              skipped.output_slots[1].mask() == 0 && skipped.output_slots[2].mask() == 0 && !skipped.diagnostic_slots_observed &&
              skipped.dimensions == result.dimensions && skipped.flags == result.flags && skipped.fov == result.fov,
          "Skipping the diagnostic slots changed a readiness or output observation");
  require(others.reader.reads.size() + 12 == full_reads &&
              std::none_of(others.reader.reads.begin(), others.reader.reads.end(),
                           [](const auto& read) { return read.first >= kMaterial + 664 && read.first < kMaterial + 728; }),
          "Skipped diagnostic slots were still read");
  // Nothing beyond the record pointer itself is dereferenced.
  for (const auto& read : direct.reader.reads)
    require(read.first < kRtRecord || read.first >= kRtRecord + 0x1000, "A render-target record was dereferenced");
}

void numeric_diagnostics() {
  for (const auto bits : {0u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
    for (unsigned pair = 0; pair < 3; ++pair) {
      Fixture test;
      test.reader.word(kView + 16 + pair * 8, bits, 4);
      test.reader.word(kView + 20 + pair * 8, bits, 4);
      const auto result = test.run();
      require(result.ready && result.dimensions[pair][0] == std::bit_cast<std::int32_t>(bits) &&
                  result.dimensions[pair][1] == std::bit_cast<std::int32_t>(bits),
              "Raw diagnostic dimensions were clamped, rejected or converted with the wrong signedness");
    }
  }
  for (const auto flags : {0ull, 1ull, 0x8000000000000000ull, 0xffffffffffffffffull}) {
    Fixture test;
    test.reader.word(kView + 48, flags);
    test.reader.word(kView + 56, flags);
    const auto result = test.run();
    require(result.ready && result.flags[0] == flags && result.flags[1] == flags,
            "Diagnostic flag words lost bits or imposed an unverified flag meaning");
  }
}

void mode_observations() {
  for (const auto mode : {0u, 1u, 2u, 3u, 0x80000000u, 0xffffffffu}) {
    Fixture test;
    test.reader.word(kEntry + 24, mode, 4);
    const auto result = test.run();
    require(result.ready && result.mode == mode, "Ready-entry mode was guessed, clamped or rejected instead of observed exactly");
    require(std::count(test.reader.reads.begin(), test.reader.reads.end(), std::pair<std::uint64_t, std::size_t>{kEntry + 24, 88}) == 2,
            "Mode was omitted from the complete field reread");
  }
  for (unsigned byte = 0; byte < 4; ++byte) {
    Fixture test;
    test.reader.bytes.erase(kEntry + 24 + byte);
    const auto result = test.run();
    require(!result.ready && !result.complete && result.mode == 0 && result.status == OwnedViewStatus::read_failed,
            "Unreadable mode published a usable snapshot");
  }
  Fixture pending;
  pending.reader.word(kEntry + 8, 0, 1);
  pending.reader.bytes.erase(kEntry + 24);
  const auto result = pending.run();
  require(result.complete && result.status == OwnedViewStatus::pending && result.mode == 0,
          "Pending setup followed an unavailable mode field");
}
void output_dimension_observations() {
  for (const auto width : {0u, 1u, 774u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
    for (const auto height : {0u, 496u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
      Fixture test;
      test.reader.word(kBitmap + 40, width, 4);
      test.reader.word(kBitmap + 44, height, 4);
      const auto result = test.run();
      require(result.ready && result.resource_present && result.output_dimensions[0] == std::bit_cast<std::int32_t>(width) &&
                  result.output_dimensions[1] == std::bit_cast<std::int32_t>(height),
              "Bitmap dimensions were clamped or treated as an unverified validity condition");
    }
  }
  for (unsigned byte = 0; byte < 8; ++byte) {
    Fixture test;
    test.reader.bytes.erase(kBitmap + 40 + byte);
    const auto result = test.run();
    require(!result.complete && result.status == OwnedViewStatus::read_failed && result.read_failures == 1,
            "Unreadable bitmap dimension bytes did not refuse the complete snapshot");
  }
  for (const auto missing : {kMaterial + 520, kControl + 0x400, kBitmap + 88, kRecord + 16, kWrapper + 168}) {
    Fixture test;
    test.reader.word(missing, 0);
    test.reader.bytes.erase(kBitmap + 40);
    const auto result = test.run();
    require(result.ready && !result.resource_present && result.output_dimensions == std::array<std::int32_t, 2>{},
            "Missing optional output demanded or published unavailable bitmap dimensions");
    require(std::none_of(test.reader.reads.begin(), test.reader.reads.end(), [](const auto& read) { return read.first == kBitmap + 40; }),
            "Bitmap dimensions were read without a present output resource member");
  }
}
void refusals() {
  for (const auto address : {kEntry, kEntry + 16}) {
    Fixture test;
    test.reader.word(address, kId + 1);
    require(test.run().status == OwnedViewStatus::id_mismatch, "Owned key/payload mismatch was accepted");
  }
  for (const auto ready : {2u, 127u, 255u}) {
    Fixture test;
    test.reader.word(kEntry + 8, ready, 1);
    require(test.run().status == OwnedViewStatus::invalid_ready_byte, "An unrecognized setup byte was accepted");
  }
  for (const auto index : {8u, 0xffffffffu, 0x80000000u, 0x7fffffffu}) {
    Fixture test;
    test.reader.word(kEntry + 76, index, 4);
    require(test.run().status == OwnedViewStatus::invalid_view_index, "An out-of-range/signed-negative pool index was followed");
  }
  for (unsigned mode = 0; mode < 3; ++mode) {
    for (unsigned reference = 0; reference < 2; ++reference) {
      Fixture test;
      const auto handle = reference == 0 ? kEntry + 96 : kView + 104;
      const auto control = kControl + reference * 0x100;
      test.reader.word(mode == 0 ? handle : mode == 1 ? control + 28 : control, mode == 1 ? 30 : 0, mode == 1 ? 4 : 8);
      require(test.run().status == OwnedViewStatus::node_unavailable, "An absent/stale required Node was usable");
    }
  }
  Fixture node_mismatch;
  node_mismatch.reader.word(kControl + 0x100, kNode + 0x1000);
  require(node_mismatch.run().status == OwnedViewStatus::node_mismatch, "Different entry/view Nodes were accepted");
  for (const auto material : {std::uint64_t{0}, kMaterial + 0x1000}) {
    Fixture test;
    test.reader.word(kControl + 0x300, material);
    require(test.run().status == OwnedViewStatus::material_mismatch, "Different entry/view materials were accepted");
  }
  for (const auto type : {0u, 6u, 8u, 0x107u, 0xffffu}) {
    Fixture test;
    test.reader.word(kCamera + 160, type, 2);
    require(test.run().status == OwnedViewStatus::wrong_camera_type, "Wrong Camera WORD type was accepted");
  }
  for (const auto bits : {0u, 0x80000000u, 0xbf800000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0x7f800001u}) {
    Fixture test;
    test.reader.word(kCamera + 1616, bits, 4);
    require(test.run().status == OwnedViewStatus::invalid_fov, "Nonpositive/nonfinite Camera FOV was accepted");
  }
  Fixture changed_pool;
  changed_pool.reader.word(kArray, kView + 8);
  require(changed_pool.run().status == OwnedViewStatus::pool_changed, "A stale snapshot view pointer was followed");
}

void pointer_and_pool_bounds() {
  constexpr auto last = std::numeric_limits<std::uint64_t>::max() - 7;
  for (const auto address : {std::uint64_t{0}, std::uint64_t{1}, last}) {
    Fixture test;
    test.entry = address;
    require(test.run().status == OwnedViewStatus::invalid_request && test.reader.reads.empty(), "Invalid entry pointer triggered access");
  }
  Fixture zero_id;
  zero_id.id = 0;
  require(zero_id.run().status == OwnedViewStatus::invalid_request && zero_id.reader.reads.empty(), "Zero owned ID triggered access");
  for (unsigned mutation = 0; mutation < 9; ++mutation) {
    Fixture test;
    switch (mutation) {
      case 0:
        test.pool.valid = false;
        break;
      case 1:
        test.pool.status = ViewPoolStatus::changed;
        break;
      case 2:
        test.pool.slots_examined = 7;
        break;
      case 3:
        test.pool.read_failures = 1;
        break;
      case 4:
        test.pool.array_address = 0;
        break;
      case 5:
        test.pool.array_address = last;
        break;
      case 6:
        test.pool.slots[7].index = 0;
        break;
      case 7:
        test.pool.slots[7].view_address = 1;
        break;
      case 8:
        test.pool.slots[7].view_address = kView;
        break;
    }
    require(test.run().status == OwnedViewStatus::invalid_pool && test.reader.reads.empty(), "Malformed pool triggered object access");
  }
  for (const auto field : {kControl, kControl + 0x100, kNode + 256, kControl + 0x200, kControl + 0x300, kControl + 0x400, kBitmap + 88,
                           kRecord + 16, kWrapper + 168}) {
    for (const auto malformed : {std::uint64_t{1}, last}) {
      Fixture test;
      test.reader.word(field, malformed);
      require(test.run().status == OwnedViewStatus::invalid_pointer, "Malformed required/optional pointer was accepted");
    }
  }
}

constexpr std::array<std::uint64_t, 5> kHandleFields{kEntry + 96, kView + 104, kEntry + 80, kView + 144, kMaterial + 520};
constexpr std::array<std::uint64_t, 5> kHandlePayloads{kNode, kNode, kMaterial, kMaterial, kBitmap};
constexpr std::uint64_t kPackedControls = 0xc00000;

void pack_controls(Fixture& test, std::uint64_t low) {
  for (unsigned reference = 0; reference < kHandleFields.size(); ++reference) {
    const auto control = kPackedControls + reference * 0x100 + low;
    test.reader.word(kHandleFields[reference], control);
    test.reader.word(control + 28, 29, 4);
    test.reader.word(control, kHandlePayloads[reference]);
    test.reader.region(control, 32);
  }
}

void packed_control_records() {
  for (std::uint64_t low = 1; low <= 7; ++low) {
    Fixture full;
    pack_controls(full, low);
    const auto complete = full.run();
    require(complete.ready && complete.resource_present, "Byte-aligned Node/material/bitmap controls did not resolve");
    for (unsigned reference = 0; reference < kHandleFields.size(); ++reference) {
      const auto control = kPackedControls + reference * 0x100 + low;
      require(std::count(full.reader.reads.begin(), full.reader.reads.end(), std::pair<std::uint64_t, std::size_t>{control, 32}) == 2,
              "Packed control address was masked, adjusted or omitted from full trace");
      for (const bool missing_payload : {false, true}) {
        Fixture test;
        pack_controls(test, low);
        const auto missing = missing_payload ? control : control + 28;
        test.reader.bytes.erase(missing);
        const auto result = test.run();
        require(result.status == OwnedViewStatus::read_failed && result.read_failures == 1 &&
                    test.reader.reads.back() == std::pair<std::uint64_t, std::size_t>{control, 32},
                "Unreadable packed control was classified as alignment refusal, retried or followed");
      }
      Fixture bad_payload;
      pack_controls(bad_payload, low);
      bad_payload.reader.word(control, kHandlePayloads[reference] + low);
      require(bad_payload.run().status == OwnedViewStatus::invalid_pointer,
              "Packed control admission also relaxed Node/material/bitmap payload alignment");
      Fixture stale;
      pack_controls(stale, low);
      stale.reader.word(control + 28, 30, 4);
      const auto result = stale.run();
      require(result.status == (reference < 2   ? OwnedViewStatus::node_unavailable
                                : reference < 4 ? OwnedViewStatus::material_mismatch
                                                : OwnedViewStatus::ready) &&
                  !result.resource_present,
              "Packed stale generation did not retain required/optional handle semantics");
      const auto payload = kHandlePayloads[reference];
      require(std::none_of(stale.reader.reads.begin(), stale.reader.reads.end(),
                           [payload](const auto& read) { return read.first >= payload && read.first - payload < 0x800; }),
              "Stale packed control payload was followed");
    }

    // The entry and view may refer to one control record, rather than separate
    // controls with equal payloads. Preserve the exact shared record too.
    Fixture shared;
    pack_controls(shared, low);
    shared.reader.word(kView + 104, kPackedControls + low);
    shared.reader.word(kView + 144, kPackedControls + 0x200 + low);
    require(shared.run().ready, "Paired entry/view handles cannot share one packed control");
    shared.reader.word(kPackedControls + 0x200 + low + 28, 30, 4);
    const auto absent = shared.run();
    require(absent.ready && !absent.resource_present, "Paired stale material handles prevented camera-only readiness");

    const auto reads = full.reader.reads;
    for (std::size_t call = 0; call < reads.size(); ++call) {
      if (reads[call].first < kPackedControls || reads[call].first >= kPackedControls + 0x500)
        continue;
      Fixture failed;
      pack_controls(failed, low);
      failed.reader.fail_call = call + 1;
      require(failed.run().status == OwnedViewStatus::read_failed && failed.reader.reads.size() == call + 1,
              "Failed packed initial/recheck read did not stop exactly");
      if (call < reads.size() / 2)
        continue;
      for (std::size_t byte = 0; byte < reads[call].second; ++byte) {
        if (!full.reader.observed(reads[call].first + byte))
          continue;
        Fixture changed;
        pack_controls(changed, low);
        changed.reader.changed_call = call + 1;
        changed.reader.changed_byte = byte;
        require(changed.run().status == OwnedViewStatus::changed && changed.reader.reads.size() == call + 1,
                "A packed generation/payload byte changed without refusing all output");
      }
    }
  }
  for (const auto handle : kHandleFields) {
    for (std::uint64_t low = 0; low <= 7; ++low) {
      Fixture test;
      test.reader.word(handle, std::numeric_limits<std::uint64_t>::max() - low);
      require(test.run().status == OwnedViewStatus::invalid_pointer, "Byte-aligned control reads bypassed overflow checks");
    }
  }
}

void failures_and_mutations() {
  Fixture full;
  require(full.run().ready, "Full failure fixture was invalid");
  const auto reads = full.reader.reads;
  std::uint32_t attempted = 0;
  for (std::size_t call = 0; call < reads.size(); ++call) {
    Fixture test;
    attempted += static_cast<std::uint32_t>(reads[call].second);
    test.reader.fail_call = call + 1;
    const auto result = test.run();
    require(!result.complete && !result.ready && result.status == OwnedViewStatus::read_failed && result.read_failures == 1 &&
                result.read_bytes == attempted && test.reader.reads.size() == call + 1,
            "Failed field/recheck was retried, uncounted or published usable addresses");
  }
  const auto half = reads.size() / 2;
  std::size_t unobserved = 0;
  for (std::size_t call = half; call < reads.size(); ++call) {
    for (std::size_t byte = 0; byte < reads[call].second; ++byte) {
      Fixture test;
      test.reader.changed_call = call + 1;
      test.reader.changed_byte = byte;
      const auto result = test.run();
      if (!full.reader.observed(reads[call].first + byte)) {
        // Span bytes between observed fields are engine data that may change
        // every frame. They are read with the span but never compared.
        ++unobserved;
        require(result.ready && test.reader.reads.size() == reads.size(), "A change between observed fields refused the trace");
        continue;
      }
      require(!result.complete && result.status == OwnedViewStatus::changed && test.reader.reads.size() == call + 1,
              "Changed field byte was missed, retried or followed to a new pointer");
    }
  }
  require(unobserved != 0, "Span reads no longer include bytes between observed fields");
  for (const std::size_t byte : {0u, 8u, 16u}) {
    Fixture test;
    test.reader.word(kEntry + 8, 0, 1);
    test.reader.changed_call = 2;
    test.reader.changed_byte = byte;
    require(test.run().status == OwnedViewStatus::changed, "Changed pending entry was accepted as a stable pending state");
  }
}

template <typename T>
concept HasCameraProof = requires(T value) {
  value.camera_address;
  value.resource_address;
  value.fov;
};
static_assert(!std::is_convertible_v<OwnedViewCloseSnapshot, OwnedViewSnapshot>);
static_assert(!HasCameraProof<OwnedViewCloseSnapshot>);

void close_only_contract() {
  Fixture fixture;
  const auto result = fixture.run_close();
  require(result.complete && result.view_address == kView && result.view_index == 0 && result.read_bytes == 576 &&
              fixture.reader.reads.size() == 12,
          "Close-only trace did not retain exactly its sixteen guarded fields in six reads");
  const auto trace = fixture.reader.reads;
  const auto full = fixture.run();
  require(full.dimensions == result.dimensions && full.flags == result.flags && full.read_bytes == 1212,
          "Close-only numeric fields differ from the full inspection");

  Fixture no_graph;
  // Remove EVERY byte outside the actual close trace. A read of the excluded
  // Node payload, Camera, material, Bitmap or resource chain must now fail.
  for (auto it = no_graph.reader.bytes.begin(); it != no_graph.reader.bytes.end();) {
    const auto address = it->first;
    const bool retained = std::any_of(trace.begin(), trace.end(), [address](const auto& field) {
      return address >= field.first && address - field.first < field.second;
    });
    if (retained)
      ++it;
    else
      it = no_graph.reader.bytes.erase(it);
  }
  require(no_graph.run_close().complete, "Unavailable unrelated graphs prevented a validated gate closure");
  require(no_graph.run().status == OwnedViewStatus::read_failed, "Unavailable Camera graph incorrectly produced a full snapshot");

  for (std::size_t call = 0; call < trace.size(); ++call) {
    Fixture failed;
    failed.reader.fail_call = call + 1;
    require(failed.run_close().status == OwnedViewStatus::read_failed && failed.reader.reads.size() == call + 1,
            "A close read/recheck failure was retried or overlooked");
    if (call < trace.size() / 2)
      continue;
    for (std::size_t byte = 0; byte < trace[call].second; ++byte) {
      // The entry span also covers the material handle, which only the full
      // trace observes.
      const auto address = trace[call].first + byte;
      if (!fixture.reader.observed(address) || (address >= kEntry + 80 && address < kEntry + 96))
        continue;
      Fixture changed;
      changed.reader.changed_call = call + 1;
      changed.reader.changed_byte = byte;
      require(changed.run_close().status == OwnedViewStatus::changed && changed.reader.reads.size() == call + 1,
              "A changed close field escaped the complete trace recheck");
    }
  }
  for (std::uint32_t mode : {0u, 1u, 3u, 0xffffffffu}) {
    Fixture wrong_mode;
    wrong_mode.reader.word(kEntry + 24, mode, 4);
    require(!wrong_mode.run_close().complete, "Close-only API admitted a non-mode2 entry");
  }
  for (std::uint32_t ready : {0u, 2u, 255u}) {
    Fixture pending;
    pending.reader.word(kEntry + 8, ready, 1);
    require(!pending.run_close().complete && pending.reader.reads.size() == 1,
            "Close-only API followed a pending or malformed ready entry");
  }
  for (std::uint32_t index : {8u, 0x80000000u, 0xffffffffu}) {
    Fixture invalid;
    invalid.reader.word(kEntry + 76, index, 4);
    require(invalid.run_close().status == OwnedViewStatus::invalid_view_index, "Close-only API admitted an invalid pool index");
  }
  for (unsigned index = 0; index < 8; ++index) {
    Fixture selected;
    selected.reader.word(kEntry + 76, index, 4);
    selected.reader.handle(selected.pool.slots[index].view_address + 104, 1, kNode);
    const auto observed = selected.run_close();
    require(observed.complete && observed.view_index == static_cast<std::int32_t>(index) &&
                observed.view_address == selected.pool.slots[index].view_address,
            "Close-only API selected the wrong pool view");
  }
  for (const auto field : {kEntry, kEntry + 16}) {
    Fixture mismatch;
    mismatch.reader.word(field, kId + 1);
    require(mismatch.run_close().status == OwnedViewStatus::id_mismatch, "Close-only API admitted a different owned ID");
  }
  for (unsigned which = 0; which < 2; ++which) {
    for (std::uint64_t low = 0; low <= 7; ++low) {
      Fixture packed;
      const auto field = which == 0 ? kEntry + 96 : kView + 104;
      const auto control = kControl + which * 0x100 + low;
      packed.reader.word(field, control);
      packed.reader.word(control + 28, 29, 4);
      packed.reader.word(control, kNode);
      packed.reader.region(control, 32);
      require(packed.run_close().complete, "Close-only API rejected a valid byte-aligned control");
      packed.reader.word(control + 28, 30, 4);
      require(packed.run_close().status == OwnedViewStatus::node_unavailable, "Close-only API accepted a stale association");
    }
    Fixture missing;
    missing.reader.word(which == 0 ? kEntry + 96 : kView + 104, 0);
    require(missing.run_close().status == OwnedViewStatus::node_unavailable, "Close-only API accepted an absent association");
  }
  Fixture mismatch;
  mismatch.reader.word(kControl + 0x100, kNode + 8);
  require(mismatch.run_close().status == OwnedViewStatus::node_mismatch, "Close-only API lost entry/view identity equality");
  Fixture invalid_pool;
  invalid_pool.pool.valid = false;
  require(invalid_pool.run_close().status == OwnedViewStatus::invalid_pool && invalid_pool.reader.reads.empty(),
          "Close-only API trusted an invalid pool snapshot");
  Fixture changed_pool;
  changed_pool.reader.word(kArray, kView + 8);
  require(changed_pool.run_close().status == OwnedViewStatus::pool_changed, "Close-only API used a changed pool slot");
  Fixture malformed;
  malformed.entry = std::numeric_limits<std::uint64_t>::max() - 3;
  require(malformed.run_close().status == OwnedViewStatus::invalid_request && malformed.reader.reads.empty(),
          "Close-only API read a malformed address");
  std::printf(
      "Close trace:12 exact reads/576 bytes; full trace:36 exact reads/1212 bytes. OS query counts not measured by synthetic reader.\n");
}
}  // namespace

int main() {
  try {
    close_only_contract();
    successful_and_pending();
    absent_output();
    render_target_records();
    numeric_diagnostics();
    output_dimension_observations();
    mode_observations();
    refusals();
    pointer_and_pool_bounds();
    packed_control_records();
    failures_and_mutations();
    std::printf("PASS: %u owned-view checks; synthetic memory only, no engine/COM calls or acquired references.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
