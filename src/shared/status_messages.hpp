#pragma once

namespace taxi_camera::standalone {
// Status::message while an aircraft or display change cannot finish: the
// companion shows it in place of its own waiting-for-displays line.
inline constexpr char TransitionFailedMessage[] = "Aircraft change paused: camera lifecycle validation failed. Restart MSFS to retry.";
}  // namespace taxi_camera::standalone
