#pragma once

#include <string>
#include <utility>

namespace taxi_camera::standalone {

struct OverviewStatusLine {
  std::wstring text;
  bool highlight = false;
};

// The bridge publishes one status line per tick. That line already reports an
// empty display inventory ("Waiting for cockpit displays to be drawn...") when
// missing displays are the blocking condition, and it is the only place where
// aircraft-transition holds, refused camera starts and loading waits are
// visible. An empty candidate list therefore never replaces a live bridge
// line; the static late-connect hint is only a fallback for a bridge that has
// not published a message yet.
inline OverviewStatusLine overview_status_line(bool graphics_ready,
                                               bool has_displays,
                                               bool heartbeat,
                                               std::wstring bridge_message,
                                               std::wstring live_connection) {
  const bool empty_displays = graphics_ready && !has_displays;
  if (heartbeat && !bridge_message.empty())
    return {std::move(bridge_message), empty_displays};
  if (empty_displays)
    return {L"Waiting for cockpit displays to be drawn. Restart Flight only if the list stays empty.", true};
  return {std::move(live_connection), false};
}

}  // namespace taxi_camera::standalone
