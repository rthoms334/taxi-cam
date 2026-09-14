#pragma once
#include <cstdint>
#include <limits>

namespace taxi_camera::standalone {
enum class TargetAssignmentResult { unchanged, changed, duplicate, sequence_exhausted };

// Zero requests automatic assignment for that side. Validate the whole tuple
// before changing either ID or its session-only request sequence.
inline TargetAssignmentResult update_target_assignment(std::uint64_t& left,
                                                       std::uint64_t& right,
                                                       std::uint64_t& request,
                                                       std::uint64_t next_left,
                                                       std::uint64_t next_right) noexcept {
  if (next_left && next_left == next_right)
    return TargetAssignmentResult::duplicate;
  if (left == next_left && right == next_right)
    return TargetAssignmentResult::unchanged;
  if (request == std::numeric_limits<std::uint64_t>::max())
    return TargetAssignmentResult::sequence_exhausted;
  left = next_left;
  right = next_right;
  ++request;
  return TargetAssignmentResult::changed;
}
}  // namespace taxi_camera::standalone
