#include "../../src/camera/owned_entry_inventory.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace {
using namespace taxi_camera::engine_camera;

unsigned checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}

constexpr std::uint64_t Manager = 0x10000000;
constexpr std::uint64_t Buckets = 0x18000000;
constexpr std::uint64_t FirstNode = 0x20000000;
constexpr std::array<std::uint64_t, 4> OwnedIds = {41, 99, 0, 0};

std::uint64_t node_address(std::uint32_t index) {
  return FirstNode + std::uint64_t(index) * 0x1000;
}

struct Memory final : MemoryReader {
  std::map<std::uint64_t, std::uint8_t> bytes;
  std::map<std::uint64_t, unsigned> occurrences;
  std::vector<std::pair<std::uint64_t, std::size_t>> reads;
  std::uint64_t fail_at = 0;
  std::uint64_t fail_on_repeat = 0;
  std::uint64_t change_on_repeat = 0;
  std::uint32_t change_byte = 0;
  std::uint32_t attempted = 0;
  std::uint32_t shared_allowance = kOwnedEntryReadLimit;

  void integer(std::uint64_t address, std::uint64_t value, unsigned size = 8) {
    for (unsigned i = 0; i < size; ++i)
      bytes[address + i] = static_cast<std::uint8_t>(value >> (8 * i));
  }
  bool read(std::uint64_t address, void* destination, std::size_t size) override {
    require(size == 8 || (address == Manager + 88 && size == 16) ||
                (address == Buckets && size != 0 && size % 8 == 0 && size <= kOwnedEntryBucketLimit * 8),
            "Reader accessed an unapproved field size");
    require(attempted + size <= kOwnedEntryReadLimit, "Core exceeded its hard attempted-read cap");
    attempted += static_cast<std::uint32_t>(size);
    reads.emplace_back(address, size);
    const auto occurrence = ++occurrences[address];
    const auto contains = [&](std::uint64_t location) { return location >= address && location - address < size; };
    if (attempted > shared_allowance || contains(fail_at) || (contains(fail_on_repeat) && occurrence > 1))
      return false;
    auto* output = static_cast<std::uint8_t*>(destination);
    for (std::size_t i = 0; i < size; ++i) {
      const auto found = bytes.find(address + i);
      if (found == bytes.end())
        return false;
      output[i] = found->second;
    }
    if (address == change_on_repeat && occurrence > 1) {
      require(change_byte < size, "Synthetic mutation is outside the observed field");
      output[change_byte] ^= 1;
    }
    return true;
  }
};

struct Fixture {
  Memory memory;

  void header(std::uint32_t entries, std::uint32_t bucket_count, std::uint64_t pointer = Buckets) {
    memory.integer(Manager + 88, entries, 4);
    memory.integer(Manager + 92, bucket_count, 4);
    memory.integer(Manager + 96, pointer);
    if (pointer == Buckets && bucket_count <= kOwnedEntryBucketLimit)
      for (std::uint32_t index = 0; index < bucket_count; ++index)
        memory.integer(Buckets + std::uint64_t(index) * 8, 0);
  }
  void node(std::uint32_t index, std::uint64_t key, std::uint64_t next = 0) {
    const auto address = node_address(index);
    memory.integer(address, key);
    memory.integer(address + 16, key);
    memory.integer(address + 296, next);
  }
  void normal() {
    header(3, 4);
    node(0, OwnedIds[0], node_address(1));
    node(1, 72, 1);  // The low tag alone is a terminal link after masking.
    node(2, OwnedIds[1]);
    memory.integer(Buckets + 8, node_address(0));
    memory.integer(Buckets + 24, node_address(2));
  }
  OwnedEntryInventory run(std::uint64_t manager = Manager, std::array<std::uint64_t, 4> ids = OwnedIds) {
    const auto result = inspect_owned_entries(memory, manager, ids);
    require(result.read_bytes == memory.attempted, "Attempted-read accounting disagrees with the adapter");
    require(result.complete == (result.error[0] == '\0'), "Complete/error states disagree");
    if (!result.complete) {
      for (const auto& entry : result.entries)
        require(!entry.found && entry.address == 0 && entry.key == 0 && entry.payload_id == 0,
                "An incomplete scan published an owned entry address or ID");
    }
    return result;
  }
};

void valid_membership() {
  {
    Fixture fixture;
    fixture.normal();
    const auto result = fixture.run();
    require(result.complete && result.entry_count == 3 && result.bucket_count == 4 && result.nodes_visited == 3 &&
                result.read_bytes == 240 && result.read_failures == 0,
            "Valid table traversal failed or read beyond exact metadata");
    for (std::size_t index = 0; index < OwnedIds.size(); ++index) {
      const auto& entry = result.entries[index];
      if (!OwnedIds[index]) {
        require(!entry.found && entry.address == 0 && entry.key == 0 && entry.payload_id == 0,
                "An unused third owned-id slot published a membership result");
        continue;
      }
      require(entry.found && entry.address == node_address(index == 0 ? 0 : 2) && entry.key == OwnedIds[index] &&
                  entry.payload_id == OwnedIds[index],
              "An owned entry did not resolve to its exact key, payload ID and address");
    }
    for (const auto& [address, count] : fixture.memory.occurrences) {
      (void)address;
      require(count == 2, "A table/header/node field was not reread exactly once");
    }
    require(fixture.memory.occurrences[Manager + 104] == 0, "The verifier followed the retained-entry reuse list");
    require(fixture.memory.reads.size() == 22 && fixture.memory.reads[1] == std::pair<std::uint64_t, std::size_t>{Buckets, 32},
            "Shared bucket heads were not read as one exact contiguous field");
  }
  for (const auto ids : {std::array<std::uint64_t, 4>{99, 41, 0}, {999, 99, 0}, {0, 41, 0}, {41, 0, 0}, {0, 0, 0}, {999, 1000, 0}}) {
    Fixture fixture;
    fixture.normal();
    const auto result = fixture.run(Manager, ids);
    require(result.complete && result.read_bytes == 240, "Absent or unused IDs prevented a complete snapshot");
    for (std::size_t index = 0; index < ids.size(); ++index)
      require(result.entries[index].found == (ids[index] == 41 || ids[index] == 99),
              "Membership result did not correspond to its requested ID slot");
  }
  // The engine traversal masks only the low bit, even on a nonterminal link.
  {
    Fixture fixture;
    fixture.normal();
    fixture.memory.integer(node_address(0) + 296, node_address(1) | 1);
    const auto result = fixture.run();
    require(result.complete && result.entries[0].found && result.entries[1].found,
            "A captured low-bit-tagged next link was dereferenced without masking");
  }
  // Full bucket traversal does not require a guessed power-of-two rule.
  {
    Fixture fixture;
    fixture.header(1, 3);
    fixture.node(0, 41);
    fixture.memory.integer(Buckets + 16, node_address(0));
    const auto result = fixture.run();
    require(result.complete && result.entries[0].found && !result.entries[1].found,
            "Full traversal depended on an undocumented bucket-mask constraint");
  }
}

void empty_and_invalid_headers() {
  for (const auto count : {0u, 4u, 4096u}) {
    Fixture fixture;
    fixture.header(0, count, count == 0 ? 0 : Buckets);
    const auto result = fixture.run();
    require(result.complete && result.nodes_visited == 0 && !result.entries[0].found && !result.entries[1].found &&
                result.read_bytes == 32 + count * 16,
            "Valid uninitialized or allocated empty table was not completely verified");
  }
  {
    Fixture fixture;
    fixture.header(0, 1);
    fixture.node(0, OwnedIds[0]);
    fixture.memory.integer(Buckets, node_address(0));
    const auto result = fixture.run();
    require(!result.complete && result.nodes_visited == 1 && result.read_failures == 0,
            "A zero declared count hid a still-linked owned entry");
  }
  for (unsigned scenario = 0; scenario < 7; ++scenario) {
    Fixture fixture;
    if (scenario == 0)
      fixture.header(1, 0, 0);
    if (scenario == 1)
      fixture.header(0, 0, Buckets);
    if (scenario == 2)
      fixture.header(0, 1, 0);
    if (scenario == 3)
      fixture.header(1025, 4);
    if (scenario == 4)
      fixture.header(1, 4097);
    if (scenario == 5)
      fixture.header(0xffffffff, 4);
    if (scenario == 6)
      fixture.header(0, 0xffffffff);
    const auto result = fixture.run();
    require(!result.complete && result.read_bytes == 16 && result.read_failures == 0,
            "An inconsistent/over-cap header permitted table traversal");
  }
  for (const auto manager : {std::uint64_t{0}, Manager + 1, std::numeric_limits<std::uint64_t>::max() - 7}) {
    Fixture fixture;
    const auto result = fixture.run(manager);
    require(!result.complete && result.read_bytes == 0, "Null, misaligned or overflowing manager reached the reader");
  }
  {
    Fixture fixture;
    const auto result = fixture.run(Manager, {41, 41});
    require(!result.complete && result.read_bytes == 0, "Duplicate owned IDs were not refused before access");
  }
}

void malformed_nodes() {
  for (unsigned scenario = 0; scenario < 9; ++scenario) {
    Fixture fixture;
    fixture.normal();
    if (scenario == 0)
      fixture.memory.integer(Buckets + 8, node_address(0) | 1);
    if (scenario == 1)
      fixture.memory.integer(node_address(0) + 296, node_address(1) + 2);
    if (scenario == 2)
      fixture.memory.integer(node_address(0) + 296, node_address(0));
    if (scenario == 3)
      fixture.memory.integer(node_address(1) + 296, node_address(0));
    if (scenario == 4)
      fixture.memory.integer(Buckets + 16, node_address(0));
    if (scenario == 5)
      fixture.memory.integer(node_address(1), OwnedIds[0]);
    if (scenario == 6)
      fixture.memory.integer(node_address(0) + 16, 1234);
    if (scenario == 7)
      fixture.memory.integer(Manager + 88, 2, 4);
    if (scenario == 8)
      fixture.memory.integer(Manager + 88, 4, 4);
    const auto result = fixture.run();
    require(!result.complete && result.read_failures == 0, "Malformed, duplicated, cyclic or inconsistent table claimed completeness");
  }
  for (const auto pointer : {Buckets + 1, std::numeric_limits<std::uint64_t>::max() - 7}) {
    Fixture fixture;
    fixture.header(0, 1, pointer);
    const auto result = fixture.run();
    require(!result.complete && result.read_bytes == 16 && result.read_failures == 0, "Misaligned/overflowing bucket pointer was read");
  }
  {
    Fixture fixture;
    fixture.header(1, 1);
    constexpr auto address = std::numeric_limits<std::uint64_t>::max() - 15;
    fixture.memory.integer(Buckets, address);
    fixture.memory.integer(address, 41);
    const auto result = fixture.run();
    require(!result.complete && result.read_failures == 0 && fixture.memory.occurrences[address + 16] == 0,
            "Overflowing node field offset reached the reader");
  }
}

void failed_and_changed_snapshots() {
  Fixture baseline;
  baseline.normal();
  require(baseline.run().complete, "Mutation fixture failed initially");
  // Exercise every header/head/node read, including empty heads and unowned IDs.
  for (const auto& [address, count] : baseline.memory.occurrences) {
    if (count == 0)
      continue;
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
      Fixture fixture;
      fixture.normal();
      if (scenario == 0)
        fixture.memory.fail_at = address;
      if (scenario == 1)
        fixture.memory.fail_on_repeat = address;
      if (scenario == 2)
        fixture.memory.change_on_repeat = address;
      const auto result = fixture.run();
      require(!result.complete && result.read_failures == (scenario == 2 ? 0u : 1u),
              "Failed or changed field survived the full snapshot verification");
    }
  }
  for (const auto byte : {4u, 8u}) {
    Fixture fixture;
    fixture.normal();
    fixture.memory.change_on_repeat = Manager + 88;
    fixture.memory.change_byte = byte;
    require(!fixture.run().complete, "Changed bucket count or array pointer survived the final header recheck");
  }
  {
    Fixture fixture;
    fixture.normal();
    fixture.memory.shared_allowance = 80;
    const auto result = fixture.run();
    require(!result.complete && result.read_failures == 1 && result.read_bytes > 80 && result.read_bytes <= 96,
            "A native adapter's smaller shared allowance was ignored or undercounted");
  }
}

void maximum_bounds() {
  Fixture fixture;
  fixture.header(kOwnedEntryNodeLimit, kOwnedEntryBucketLimit);
  for (std::uint32_t index = 0; index < kOwnedEntryNodeLimit; ++index) {
    fixture.node(index, 10000 + index);
    fixture.memory.integer(Buckets + std::uint64_t(index) * 8, node_address(index));
  }
  const auto result = fixture.run(Manager, {10000, 11023});
  require(result.complete && result.entries[0].found && result.entries[1].found && result.nodes_visited == 1024 &&
              result.bucket_count == 4096 && result.read_bytes == 114720 && result.read_bytes <= kOwnedEntryReadLimit,
          "Maximum permitted table exceeded the read or distinct-node limits");
  require(fixture.memory.occurrences[Buckets + std::uint64_t(kOwnedEntryBucketLimit) * 8] == 0,
          "Maximum table accessed a head outside the declared bucket count");
  require(fixture.memory.reads.size() == 4 + 6 * kOwnedEntryNodeLimit && fixture.memory.occurrences[Buckets] == 2,
          "Maximum table reverted to separate per-head reads or omitted its full reread");
  std::printf("Maximum owned-entry snapshot: %u attempted bytes for %u buckets/%u nodes with complete rereads.\n", result.read_bytes,
              result.bucket_count, result.nodes_visited);
  {
    Fixture excess;
    excess.header(1024, 1);
    excess.memory.integer(Buckets, node_address(0));
    for (std::uint32_t index = 0; index <= 1024; ++index)
      excess.node(index, 20000 + index, index == 1024 ? 0 : node_address(index + 1));
    const auto stopped = excess.run();
    require(!stopped.complete && stopped.nodes_visited == 1024 && stopped.read_failures == 0 &&
                excess.memory.occurrences[node_address(1024)] == 0,
            "A chain longer than the distinct-node limit read another entry");
  }
}

void bulk_head_failures_and_request_counts() {
  for (const auto count : {1u, 4u, 256u, kOwnedEntryBucketLimit}) {
    Fixture fixture;
    fixture.header(0, count);
    const auto result = fixture.run();
    require(result.complete && result.read_bytes == 32 + count * 16 && fixture.memory.reads.size() == 4,
            "Empty table batching changed observed bytes or retained per-bucket read calls");
    require(fixture.memory.reads[1] == std::pair<std::uint64_t, std::size_t>{Buckets, count * 8} &&
                fixture.memory.reads[2] == fixture.memory.reads[1],
            "Initial and repeated bucket blocks differ in address or exact extent");
  }
  // Every byte of the normal block, including empty heads after the owned IDs,
  // must participate in the final comparison rather than just its first word.
  for (unsigned byte = 0; byte < 32; ++byte) {
    Fixture fixture;
    fixture.normal();
    fixture.memory.change_on_repeat = Buckets;
    fixture.memory.change_byte = byte;
    require(!fixture.run().complete, "A changed byte inside the batched heads escaped the full reread");
  }
  for (const auto offset : {0u, 16384u, 32767u}) {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
      Fixture fixture;
      fixture.header(0, kOwnedEntryBucketLimit);
      if (scenario == 0)
        fixture.memory.fail_at = Buckets + offset;
      if (scenario == 1)
        fixture.memory.fail_on_repeat = Buckets + offset;
      if (scenario == 2) {
        fixture.memory.change_on_repeat = Buckets;
        fixture.memory.change_byte = offset;
      }
      if (scenario == 3)
        fixture.memory.bytes.erase(Buckets + offset);
      const auto result = fixture.run();
      require(!result.complete && result.read_failures == (scenario == 2 ? 0u : 1u),
              "A missing, failed or changed byte inside a maximum head block was accepted");
    }
  }
  std::printf("4096 empty buckets: 4 exact reader requests instead of 8194; both observe 65568 bytes including all rereads.\n");
}

}  // namespace

int main() {
  valid_membership();
  empty_and_invalid_headers();
  malformed_nodes();
  failed_and_changed_snapshots();
  maximum_bounds();
  bulk_head_failures_and_request_counts();
  std::printf("PASS: %u bounded owned-entry inventory checks. Synthetic memory only.\n", checks);
  return 0;
}
