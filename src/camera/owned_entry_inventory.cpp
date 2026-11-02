#include "owned_entry_inventory.hpp"

#include <limits>

namespace taxi_camera::engine_camera {
namespace {

std::uint32_t u32(const std::uint8_t* bytes) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i)
    value |= std::uint32_t(bytes[i]) << (8 * i);
  return value;
}

std::uint64_t u64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= std::uint64_t(bytes[i]) << (8 * i);
  return value;
}

bool aligned(std::uint64_t address) {
  return address != 0 && (address & 7) == 0;
}

struct Node {
  std::uint64_t address = 0;
  std::uint64_t key = 0;
  std::uint64_t payload_id = 0;
  std::uint64_t next = 0;  // Retain the tag for exact consistency comparisons.
};

class BoundedReader {
 public:
  BoundedReader(MemoryReader& reader, OwnedEntryInventory& result) : reader_(reader), result_(result) {}

  bool field(std::uint64_t address, std::uint64_t offset, void* destination, std::uint32_t size) {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (!aligned(address) || offset > maximum - address || size > maximum - (address + offset)) {
      result_.error = "An entry-table field has a null, misaligned or overflowing address.";
      return false;
    }
    if (size > kOwnedEntryReadLimit - result_.read_bytes) {
      result_.error = "The bounded entry-table read allowance was exhausted.";
      return false;
    }
    result_.read_bytes += size;
    if (!reader_.read(address + offset, destination, size)) {
      ++result_.read_failures;
      result_.error = "A required entry-table field could not be read exactly.";
      return false;
    }
    return true;
  }

  bool word(std::uint64_t address, std::uint64_t offset, std::uint64_t& value) {
    std::array<std::uint8_t, 8> bytes{};
    if (!field(address, offset, bytes.data(), bytes.size()))
      return false;
    value = u64(bytes.data());
    return true;
  }

 private:
  MemoryReader& reader_;
  OwnedEntryInventory& result_;
};

}  // namespace

OwnedEntryInventory inspect_owned_entries(MemoryReader& reader,
                                          std::uint64_t manager_address,
                                          const std::array<std::uint64_t, kOwnedEntryViews>& owned_ids) {
  OwnedEntryInventory result;
  for (std::size_t i = 0; i < owned_ids.size(); ++i) {
    if (!owned_ids[i])
      continue;
    for (std::size_t j = i + 1; j < owned_ids.size(); ++j) {
      if (owned_ids[j] == owned_ids[i]) {
        result.error = "Duplicate nonzero owned IDs are not a valid verification request.";
        return result;
      }
    }
  }
  BoundedReader source(reader, result);
  std::array<std::uint8_t, 16> header{};
  if (!source.field(manager_address, 88, header.data(), header.size()))
    return result;
  result.entry_count = u32(header.data());
  result.bucket_count = u32(header.data() + 4);
  const auto buckets = u64(header.data() + 8);
  if (result.entry_count > kOwnedEntryNodeLimit || result.bucket_count > kOwnedEntryBucketLimit) {
    result.error = "The entry or bucket count exceeds the bounded verification limits.";
    return result;
  }
  if (result.bucket_count == 0) {
    if (result.entry_count != 0 || buckets != 0) {
      result.error = "The empty entry-table header is inconsistent.";
      return result;
    }
  } else if (!aligned(buckets)) {
    result.error = "A nonempty bucket array has a null or misaligned address.";
    return result;
  }

  std::array<std::uint64_t, kOwnedEntryBucketLimit> heads{};
  std::array<Node, kOwnedEntryNodeLimit> nodes{};
  std::array<OwnedEntry, kOwnedEntryViews> found{};
  // This is one contiguous metadata field. Read all declared heads at once,
  // rather than issuing VirtualQuery/ReadProcessMemory for every eight bytes.
  // The same bytes and final full reread remain required.
  if (result.bucket_count != 0 && !source.field(buckets, 0, heads.data(), result.bucket_count * 8))
    return result;
  for (std::uint32_t bucket = 0; bucket < result.bucket_count; ++bucket) {
    auto address = heads[bucket];
    while (address != 0) {
      if (!aligned(address)) {
        result.error = "A live bucket head or masked entry link is misaligned.";
        return result;
      }
      if (result.nodes_visited == kOwnedEntryNodeLimit) {
        result.error = "The bounded distinct-entry limit was exhausted.";
        return result;
      }
      for (std::uint32_t previous = 0; previous < result.nodes_visited; ++previous) {
        if (nodes[previous].address == address) {
          result.error = "A cycle or repeated live entry crosses the bucket traversal.";
          return result;
        }
      }
      Node node;
      node.address = address;
      if (!source.word(address, 0, node.key) || !source.word(address, 16, node.payload_id) || !source.word(address, 296, node.next))
        return result;
      for (std::uint32_t previous = 0; previous < result.nodes_visited; ++previous) {
        if (nodes[previous].key == node.key) {
          result.error = "Duplicate live keys make the entry table inconsistent.";
          return result;
        }
      }
      for (std::size_t index = 0; index < owned_ids.size(); ++index) {
        if (owned_ids[index] != 0 && node.key == owned_ids[index]) {
          if (node.payload_id != owned_ids[index]) {
            result.error = "An owned lookup key disagrees with its entry payload ID.";
            return result;
          }
          found[index] = {true, address, node.key, node.payload_id};
        }
      }
      nodes[result.nodes_visited++] = node;
      if (result.nodes_visited > result.entry_count) {
        result.error = "More live entries were observed than the table declares.";
        return result;
      }
      address = node.next & ~std::uint64_t{1};
    }
  }
  if (result.nodes_visited != result.entry_count) {
    result.error = "The complete bucket traversal disagrees with the declared entry count.";
    return result;
  }

  // Reread every observed word, including unowned nodes and all empty heads.
  // This detects observed changes but cannot make concurrent memory atomic.
  for (std::uint32_t index = 0; index < result.nodes_visited; ++index) {
    const auto& node = nodes[index];
    Node current;
    if (!source.word(node.address, 0, current.key) || !source.word(node.address, 16, current.payload_id) ||
        !source.word(node.address, 296, current.next))
      return result;
    if (current.key != node.key || current.payload_id != node.payload_id || current.next != node.next) {
      result.error = "An entry key, payload ID or tagged link changed during verification.";
      return result;
    }
  }
  std::array<std::uint64_t, kOwnedEntryBucketLimit> current_heads{};
  if (result.bucket_count != 0 && !source.field(buckets, 0, current_heads.data(), result.bucket_count * 8))
    return result;
  for (std::uint32_t bucket = 0; bucket < result.bucket_count; ++bucket) {
    if (current_heads[bucket] != heads[bucket]) {
      result.error = "A bucket head changed during verification.";
      return result;
    }
  }
  std::array<std::uint8_t, 16> current_header{};
  if (!source.field(manager_address, 88, current_header.data(), current_header.size()))
    return result;
  if (current_header != header) {
    result.error = "The manager's entry-table header changed during verification.";
    return result;
  }
  result.entries = found;
  result.complete = true;
  return result;
}

}  // namespace taxi_camera::engine_camera
