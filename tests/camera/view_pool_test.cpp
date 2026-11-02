#include "../../src/camera/view_pool.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace taxi_camera::engine_camera;
constexpr std::uint64_t kRenderer = 0x100000;
constexpr std::uint64_t kArray = 0x200000;
constexpr std::uint64_t kViews = 0x300000;
constexpr std::uint64_t kControls = 0x400000;
constexpr std::uint64_t kPayloads = 0x500000;
std::uint32_t checks = 0;

void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

std::uint64_t view(std::uint32_t index) {
  return kViews + index * 0x1000;
}

std::uint64_t control(std::uint32_t index) {
  return kControls + index * 0x1000;
}

struct Reader final : MemoryReader {
  std::map<std::uint64_t, std::uint8_t> bytes;
  std::vector<std::pair<std::uint64_t, std::size_t>> reads;
  std::map<std::uint64_t, unsigned> address_reads;
  std::size_t fail_call = 0;
  std::uint64_t changed_address = 0;
  std::uint32_t changed_byte = 0;

  bool read(std::uint64_t address, void* destination, std::size_t size) override {
    reads.emplace_back(address, size);
    ++address_reads[address];
    if (reads.size() == fail_call)
      return false;
    if (address == changed_address && address_reads[address] == 2)
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
  }
};

struct Fixture {
  Reader reader;
  std::uint64_t renderer = kRenderer;
  std::array<std::uint64_t, 8> control_addresses{};

  Fixture() {
    reader.word(kRenderer + 2752, kArray);
    for (std::uint32_t index = 0; index < 8; ++index) {
      reader.word(kArray + index * 8, view(index));
      association(index, ViewAssociation::occupied);
    }
  }

  void association(std::uint32_t index, ViewAssociation state, std::uint32_t control_offset = 0) {
    control_addresses[index] = control(index) + control_offset;
    reader.word(view(index) + 72, state == ViewAssociation::null_control ? 0 : control_addresses[index]);
    reader.word(view(index) + 80, 7, 4);
    reader.word(view(index) + 84, 0, 4);
    reader.word(control_addresses[index] + 28, state == ViewAssociation::stale_generation ? 8 : 7, 4);
    reader.word(control_addresses[index], state == ViewAssociation::null_payload ? 0 : kPayloads + index * 0x1000);
    // The control record is read as one 32-byte span; its middle is not observed.
    for (std::uint32_t byte = 8; byte < 28; ++byte)
      reader.bytes.try_emplace(control_addresses[index] + byte, 0);
  }

  ViewPoolSnapshot run() {
    reader.reads.clear();
    reader.address_reads.clear();
    const auto result = inspect_view_pool(reader, renderer);
    require(result.read_bytes <= 8192 && result.read_bytes <= 912 && result.slots_examined <= 8,
            "View-pool inspection exceeded its bounded byte/slot allowance");
    std::uint32_t attempted = 0;
    for (const auto& read : reader.reads) {
      // The array pointer, all eight view pointers in one span, each view's
      // association, and each control record as one span. Never a payload.
      bool permitted = (read.first == kRenderer + 2752 && read.second == 8) || (read.first == kArray && read.second == 64);
      for (std::uint32_t index = 0; index < 8; ++index) {
        permitted |= (read.first == view(index) + 72 && read.second == 16) || (read.first == control_addresses[index] && read.second == 32);
      }
      require(permitted, "Reader accessed a global, inline pool, payload object or an unlisted field");
      require(read.first <= std::numeric_limits<std::uint64_t>::max() - read.second, "Reader was called with an overflowing address range");
      attempted += static_cast<std::uint32_t>(read.second);
    }
    require(attempted == result.read_bytes, "Attempted-byte counter omitted failed reads or rechecks");
    for (std::uint32_t index = 0; index < 8; ++index)
      require(result.slots[index].index == index, "View slot indices are not stable");
    if (result.valid) {
      require(
          result.status == ViewPoolStatus::complete && result.slots_examined == 8 && result.read_failures == 0 && result.failure_slot == -1,
          "Complete capacity was published with an incomplete walk");
      require(reader.reads.size() % 2 == 0, "A successful walk did not reread its entire trace");
      const auto half = reader.reads.size() / 2;
      require(std::equal(reader.reads.begin(), reader.reads.begin() + half, reader.reads.begin() + half),
              "Consistency pass changed its addresses or omitted earlier observations");
      for (const auto& slot : result.slots)
        require(slot.free != slot.association_valid && slot.association != ViewAssociation::unobserved,
                "Successful pool result contains ambiguous association status");
    } else {
      require(result.free_count == 0 && result.first_free_indices == std::array<std::int32_t, 4>{-1, -1, -1, -1},
              "Partial or changed pool published actionable capacity");
    }
    return result;
  }
};

void availability_and_exact_layout() {
  Fixture test;
  auto result = test.run();
  require(result.valid && result.array_address == kArray && result.read_bytes == 912 && test.reader.reads.size() == 36 &&
              result.free_count == 0 && result.first_free_indices == std::array<std::int32_t, 4>{-1, -1, -1, -1},
          "A full eight-view pool was misclassified");
  for (std::uint32_t index = 0; index < 8; ++index)
    require(result.slots[index].view_address == view(index) && result.slots[index].association_valid &&
                result.slots[index].association == ViewAssociation::occupied && !result.slots[index].free,
            "An occupied view did not preserve its internal address/status");

  for (const auto state : {ViewAssociation::null_control, ViewAssociation::stale_generation, ViewAssociation::null_payload}) {
    test = Fixture{};
    for (std::uint32_t index = 0; index < 8; ++index)
      test.association(index, state);
    result = test.run();
    require(result.valid && result.free_count == 8 && result.first_free_indices == std::array<std::int32_t, 4>{0, 1, 2, 3},
            "A native free-handle condition was not recognized");
    const auto expected_bytes = state == ViewAssociation::null_control ? 400u : 912u;
    require(result.read_bytes == expected_bytes, "A free association caused unnecessary control reads");
    for (std::uint32_t index = 0; index < 8; ++index) {
      require(result.slots[index].association == state && result.slots[index].free && !result.slots[index].association_valid,
              "Free association reason was lost");
      // A stale control record is read whole, but its payload object never is:
      // run() refuses any read outside the listed pool fields.
      require(test.reader.address_reads[control(index)] == (state == ViewAssociation::null_control ? 0u : 2u),
              "A null handle's control was read, or a control record was not rechecked");
    }
  }
  test = Fixture{};
  test.association(1, ViewAssociation::null_control);
  test.association(2, ViewAssociation::stale_generation);
  test.association(3, ViewAssociation::null_payload);
  test.association(5, ViewAssociation::null_control);
  result = test.run();
  require(result.valid && result.read_bytes == 784 && result.free_count == 4 &&
              result.first_free_indices == std::array<std::int32_t, 4>{1, 2, 3, 5},
          "Mixed pool did not identify the first four distinct free indices in order");
  test = Fixture{};
  test.association(7, ViewAssociation::null_control);
  result = test.run();
  require(result.valid && result.free_count == 1 && result.first_free_indices == std::array<std::int32_t, 4>{7, -1, -1, -1},
          "The last native slot was skipped or a second free slot was invented");
}

void input_and_pointer_refusals() {
  for (const auto renderer :
       {std::uint64_t(0), kRenderer + 1, std::numeric_limits<std::uint64_t>::max() - 7, std::numeric_limits<std::uint64_t>::max() - 2751}) {
    Fixture test;
    test.renderer = renderer;
    const auto result = test.run();
    require(!result.valid && result.status == ViewPoolStatus::invalid_renderer && test.reader.reads.empty(),
            "Null, misaligned or overflowing renderer caused a read");
  }
  for (const auto array : {std::uint64_t(0), kArray + 1, std::numeric_limits<std::uint64_t>::max() - 31}) {
    Fixture test;
    test.reader.word(kRenderer + 2752, array);
    const auto result = test.run();
    require(!result.valid && result.status == ViewPoolStatus::invalid_array && result.read_bytes == 8 && result.slots_examined == 0,
            "Null, misaligned or overflowing view-pointer array was followed");
  }
  for (std::uint32_t index = 0; index < 8; ++index) {
    for (const auto pointer : {std::uint64_t(0), view(index) + 1, std::numeric_limits<std::uint64_t>::max() - 79}) {
      Fixture test;
      test.reader.word(kArray + index * 8, pointer);
      const auto result = test.run();
      require(!result.valid && result.status == ViewPoolStatus::invalid_view && result.slots_examined == index &&
                  result.failure_slot == static_cast<std::int32_t>(index),
              "Invalid view pointer was mistaken for native free capacity");
    }
    for (const auto pointer : {std::numeric_limits<std::uint64_t>::max() - 30, std::numeric_limits<std::uint64_t>::max() - 23}) {
      Fixture test;
      test.reader.word(view(index) + 72, pointer);
      const auto result = test.run();
      require(!result.valid && result.status == ViewPoolStatus::invalid_control && result.slots_examined == index &&
                  result.failure_slot == static_cast<std::int32_t>(index),
              "Malformed control pointer was treated as a free handle");
    }
  }
  Fixture test;
  test.association(0, ViewAssociation::null_control);
  test.reader.word(kArray + 8, view(0));
  const auto result = test.run();
  require(!result.valid && result.status == ViewPoolStatus::duplicate_view && result.failure_slot == 1 && result.slots_examined == 1,
          "Aliased view indices overstated capacity for two distinct creations");
}

void packed_control_records() {
  for (std::uint32_t offset = 1; offset < 8; ++offset) {
    for (const auto state : {ViewAssociation::occupied, ViewAssociation::stale_generation, ViewAssociation::null_payload}) {
      Fixture test;
      for (std::uint32_t index = 0; index < 8; ++index)
        test.association(index, state, offset);
      const auto result = test.run();
      require(result.valid && result.free_count == (state == ViewAssociation::occupied ? 0u : 8u) && result.read_bytes == 912u,
              "Packed generation records changed the native association classification or bounded reads");
      for (std::uint32_t index = 0; index < 8; ++index)
        require(result.slots[index].association == state && test.reader.address_reads[control(index) + offset] == 2,
                "A packed control record was not read and rechecked at its exact address");
    }

    Fixture full;
    full.association(0, ViewAssociation::occupied, offset);
    require(full.run().valid, "Packed control failure fixture is invalid");
    const auto reads = full.reader.reads;
    for (std::size_t call = 0; call < reads.size(); ++call) {
      if (reads[call].first != control(0) + offset && reads[call].first != control(0) + offset + 28)
        continue;
      Fixture failed;
      failed.association(0, ViewAssociation::occupied, offset);
      failed.reader.fail_call = call + 1;
      const auto result = failed.run();
      require(!result.valid && result.status == ViewPoolStatus::read_failed && result.read_failures == 1 && result.failure_slot == 0,
              "An unreadable packed control field or recheck published capacity");
    }
    for (const auto& [address, byte] :
         {std::pair<std::uint64_t, std::uint32_t>{view(0) + 72, 0}, {control(0) + offset, 0}, {control(0) + offset, 28}}) {
      Fixture changed;
      changed.association(0, ViewAssociation::occupied, offset);
      changed.reader.changed_address = address;
      changed.reader.changed_byte = byte;
      const auto result = changed.run();
      require(!result.valid && result.status == ViewPoolStatus::changed && result.read_failures == 0 && result.failure_slot == 0,
              "A changed packed control pointer, generation or payload passed the consistency recheck");
    }

    Fixture invalid_view;
    invalid_view.reader.word(kArray, view(0) + offset);
    const auto refused = invalid_view.run();
    require(!refused.valid && refused.status == ViewPoolStatus::invalid_view && refused.read_bytes == 72,
            "Allowing packed controls also admitted a misaligned raw view pointer");
  }
}

void failures_and_mutations() {
  Fixture full;
  require(full.run().valid, "Complete failure fixture was not valid");
  const auto reads = full.reader.reads;
  std::uint32_t attempted_bytes = 0;
  for (std::size_t index = 0; index < reads.size(); ++index) {
    attempted_bytes += static_cast<std::uint32_t>(reads[index].second);
    Fixture test;
    test.reader.fail_call = index + 1;
    const auto result = test.run();
    require(!result.valid && result.status == ViewPoolStatus::read_failed && result.read_failures == 1 &&
                result.read_bytes == attempted_bytes && test.reader.reads.size() == index + 1,
            "Failed field or recheck read was retried, uncounted or accepted");
  }
  for (std::size_t index = 0; index < reads.size() / 2; ++index) {
    for (std::uint32_t byte = 0; byte < reads[index].second; ++byte) {
      Fixture test;
      test.reader.changed_address = reads[index].first;
      test.reader.changed_byte = byte;
      const auto result = test.run();
      if (reads[index].second == 32 && byte >= 8 && byte < 28) {
        require(result.valid, "A change between a control record's payload and generation refused the pool");
        continue;
      }
      require(!result.valid && result.status == ViewPoolStatus::changed && result.read_failures == 0 && result.slots_examined == 8,
              "Changed pool pointer, view pointer, association byte, generation or payload passed the recheck");
      require(test.reader.reads.size() == reads.size() / 2 + index + 1, "A detected change triggered a restart or followed the new value");
    }
  }
  Fixture test;
  for (std::uint32_t index = 0; index < 8; ++index)
    test.association(index, ViewAssociation::null_control);
  const auto success = test.run();
  require(success.valid && success.free_count == 8, "Free-pool failure fixture was invalid");
  test.reader.fail_call = test.reader.reads.size();
  const auto incomplete = test.run();
  require(!incomplete.valid && incomplete.slots_examined == 8 && incomplete.free_count == 0 &&
              incomplete.first_free_indices == std::array<std::int32_t, 4>{-1, -1, -1, -1},
          "Eight initially free slots bypassed the required final consistency check");
}

void exhaustive_free_masks() {
  for (std::uint32_t mask = 0; mask < 256; ++mask) {
    Fixture test;
    std::uint32_t expected_count = 0;
    std::array<std::int32_t, 4> expected_first{-1, -1, -1, -1};
    for (std::uint32_t index = 0; index < 8; ++index) {
      if ((mask & (1u << index)) == 0)
        continue;
      test.association(index, ViewAssociation::null_control);
      if (expected_count < expected_first.size())
        expected_first[expected_count] = static_cast<std::int32_t>(index);
      ++expected_count;
    }
    const auto result = test.run();
    require(result.valid && result.free_count == expected_count && result.first_free_indices == expected_first,
            "An eight-slot capacity combination was counted or ordered incorrectly");
  }
}

void deferred_renderer_retirement() {
  constexpr std::uint64_t queue = 0x600000;
  const auto prepared = [] {
    Fixture f;
    for (unsigned i = 0; i < 8; ++i) {
      f.reader.word(view(i) + 23688, 0, 1);
      if (i >= 2)
        f.association(i, ViewAssociation::null_control);
    }
    for (auto offset : {2768u, 2784u}) {
      f.reader.word(kRenderer + offset, 0);
      f.reader.word(kRenderer + offset + 8, 0);
    }
    return f;
  };
  const auto run = [](Fixture& f) {
    f.reader.reads.clear();
    f.reader.address_reads.clear();
    auto result = inspect_view_creation_pool(f.reader, f.renderer);
    require(result.read_bytes <= 8192 && result.release_count <= 128, "Retirement inspection exceeded its bound");
    if (result.valid) {
      const auto half = f.reader.reads.size() / 2;
      require(f.reader.reads.size() % 2 == 0 &&
                  std::equal(f.reader.reads.begin(), f.reader.reads.begin() + half, f.reader.reads.begin() + half),
              "Creation inspection did not recheck the entire combined pool/marker/queue trace");
    } else {
      require(!result.creation_available(1) && !result.creation_available(2) && !result.creation_available(3),
              "Incomplete creation evidence published permission");
    }
    return result;
  };
  auto f = prepared();
  auto result = run(f);
  require(result.creation_available(2) && result.creation_available(3) && result.creation_available(4) && !result.creation_available(5),
          "Clean first native-free slots were refused or admitted past four views");
  f.reader.word(view(2) + 23688, 1, 1);
  require(f.run().free_count == 6, "Regression fixture must remain native-free under the old association-only predicate");
  result = run(f);
  require(result.valid && result.free_count == 6 && !result.creation_available(1) && !result.creation_available(2) &&
              !result.creation_available(3),
          "Pending first native-free slot was skipped in favor of later clean slots");
  f.reader.word(view(2) + 23688, 0, 1);
  f.reader.word(view(3) + 23688, 255, 1);
  result = run(f);
  require(result.creation_available(1) && !result.creation_available(2) && !result.creation_available(3),
          "Pair admission ignored a pending second allocation slot");
  f.reader.word(view(3) + 23688, 0, 1);
  f.reader.word(view(4) + 23688, 1, 1);
  result = run(f);
  require(result.creation_available(2) && !result.creation_available(3),
          "Three-feed admission ignored a pending third allocation slot");
  f.reader.word(view(4) + 23688, 0, 1);
  f.reader.word(view(5) + 23688, 1, 1);
  result = run(f);
  require(result.creation_available(3) && !result.creation_available(4), "Four-view admission ignored a pending fourth allocation slot");
  f.reader.word(view(5) + 23688, 0, 1);
  f.reader.word(view(7) + 23688, 1, 1);
  result = run(f);
  require(result.creation_available(2) && result.creation_available(3),
          "An unselected later slot blocked clean native allocation order");

  for (auto offset : {2768u, 2784u}) {
    f = prepared();
    f.reader.word(kRenderer + offset, 31, 4);
    f.reader.word(kRenderer + offset + 4, 1, 4);
    f.reader.word(kRenderer + offset + 8, queue);
    f.reader.word(queue, view(2));
    result = run(f);
    require(result.valid && result.release_count == 1 && !result.creation_available(2),
            "A marker cleared before consumed-queue removal authorized premature reuse");
    RetiredViewPool retired;
    require(retired.retain(kRenderer, view(2)), "Old verified view identity was not retained");
    require(retired.retain(kRenderer, view(3)) && retired.retain(kRenderer, view(4)) && retired.retain(kRenderer, view(5)) &&
                !retired.retain(kRenderer, view(6)),
            "Four-view retirement ledger overflowed or refused the door camera's identity");
    require(!retired.observe(kRenderer, result) && retired.pending(), "ID disappearance was mistaken for renderer retirement");
    require(!retired.observe(kRenderer + 8, result), "A different renderer discarded old pending lifetime evidence");
    f.reader.word(kRenderer + offset, 128, 4);
    f.reader.word(kRenderer + offset + 4, 0, 4);
    result = run(f);
    require(result.creation_available(2) && retired.observe(kRenderer, result) && !retired.pending(),
            "Native drain with retained queue capacity did not unblock the same start");
  }
  f = prepared();
  f.reader.word(kRenderer + 2784 + 4, 65, 4);
  require(!run(f).valid, "Oversized live queue was truncated into an absence proof");
  f = prepared();
  f.reader.word(kRenderer + 2784, UINT32_MAX, 4);
  f.reader.word(kRenderer + 2784 + 4, 1, 4);
  require(!run(f).valid, "Queue capacity arithmetic overflow was accepted");
  for (const auto& [address, byte] :
       {std::pair<std::uint64_t, std::uint32_t>{view(2) + 23688, 0}, {kRenderer + 2768, 0}, {kRenderer + 2784, 0}, {kArray, 16}}) {
    f = prepared();
    f.reader.changed_address = address;
    f.reader.changed_byte = byte;
    require(!run(f).valid, "Changed retirement evidence survived its exact trace reread");
  }
  f = prepared();
  require(run(f).valid, "Read failure baseline invalid");
  const auto calls = f.reader.reads.size();
  for (std::size_t call = 1; call <= calls; ++call) {
    f = prepared();
    f.reader.fail_call = call;
    require(!run(f).valid, "A failed retirement field/readback published allocation permission");
  }
}
}  // namespace

int main() {
  try {
    availability_and_exact_layout();
    input_and_pointer_refusals();
    packed_control_records();
    failures_and_mutations();
    exhaustive_free_masks();
    deferred_renderer_retirement();
    std::printf("PASS: %u bounded view-pool checks; synthetic memory only, no reservations or engine calls.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
