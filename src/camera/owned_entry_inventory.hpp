#pragma once

#include "memory_reader.hpp"

#include <array>

namespace taxi_camera::engine_camera {

inline constexpr std::uint32_t kOwnedEntryBucketLimit = 4096;
inline constexpr std::uint32_t kOwnedEntryNodeLimit = 1024;
// The owned views one inventory can verify: a whole camera set.
inline constexpr std::size_t kOwnedEntryViews = 4;
inline constexpr std::uint32_t kOwnedEntryReadLimit = 128 * 1024;

struct OwnedEntry {
  bool found = false;
  // Internal adapter data only. Never log these addresses or retain them past
  // the proven manager lifetime/update phase. No output is usable if incomplete.
  std::uint64_t address = 0;
  std::uint64_t key = 0;
  std::uint64_t payload_id = 0;
};

struct OwnedEntryInventory {
  // Only complete=true permits treating found=false as confirmed absence in
  // this snapshot. It does not establish an atomic snapshot or GPU retirement.
  bool complete = false;
  const char* error = "";
  std::uint32_t read_bytes = 0;  // Attempted bytes, including failed reads.
  std::uint32_t read_failures = 0;
  std::uint32_t entry_count = 0;
  std::uint32_t bucket_count = 0;
  std::uint32_t nodes_visited = 0;
  // Published only after full traversal and every consistency reread succeeds.
  std::array<OwnedEntry, kOwnedEntryViews> entries{};
};

// Build-specific fixed layout, no engine calls: T=manager+88 contains DWORD
// entry count+0, DWORD bucket count+4 and pointer+8 to untagged 8-byte heads.
// Every live E contributes key+0, payload ID+16 and tagged next+296 (mask bit0).
// No reuse list, ready byte, handles, strings or other payload fields are read.
// Zero input IDs are unused slots; duplicate nonzero IDs refuse before access.
//
// Full distinct-node traversal, count agreement and a reread of every observed
// header/head/node field are required. Malformed pointers, cycles, duplicate
// keys, owned-key/payload-ID mismatch, caps and changed/unreadable fields leave
// complete=false. Reader caps are not recovered engine allocation limits.
// The caller owns fresh build/code proof, manager generation and update-thread
// lifetime/synchronization. This function cannot freeze or acquire that state.
// Bucket heads are read as one exact contiguous field then fully reread. A
// MemoryReader must accept up to32768 bytes for that field. Fixed local storage
// is about96 KiB; it performs no dynamic allocation.
OwnedEntryInventory inspect_owned_entries(MemoryReader& reader,
                                          std::uint64_t manager_address,
                                          const std::array<std::uint64_t, kOwnedEntryViews>& owned_ids);

}  // namespace taxi_camera::engine_camera
