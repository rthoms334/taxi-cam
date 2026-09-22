#include "../../src/app/overview_status.hpp"

#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace {
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}
bool contains(const std::wstring& text, const wchar_t* needle) {
  return text.find(needle) != std::wstring::npos;
}
}  // namespace

int main() {
  using taxi_camera::standalone::overview_status_line;
  // Issue 57: the bridge held "Waiting for pending camera startup to finish
  // before changing aircraft profile." while its transition status carried no
  // display candidates. The Overview replaced that line with the static
  // display hint, so the report described a display problem.
  const std::wstring hold = L"Waiting for pending camera startup to finish before changing aircraft profile.";
  auto line = overview_status_line(true, false, true, hold, L"Connecting");
  require(line.text == hold, "A live bridge status line survives an empty display inventory");
  require(line.highlight, "An empty inventory still highlights the live line");

  const std::wstring refused =
      L"There is no support for this sim version. Send a report. Instruction discovery refused: template_not_found";
  line = overview_status_line(true, false, true, refused, L"");
  require(line.text == refused, "A refused native start is not hidden behind the display hint");

  const std::wstring loading = L"Waiting for the flight to finish loading and fresh camera telemetry.";
  line = overview_status_line(true, false, true, loading, L"");
  require(line.text == loading, "A loading wait is reported as a loading wait");

  const std::wstring displays =
      L"Waiting for cockpit displays to be drawn. They are learned on first use; Restart Flight if the list stays empty.";
  line = overview_status_line(true, false, true, displays, L"");
  require(line.text == displays && line.highlight, "The bridge's own display wait is shown when displays are the blocker");

  line = overview_status_line(true, false, false, L"", L"Connecting to the simulator");
  require(contains(line.text, L"Waiting for cockpit displays") && line.highlight,
          "Without a bridge heartbeat the late-connect hint remains the fallback");

  line = overview_status_line(true, false, true, L"", L"Connecting to the simulator");
  require(contains(line.text, L"Waiting for cockpit displays"), "A heartbeat with an empty message falls back to the display hint");

  line = overview_status_line(true, true, true, L"Ready. Use the aircraft's left or right TAXI button.", L"");
  require(contains(line.text, L"Ready.") && !line.highlight, "Assigned displays show the plain bridge line");

  line = overview_status_line(false, false, false, L"", L"Waiting for FlightSimulator2024.exe");
  require(line.text == L"Waiting for FlightSimulator2024.exe" && !line.highlight,
          "Before graphics are ready the companion connection state is shown");

  line = overview_status_line(false, false, true, L"Disconnected. Use Connect in the Windows companion.", L"idle");
  require(contains(line.text, L"Disconnected"), "A bridge line is preferred over the connection state whenever it exists");

  std::printf("Overview status: PASS %u checks\n", checks);
  return 0;
}
