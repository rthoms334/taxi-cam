#include "../../src/graphics/query_scope.hpp"

#include <d3d12.h>
#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using taxi_camera::standalone::QueryScope;
static_assert(D3D12_QUERY_TYPE_OCCLUSION == 0 && D3D12_QUERY_TYPE_BINARY_OCCLUSION == 1 && D3D12_QUERY_TYPE_TIMESTAMP == 2 &&
              D3D12_QUERY_TYPE_PIPELINE_STATISTICS == 3 && D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 == 4 &&
              D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1 == 5 && D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2 == 6 &&
              D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3 == 7 && D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS == 8 &&
              D3D12_QUERY_TYPE_PIPELINE_STATISTICS1 == 10);
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
void blocked(const QueryScope& scope) {
  require(!scope.can_stamp() && !scope.known_empty(), "Unknown/open scope allowed stamping");
}
void reset_and_unknown() {
  QueryScope scope;
  blocked(scope);
  require(!scope.known(), "Late registration assumed an empty recording");
  require(!scope.begin(1, 0, 0) && !scope.end(1, 2, 0), "An event repaired unknown history");
  scope.clear_state();
  blocked(scope);
  scope.reset(true);
  require(scope.known() && scope.known_empty() && scope.active_count() == 0, "Successful Reset did not establish empty history");
  scope.clear_state();
  require(scope.can_stamp(), "ClearState changed empty history");
  require(scope.begin(1, 0, 0), "Valid Begin rejected");
  scope.reset(false);
  blocked(scope);
  require(!scope.end(1, 0, 0), "Failed Reset let an old End repair history");
  scope.reset(true);
  require(scope.can_stamp(), "Reset did not recover failed Reset");
  scope.invalidate();
  blocked(scope);
  scope.clear_state();
  require(!scope.known(), "ClearState repaired an observer gap");
  scope.reset(true);
  require(scope.can_stamp(), "Reset did not recover observer gap");
}
void paired_types_and_order() {
  constexpr std::array<std::uint32_t, 8> types{0, 1, 3, 4, 5, 6, 7, 10};
  for (const auto type : types) {
    QueryScope scope;
    scope.reset(true);
    require(scope.begin(0x123456789ABCDEF0ull, type, 0xFFFFFFFFu), "Public paired type or full-width key rejected");
    blocked(scope);  // This remains true until the caller forwards native End.
    scope.clear_state();
    require(scope.known() && scope.active_count() == 1, "ClearState forgot an open query");
    blocked(scope);
    require(scope.end(0x123456789ABCDEF0ull, type, 0xFFFFFFFFu), "Matching full-width End rejected");
    require(scope.can_stamp(), "Last End did not release admission");
    require(scope.begin(1, type, 7) && scope.end(1, type, 7), "Sequential slot reuse rejected");
  }
  QueryScope scope;
  scope.reset(true);
  require(scope.begin(11, 0, 1) && scope.begin(12, 0, 1) && scope.begin(11, 3, 2), "Overlapping independent queries rejected");
  require(scope.end(12, 0, 1) && scope.active_count() == 2, "Out-of-order End lost another heap query");
  blocked(scope);
  require(scope.end(11, 0, 1) && scope.active_count() == 1, "Second End lost remaining query");
  blocked(scope);
  require(scope.end(11, 3, 2) && scope.can_stamp(), "Final overlapping End failed");
}
void timestamps() {
  QueryScope scope;
  scope.reset(true);
  require(scope.end(1, 2, 3) && scope.end(1, 2, 3) && scope.can_stamp(), "Timestamp End opened a scope");
  require(scope.begin(2, 3, 1), "Pipeline Begin failed");
  require(scope.end(1, 2, 3) && scope.active_count() == 1, "Timestamp End changed a paired scope");
  blocked(scope);
  require(scope.end(2, 3, 1) && scope.can_stamp(), "Timestamp interfered with paired End");
  require(!scope.begin(1, 2, 3), "Timestamp Begin accepted");
  require(!scope.end(1, 2, 3), "Timestamp repaired poisoned history");
  blocked(scope);
}
void malformed() {
  for (const auto type : {8u, 9u, 11u, std::numeric_limits<std::uint32_t>::max()}) {
    for (const bool begin : {false, true}) {
      QueryScope scope;
      scope.reset(true);
      require(!(begin ? scope.begin(1, type, 0) : scope.end(1, type, 0)), "Invalid graphics query type accepted");
      blocked(scope);
      require(!scope.known(), "Invalid type did not poison");
    }
  }
  for (const auto type : {0u, 2u}) {
    QueryScope scope;
    scope.reset(true);
    require(!scope.end(0, type, 0), "Null End heap accepted");
    blocked(scope);
    scope.reset(true);
    require(!scope.begin(0, type, 0), "Null Begin heap accepted");
    blocked(scope);
  }
  QueryScope scope;
  scope.reset(true);
  require(scope.begin(1, 0, 3) && !scope.begin(1, 0, 3), "Duplicate Begin accepted");
  require(!scope.end(1, 0, 3), "Matching End repaired duplicate Begin");
  blocked(scope);
  // Begin owns a heap element, regardless of query type. Both occlusion
  // variants share a compatible heap, as do the stream-output query variants.
  for (const auto types : {std::array<std::uint32_t, 2>{0, 1}, {1, 0}, {4, 5}}) {
    scope.reset(true);
    require(scope.begin(1, types[0], 3), "Initial cross-type Begin rejected");
    require(!scope.begin(1, types[1], 3), "Different type reopened an already active heap element");
    require(!scope.known() && !scope.end(1, types[0], 3), "Exact End repaired cross-type duplicate Begin");
    blocked(scope);
    scope.reset(true);
    require(scope.begin(1, types[0], 3) && scope.end(1, types[0], 3) && scope.begin(1, types[1], 3) && scope.end(1, types[1], 3) &&
                scope.can_stamp(),
            "Legal sequential cross-type reuse rejected");
  }
  for (unsigned mismatch = 0; mismatch != 3; ++mismatch) {
    scope.reset(true);
    require(scope.begin(1, 0, 3), "Begin after Reset failed");
    require(!scope.end(mismatch == 0 ? 2 : 1, mismatch == 1 ? 1 : 0, mismatch == 2 ? 4 : 3), "Mismatched End accepted");
    blocked(scope);
    require(!scope.end(1, 0, 3), "Later exact End repaired unmatched End");
  }
  scope.reset(true);
  require(!scope.end(1, 0, 3), "End without any Begin accepted");
  blocked(scope);
}
void capacity_and_retirement() {
  QueryScope scope;
  scope.reset(true);
  for (std::size_t i = 0; i != QueryScope::capacity; ++i)
    require(scope.begin(1, 0, static_cast<std::uint32_t>(i)), "Capacity prematurely exhausted");
  require(scope.active_count() == 64, "Incorrect full capacity");
  require(!scope.begin(2, 0, 0), "Overflow accepted");
  blocked(scope);
  require(!scope.end(1, 0, 0), "End recovered overflow");
  scope.reset(true);
  for (std::size_t i = 0; i != QueryScope::capacity; ++i)
    require(scope.begin(1, 0, static_cast<std::uint32_t>(i)), "Reset did not clear capacity");
  for (std::size_t i = 0; i != QueryScope::capacity; ++i) {
    require(scope.end(1, 0, static_cast<std::uint32_t>((i * 17) % 64)), "Non-LIFO End corrupted bounded set");
    require(scope.can_stamp() == (i == 63), "Admission released before all queries ended");
  }
  scope.retire(1);
  require(scope.can_stamp(), "Retirement of inactive heap blocked empty history");
  require(scope.begin(1, 0, 7), "Begin failed before retirement");
  scope.retire(2);
  require(scope.known() && scope.active_count() == 1, "Unrelated retirement changed active scope");
  scope.retire(1);
  require(!scope.known() && !scope.end(1, 0, 7), "Replacement heap at retired address closed an old query");
  scope.reset(true);
  require(scope.begin(1, 0, 7) && scope.end(1, 0, 7) && scope.can_stamp(), "Fresh recording could not reuse retired address");
}
}  // namespace
int main() {
  try {
    reset_and_unknown();
    paired_types_and_order();
    timestamps();
    malformed();
    capacity_and_retirement();
    std::printf("{\"passed\":true,\"checks\":%u,\"capacity\":64}\n", checks);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "query-scope failure after %u checks: %s\n", checks, e.what());
    return 1;
  }
}
