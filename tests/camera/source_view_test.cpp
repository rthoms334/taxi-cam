#include "../../src/camera/source_view.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace taxi_camera;
constexpr std::uint64_t kArray = 0x100000;
constexpr std::uint64_t kSource = 0x200000;
constexpr std::uint64_t kControl = 0x300000;
constexpr std::uint64_t kNode = 0x400000;
constexpr std::uint64_t kCamera = 0x500000;
constexpr std::uint64_t kMatrix = 0x600000;
std::uint32_t checks = 0;

void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

struct Reader final : engine_camera::MemoryReader {
  std::map<std::uint64_t, std::uint8_t> bytes;
  std::vector<std::pair<std::uint64_t, std::size_t>> reads;
  std::vector<std::pair<std::uint64_t, std::size_t>> allowed;
  std::size_t fail_call = 0;
  std::size_t change_call = 0;
  std::size_t change_byte = 0;

  bool read(std::uint64_t address, void* destination, std::size_t size) override {
    reads.emplace_back(address, size);
    if (reads.size() == fail_call)
      return false;
    if (reads.size() == change_call)
      bytes[address + change_byte] ^= 1;
    auto* output = static_cast<std::uint8_t*>(destination);
    for (std::size_t index = 0; index < size; ++index) {
      const auto found = bytes.find(address + index);
      if (found == bytes.end())
        return false;
      output[index] = found->second;
    }
    return true;
  }

  void word(std::uint64_t address, std::uint64_t value, std::size_t size = 8) {
    for (std::size_t index = 0; index < size; ++index)
      bytes[address + index] = static_cast<std::uint8_t>(value >> (index * 8));
    allowed.emplace_back(address, size);
  }

  void vector(std::uint64_t address, const std::array<double, 3>& values) {
    for (unsigned index = 0; index < 3; ++index)
      word(address + index * 8, std::bit_cast<std::uint64_t>(values[index]));
    allowed.emplace_back(address, 24);
  }
};

struct Fixture {
  Reader reader;
  engine_camera::ViewPoolSnapshot pool;

  Fixture() {
    pool.valid = true;
    pool.status = engine_camera::ViewPoolStatus::complete;
    pool.slots_examined = 8;
    pool.array_address = kArray;
    for (unsigned index = 0; index < 8; ++index) {
      const std::uint64_t offset = index * 0x1000;
      auto& slot = pool.slots[index];
      slot.index = index;
      slot.view_address = kSource + offset;
      slot.association = engine_camera::ViewAssociation::occupied;
      slot.association_valid = true;
      reader.word(kArray + index * 8, slot.view_address);
      reader.word(kSource + offset + 104, kControl + offset);
      reader.word(kSource + offset + 112, 31, 4);
      reader.word(kSource + offset + 116, 41, 4);
      reader.allowed.emplace_back(kSource + offset + 104, 16);
      reader.word(kControl + offset + 28, 31, 4);
      reader.word(kControl + offset, kNode + offset);
      reader.word(kNode + offset + 256, kCamera + offset);
      reader.word(kCamera + offset + 160, 7, 2);
      reader.word(kNode + offset + 296, kMatrix + offset);
      reader.vector(kMatrix + offset + 96, {12, -1234, 0.005});
      reader.word(kCamera + offset + 1616, std::bit_cast<std::uint32_t>(1.5f), 4);
      reader.vector(kCamera + offset + 1648, {1, 0, 0});
      reader.vector(kCamera + offset + 1680, {0, 1, 0});
      reader.vector(kCamera + offset + 1712, {0, 0, 1});
    }
  }

  native_camera::SourceViewSnapshot run(bool explicit_source = false, std::uint64_t address = kSource) {
    reader.reads.clear();
    const auto result =
        explicit_source ? native_camera::inspect_source_pose(reader, address) : native_camera::inspect_source_view(reader, pool);
    std::uint32_t attempted = 0;
    for (const auto& read : reader.reads) {
      require(std::find(reader.allowed.begin(), reader.allowed.end(), read) != reader.allowed.end(), "An unlisted field was read");
      require(read.first <= std::numeric_limits<std::uint64_t>::max() - read.second, "An overflowing range was passed to reader");
      require(!explicit_source || read.first < kArray || read.first >= kArray + 64, "Explicit source inspection accessed a pool");
      attempted += static_cast<std::uint32_t>(read.second);
    }
    require(attempted == result.read_bytes && attempted <= 2464 && result.read_failures <= 1 && result.candidates_examined <= 8,
            "Byte/failure/candidate bounds were not enforced");
    if (result.complete || result.status == native_camera::SourceViewStatus::no_source) {
      require(reader.reads.size() % 2 == 0 && result.read_failures == 0, "Complete/no-source result skipped its rechecks");
      const auto half = reader.reads.size() / 2;
      require(std::equal(reader.reads.begin(), reader.reads.begin() + half, reader.reads.begin() + half),
              "Consistency trace omitted a field");
    }
    if (result.complete) {
      require(result.status == native_camera::SourceViewStatus::ready && result.error[0] == 0 && result.fov > 0,
              "Usable source retained an error or invalid metadata");
    } else {
      require(result.source_address == 0 && result.node_address == 0 && result.camera_address == 0 && result.fov == 0 &&
                  result.view_index == -1,
              "Unusable source published borrowed metadata");
    }
    return result;
  }
};

void success_and_selection() {
  Fixture full;
  auto result = full.run();
  require(result.complete && result.source_address == kSource && result.node_address == kNode && result.camera_address == kCamera &&
              result.view_index == 0 && result.candidates_examined == 1 && result.fov == 1.5f && result.read_bytes == 308,
          "First occupied source was not verified exactly");
  Fixture skipped_aircraft_camera;
  skipped_aircraft_camera.reader.vector(kMatrix + 96, {1, 2, 3});
  skipped_aircraft_camera.reader.reads.clear();
  std::array<double, 3> selected{};
  const auto matched = native_camera::select_source_view(
      skipped_aircraft_camera.reader, skipped_aircraft_camera.pool,
      [](const std::array<double, 3>& translation, float, void*) noexcept { return translation[0] == 12; }, nullptr, &selected);
  require(matched.complete && matched.view_index == 1 && matched.candidates_examined == 2 && selected[0] == 12 && selected[1] == -1234,
          "A rejected first camera did not yield the later public-view camera");
  selected = {9, 9, 9};
  const auto none = native_camera::select_source_view(
      skipped_aircraft_camera.reader, skipped_aircraft_camera.pool,
      [](const std::array<double, 3>&, float, void*) noexcept { return false; }, nullptr, &selected);
  require(!none.complete && none.status == native_camera::SourceViewStatus::no_source && selected == std::array<double, 3>{} &&
              none.candidates_examined == 8,
          "Rejected cameras published a position or stopped the scan");
  result = full.run(true);
  require(result.complete && result.view_index == -1 && result.read_bytes == 292 && result.source_address == kSource,
          "Explicit source did not use the same bounded pose validation");
  for (unsigned first = 0; first < 8; ++first) {
    Fixture test;
    for (unsigned index = 0; index < first; ++index)
      test.reader.word(kCamera + index * 0x1000 + 160, 6, 2);
    result = test.run();
    require(result.complete && result.view_index == static_cast<std::int32_t>(first) && result.candidates_examined == first + 1,
            "Selection did not skip non-Camera occupied views in order");
  }
  Fixture free_pool;
  for (auto& slot : free_pool.pool.slots) {
    slot.association = engine_camera::ViewAssociation::null_control;
    slot.association_valid = false;
    slot.free = true;
  }
  require(free_pool.run().status == native_camera::SourceViewStatus::no_source && free_pool.reader.reads.empty(),
          "Free pool slots were treated as scene sources");
  Fixture maximum;
  for (unsigned index = 0; index < 8; ++index)
    maximum.reader.vector(kCamera + index * 0x1000 + 1712, {0, 1, 0});
  result = maximum.run();
  require(result.status == native_camera::SourceViewStatus::no_source && result.read_bytes == 2464 && result.candidates_examined == 8,
          "Eight unusable full pose candidates exceeded or bypassed the complete trace budget");
  Fixture rotation;
  const auto angle = 0.82;
  rotation.reader.vector(kCamera + 1648, {std::cos(angle), std::sin(angle), 0});
  rotation.reader.vector(kCamera + 1680, {-std::sin(angle), std::cos(angle), 0});
  require(rotation.run(true).complete, "A rotated orthonormal basis was rejected");
}

void unavailable_and_basis() {
  for (const auto address : {kSource + 104, kControl, kNode + 256, kNode + 296}) {
    Fixture test;
    test.reader.word(address, 0);
    require(test.run(true).status == native_camera::SourceViewStatus::no_source, "Absent source member was accepted");
  }
  Fixture stale;
  stale.reader.word(kControl + 28, 32, 4);
  require(stale.run(true).status == native_camera::SourceViewStatus::no_source, "Stale Node handle was followed");
  require(std::none_of(stale.reader.reads.begin(), stale.reader.reads.end(), [](const auto& read) { return read.first == kControl; }),
          "Generation mismatch did not prevent payload access");
  for (const auto type : {0u, 8u, 0x107u}) {
    Fixture test;
    test.reader.word(kCamera + 160, type, 2);
    require(test.run(true).status == native_camera::SourceViewStatus::no_source, "Wrong Camera WORD type was accepted");
  }
  for (const auto bits : {0u, 0x80000000u, 0xbf800000u, 0x7f800000u, 0xff800000u, 0x7fc00000u}) {
    Fixture test;
    test.reader.word(kCamera + 1616, bits, 4);
    require(test.run(true).status == native_camera::SourceViewStatus::no_source, "Nonfinite/nonpositive FOV was accepted");
  }
  for (const auto base : {kMatrix + 96, kCamera + 1648, kCamera + 1680, kCamera + 1712}) {
    for (unsigned coordinate = 0; coordinate < 3; ++coordinate) {
      for (const auto invalid :
           {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        Fixture test;
        test.reader.word(base + coordinate * 8, std::bit_cast<std::uint64_t>(invalid));
        require(test.run(true).status == native_camera::SourceViewStatus::no_source, "Nonfinite translation/basis coordinate was accepted");
      }
    }
  }
  for (const auto row : {std::array<double, 3>{0, 0, 0}, {2, 0, 0}, {1, 0.2, 0}, {1e308, 0, 0}}) {
    Fixture test;
    test.reader.vector(kCamera + 1648, row);
    require(test.run(true).status == native_camera::SourceViewStatus::no_source, "Degenerate/scaled/nonorthogonal basis was accepted");
  }
  for (const auto delta : {0.0004, 0.0006}) {
    Fixture test;
    test.reader.vector(kCamera + 1648, {1 + delta, 0, 0});
    require(test.run(true).complete == (delta == 0.0004), "Basis norm-squared tolerance is not 1e-3");
  }
}

void pointers_and_pool() {
  constexpr auto last = std::numeric_limits<std::uint64_t>::max() - 7;
  for (const auto address : {std::uint64_t{0}, std::uint64_t{1}, last}) {
    Fixture test;
    require(test.run(true, address).status == native_camera::SourceViewStatus::invalid_pointer && test.reader.reads.empty(),
            "Invalid explicit source triggered access");
  }
  for (const auto field : {kControl, kNode + 256, kNode + 296}) {
    for (const auto invalid : {std::uint64_t{1}, last}) {
      Fixture test;
      test.reader.word(field, invalid);
      require(test.run(true).status == native_camera::SourceViewStatus::invalid_pointer, "Malformed source pointer was followed");
    }
  }
  for (unsigned mutation = 0; mutation < 11; ++mutation) {
    Fixture test;
    switch (mutation) {
      case 0:
        test.pool.valid = false;
        break;
      case 1:
        test.pool.status = engine_camera::ViewPoolStatus::changed;
        break;
      case 2:
        test.pool.slots_examined = 7;
        break;
      case 3:
        test.pool.read_failures = 1;
        break;
      case 4:
        test.pool.array_address = last;
        break;
      case 5:
        test.pool.slots[7].index = 0;
        break;
      case 6:
        test.pool.slots[7].view_address = 1;
        break;
      case 7:
        test.pool.slots[7].view_address = kSource;
        break;
      case 8:
        test.pool.slots[7].association = engine_camera::ViewAssociation::unobserved;
        break;
      case 9:
        test.pool.slots[7].association_valid = false;
        break;
      case 10:
        test.pool.slots[7].free = true;
        break;
    }
    require(test.run().status == native_camera::SourceViewStatus::invalid_pool && test.reader.reads.empty(),
            "Malformed/inconsistent pool triggered candidate reads");
  }
  Fixture changed;
  changed.reader.word(kArray, kSource + 8);
  require(changed.run().status == native_camera::SourceViewStatus::pool_changed, "Changed pool address was followed");
}

void packed_control_records() {
  constexpr std::uint64_t packed_base = 0xc00000;
  for (std::uint64_t low = 1; low <= 7; ++low) {
    const auto packed = packed_base + low;
    for (const bool explicit_source : {false, true}) {
      Fixture test;
      test.reader.word(kSource + 104, packed);
      test.reader.word(packed + 28, 31, 4);
      test.reader.word(packed, kNode);
      const auto result = test.run(explicit_source);
      require(result.complete && result.node_address == kNode && result.rejected_alignment == 0,
              "A readable byte-aligned control record was rejected");
      require(std::count(test.reader.reads.begin(), test.reader.reads.end(), std::pair<std::uint64_t, std::size_t>{packed + 28, 4}) == 2 &&
                  std::count(test.reader.reads.begin(), test.reader.reads.end(), std::pair<std::uint64_t, std::size_t>{packed, 8}) == 2,
              "Packed generation/payload address was adjusted, masked or omitted from recheck");
    }
    Fixture unreadable;
    unreadable.reader.word(kSource + 104, packed);
    unreadable.reader.allowed.emplace_back(packed + 28, 4);
    auto result = unreadable.run(true);
    require(result.status == native_camera::SourceViewStatus::read_failed && result.read_failures == 1 && result.read_bytes == 20 &&
                std::strcmp(result.field, "node_handle.generation") == 0 && result.rejected_alignment == 0,
            "Unreadable packed generation was classified as alignment refusal or lost its field diagnostics");
    unreadable.reader.word(packed + 28, 31, 4);
    unreadable.reader.allowed.emplace_back(packed, 8);
    result = unreadable.run(true);
    require(result.status == native_camera::SourceViewStatus::read_failed && result.read_bytes == 28 &&
                std::strcmp(result.field, "node_handle.payload") == 0,
            "Unreadable packed payload was followed or lost its field diagnostics");

    Fixture stale;
    stale.reader.word(kSource + 104, packed);
    stale.reader.word(packed + 28, 32, 4);
    result = stale.run(true);
    require(result.status == native_camera::SourceViewStatus::no_source && result.read_bytes == 40 &&
                std::strcmp(result.field, "node_handle.generation") == 0,
            "A stale packed control record was followed to its payload");

    Fixture bad_payload;
    bad_payload.reader.word(kSource + 104, packed);
    bad_payload.reader.word(packed + 28, 31, 4);
    bad_payload.reader.word(packed, kNode + low);
    result = bad_payload.run(true);
    require(result.status == native_camera::SourceViewStatus::invalid_pointer && result.rejected_alignment == low &&
                std::strcmp(result.field, "node.camera_pointer") == 0 && result.read_bytes == 28,
            "Control admission incorrectly relaxed the Node payload alignment guard");

    for (const auto missing_call : {2u, 3u, 13u, 14u}) {
      Fixture test;
      test.reader.word(kSource + 104, packed);
      test.reader.word(packed + 28, 31, 4);
      test.reader.word(packed, kNode);
      test.reader.fail_call = missing_call;
      result = test.run(true);
      require(
          result.status == native_camera::SourceViewStatus::read_failed && test.reader.reads.size() == missing_call &&
              std::strcmp(result.field, missing_call == 2 || missing_call == 13 ? "node_handle.generation" : "node_handle.payload") == 0,
          "Packed initial/recheck read failure lost its exact field or continued reading");
    }
    for (const auto changed_call : {13u, 14u}) {
      const auto size = changed_call == 13 ? 4u : 8u;
      for (unsigned byte = 0; byte < size; ++byte) {
        Fixture test;
        test.reader.word(kSource + 104, packed);
        test.reader.word(packed + 28, 31, 4);
        test.reader.word(packed, kNode);
        test.reader.change_call = changed_call;
        test.reader.change_byte = byte;
        result = test.run(true);
        require(result.status == native_camera::SourceViewStatus::changed && test.reader.reads.size() == changed_call &&
                    std::strcmp(result.field, changed_call == 13 ? "node_handle.generation" : "node_handle.payload") == 0,
                "Packed control byte mutation was missed or lost field diagnostics");
      }
    }
  }
  for (std::uint64_t low = 0; low <= 7; ++low) {
    Fixture overflow;
    overflow.reader.word(kSource + 104, std::numeric_limits<std::uint64_t>::max() - low);
    const auto result = overflow.run(true);
    require(result.status == native_camera::SourceViewStatus::invalid_pointer && result.read_bytes == 16 &&
                std::strcmp(result.field, "node_handle.generation") == 0,
            "Byte-aligned control admission bypassed the overflowing field range guard");
  }
}

void failures_and_mutations() {
  for (const bool explicit_source : {false, true}) {
    Fixture full;
    require(full.run(explicit_source).complete, "Failure fixture was not complete");
    const auto reads = full.reader.reads;
    std::uint32_t attempted = 0;
    for (std::size_t call = 0; call < reads.size(); ++call) {
      Fixture test;
      attempted += static_cast<std::uint32_t>(reads[call].second);
      test.reader.fail_call = call + 1;
      const auto result = test.run(explicit_source);
      require(result.status == native_camera::SourceViewStatus::read_failed && result.read_failures == 1 &&
                  result.read_bytes == attempted && test.reader.reads.size() == call + 1,
              "Failed read/recheck was retried or published source metadata");
    }
    for (std::size_t call = reads.size() / 2; call < reads.size(); ++call) {
      for (std::size_t byte = 0; byte < reads[call].second; ++byte) {
        Fixture test;
        test.reader.change_call = call + 1;
        test.reader.change_byte = byte;
        const auto result = test.run(explicit_source);
        require(result.status == native_camera::SourceViewStatus::changed && test.reader.reads.size() == call + 1,
                "A changed observed byte was ignored or followed");
      }
    }
  }
  Fixture skipped;
  skipped.reader.word(kCamera + 160, 6, 2);
  require(skipped.run().view_index == 1, "Skipped-candidate fixture did not choose second source");
  skipped.reader.change_call = skipped.reader.reads.size() / 2 + 6;
  require(skipped.run().status == native_camera::SourceViewStatus::changed, "Skipped candidate fields were omitted from recheck");
}

}  // namespace

void numeric_position_output() {
  Fixture complete;
  std::array<double, 3> position{};
  const auto result = native_camera::inspect_source_pose(complete.reader, kSource, &position);
  require(result.complete && position == std::array<double, 3>{12, -1234, 0.005}, "Validated numeric translation was not published");
  require(result.read_bytes == 292, "Numeric output added target reads");
  const auto reads = complete.reader.reads;
  for (std::size_t index = 1; index <= reads.size(); ++index) {
    Fixture fail;
    fail.reader.fail_call = index;
    position = {1, 2, 3};
    const auto failure = native_camera::inspect_source_pose(fail.reader, kSource, &position);
    require(!failure.complete && position == std::array<double, 3>{}, "Failed read leaked numeric translation");
  }
  for (std::size_t index = reads.size() / 2 + 1; index <= reads.size(); ++index) {
    Fixture changed;
    changed.reader.change_call = index;
    position = {1, 2, 3};
    const auto failure = native_camera::inspect_source_pose(changed.reader, kSource, &position);
    require(!failure.complete && position == std::array<double, 3>{}, "Changed trace leaked numeric translation");
  }
  Fixture invalid;
  invalid.reader.word(kCamera + 160, 6, 2);
  position = {1, 2, 3};
  require(!native_camera::inspect_source_pose(invalid.reader, kSource, &position).complete && position == std::array<double, 3>{},
          "Unusable camera retained numeric output");
}

int main() {
  try {
    success_and_selection();
    unavailable_and_basis();
    pointers_and_pool();
    packed_control_records();
    failures_and_mutations();
    numeric_position_output();
    std::printf("PASS: %u bounded source-view checks; synthetic memory only, no engine calls or pose conversion.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
