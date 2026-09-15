#include "../../src/graphics/target_assignment.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace taxi_camera::standalone;
unsigned checks{};
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
void edits() {
  std::uint64_t left{}, right{}, request{};
  require(update_target_assignment(left, right, request, 0, 0) == TargetAssignmentResult::unchanged && request == 0,
          "Repeated automatic selection created a request");
  const std::array<std::array<std::uint64_t, 2>, 7> sequence{{{41, 0}, {41, 42}, {0, 42}, {0, 0}, {0, 41}, {42, 41}, {41, 42}}};
  std::uint64_t expected{};
  for (const auto& next : sequence) {
    require(update_target_assignment(left, right, request, next[0], next[1]) == TargetAssignmentResult::changed,
            "Manual, partial automatic, full automatic or swapped selection did not request assignment");
    require(left == next[0] && right == next[1] && request == ++expected, "Changed tuple was not published atomically once");
    for (unsigned repeat = 0; repeat < 3; ++repeat)
      require(update_target_assignment(left, right, request, next[0], next[1]) == TargetAssignmentResult::unchanged && request == expected,
              "Apply, Refresh or page change repeated an unchanged assignment");
    require(update_target_assignment(left, right, request, 99, 99) == TargetAssignmentResult::duplicate,
            "Duplicate nonzero textures were accepted");
    require(left == next[0] && right == next[1] && request == expected, "Rejected duplicate damaged the accepted tuple");
  }
  request = std::numeric_limits<std::uint64_t>::max();
  require(update_target_assignment(left, right, request, 0, 0) == TargetAssignmentResult::sequence_exhausted,
          "Request sequence wrapped through zero");
  require(left == 41 && right == 42 && request == std::numeric_limits<std::uint64_t>::max(), "Exhaustion mutated the accepted tuple");
  require(update_target_assignment(left, right, request, 41, 42) == TargetAssignmentResult::unchanged,
          "Unchanged selection failed at the sequence limit");
}
}  // namespace
int main() {
  try {
    edits();
    std::printf("PASS target assignment: %u manual, automatic, duplicate, repeat and sequence checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL target assignment: %s\n", error.what());
    return 1;
  }
}
