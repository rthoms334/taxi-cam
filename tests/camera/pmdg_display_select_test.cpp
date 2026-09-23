#include "../../src/camera/pmdg_display_select.hpp"
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using taxi_camera::native_camera::PmdgDisplaySelect;
using Values = std::array<double, PmdgDisplaySelect::Values>;
unsigned checks{};
void require(bool value, const char* text) {
  ++checks;
  if (!value)
    throw std::runtime_error(text);
}
constexpr double Lit = 1.00275;  // Observed selector lamp value.
// lamp: 0 L INBD, 1 R INBD, 2 LWR CTR, 3 none. Knobs default to position 0.
Values panel(unsigned lamp, double cam = 0, std::size_t page = 0, double left_knob = 0, double right_knob = 0) {
  Values v{};
  if (lamp < 3)
    v[lamp] = Lit;
  v[PmdgDisplaySelect::Cam] = cam;
  if (page)
    v[page] = 100;
  v[PmdgDisplaySelect::LeftKnob] = left_knob;
  v[PmdgDisplaySelect::RightKnob] = right_knob;
  return v;
}
void press(PmdgDisplaySelect& dsp, unsigned lamp) {
  require(dsp.observe(panel(lamp, 100)), "CAM push sample rejected");
  require(dsp.observe(panel(lamp, 0)), "CAM release sample rejected");
}

void live_capture_sequence() {
  // Read-only capture of the 777-300ER on 2026-09-23 (seconds after start):
  // CAM on L INBD at 74.92 and 86.20, LWR CTR at 87.06, CAM at 88.09 and
  // 91.12, R INBD at 92.52, CAM at 93.69 and 95.09, lamp back to LWR at 97.33.
  PmdgDisplaySelect dsp;
  require(dsp.observe(panel(0)) && dsp.mask() == 0, "The first sample invented a CAM page");
  press(dsp, 0);
  require(dsp.mask() == 1, "CAM with L INBD lit did not select the left display");
  press(dsp, 0);
  require(dsp.mask() == 0, "Second CAM with L INBD lit did not remove it");
  require(dsp.observe(panel(2)) && dsp.mask() == 0, "Moving the selector changed a page");
  press(dsp, 2);
  require(dsp.mask() == 4, "CAM with LWR CTR lit did not select the lower display");
  press(dsp, 2);
  require(dsp.mask() == 0, "Second CAM with LWR CTR lit did not remove it");
  require(dsp.observe(panel(1)), "R INBD selection rejected");
  press(dsp, 1);
  require(dsp.mask() == 2, "CAM with R INBD lit did not select the right display");
  press(dsp, 1);
  require(dsp.mask() == 0, "Second CAM with R INBD lit did not remove it");
  require(dsp.observe(panel(2)) && dsp.mask() == 0, "Automatic return to LWR CTR changed a page");
}

void independent_displays() {
  PmdgDisplaySelect dsp;
  dsp.observe(panel(0));
  press(dsp, 0);
  dsp.observe(panel(2));
  press(dsp, 2);
  dsp.observe(panel(1));
  press(dsp, 1);
  require(dsp.mask() == 7, "Displays are not independent");
  // A held button is one push, whether sampled once or on every frame.
  for (int i = 0; i < 5; ++i)
    dsp.observe(panel(1, 100));
  dsp.observe(panel(1, 0));
  require(dsp.mask() == 5, "A held CAM push toggled more than once");
  // The polled and streamed requests can deliver the same sample twice.
  dsp.observe(panel(1, 0));
  require(dsp.mask() == 5, "A repeated released sample toggled");
}

void other_pages_and_knobs() {
  PmdgDisplaySelect dsp;
  dsp.observe(panel(0));
  press(dsp, 0);
  dsp.observe(panel(2));
  press(dsp, 2);
  dsp.observe(panel(1));
  press(dsp, 1);
  require(dsp.mask() == 7, "Setup failed");
  // ENG (first page) on LWR CTR replaces only that display's CAM page.
  dsp.observe(panel(2));
  dsp.observe(panel(2, 0, PmdgDisplaySelect::FirstPage));
  dsp.observe(panel(2));
  require(dsp.mask() == 3, "Another synoptic did not replace the lower CAM page");
  // NAV (last page) on L INBD.
  dsp.observe(panel(0));
  dsp.observe(panel(0, 0, PmdgDisplaySelect::LastPage));
  dsp.observe(panel(0));
  require(dsp.mask() == 2, "Another synoptic did not replace the left CAM page");
  // The right inboard knob changes that display's format.
  dsp.observe(panel(0, 0, 0, 0, 10));
  require(dsp.mask() == 0, "The right INBD DSPL knob left the CAM page on");
  dsp.observe(panel(0));
  press(dsp, 0);
  dsp.observe(panel(0, 0, 0, 10, 10));
  require(dsp.mask() == 0, "The left INBD DSPL knob left the CAM page on");
}

void power_and_lamp_test() {
  PmdgDisplaySelect dsp;
  dsp.observe(panel(0));
  press(dsp, 0);
  require(dsp.observe(panel(3)) && dsp.mask() == 0, "An unpowered panel kept a CAM page");
  require(!dsp.observe(panel(3, 100)) || dsp.mask() == 0, "CAM without a selected display did something");
  dsp.observe(panel(3));
  Values test = panel(0);
  test[1] = test[2] = Lit;
  require(dsp.observe(test), "Lamp test sample rejected");
  test[PmdgDisplaySelect::Cam] = 100;
  dsp.observe(test);
  require(dsp.mask() == 0, "CAM during the lamp test selected a display");
}

void first_sample_and_invalid_values() {
  PmdgDisplaySelect dsp;
  // A CAM push already held when Taxi Cam connects is not a new push.
  require(dsp.observe(panel(0, 100)) && dsp.mask() == 0, "A push held before connection toggled");
  dsp.observe(panel(0));
  press(dsp, 0);
  require(dsp.mask() == 1, "Setup failed");
  auto bad = panel(0);
  bad[PmdgDisplaySelect::Cam] = std::numeric_limits<double>::quiet_NaN();
  require(!dsp.observe(bad) && dsp.mask() == 1, "A NaN sample changed state");
  bad = panel(0);
  bad[PmdgDisplaySelect::Cam] = 150;
  require(!dsp.observe(bad) && dsp.mask() == 1, "An out-of-range switch changed state");
  bad = panel(0);
  bad[0] = -1;
  require(!dsp.observe(bad) && dsp.mask() == 1, "A negative lamp changed state");
  bad = panel(0);
  bad[PmdgDisplaySelect::LeftKnob] = 1e9;
  require(!dsp.observe(bad) && dsp.mask() == 1, "An out-of-range knob changed state");
  dsp.reset();
  require(dsp.mask() == 0, "Reset kept a CAM page");
}

void lvar_names() {
  using taxi_camera::native_camera::PmdgDspLvars;
  require(std::string(PmdgDspLvars[0]) == "L:switch_2311_a" && std::string(PmdgDspLvars[1]) == "L:switch_2321_a" &&
              std::string(PmdgDspLvars[2]) == "L:switch_2331_a",
          "Selector lamps are not in display-slot order");
  require(std::string(PmdgDspLvars[PmdgDisplaySelect::Cam]) == "L:switch_243_a", "CAM is not switch 243");
  for (std::size_t i = PmdgDisplaySelect::FirstPage; i <= PmdgDisplaySelect::LastPage; ++i)
    require(std::string(PmdgDspLvars[i]) != "L:switch_243_a" && std::string(PmdgDspLvars[i]) != "L:switch_247_a",
            "CAM or CANC/RCL is listed as a page replacement");
  require(std::string(PmdgDspLvars[PmdgDisplaySelect::LeftKnob]) == "L:switch_315_a" &&
              std::string(PmdgDspLvars[PmdgDisplaySelect::RightKnob]) == "L:switch_290_a",
          "Inboard knobs are swapped");
}
}  // namespace

int main() {
  try {
    live_capture_sequence();
    independent_displays();
    other_pages_and_knobs();
    power_and_lamp_test();
    first_sample_and_invalid_values();
    lvar_names();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "PMDG display select: FAIL: %s\n", error.what());
    return 1;
  }
  std::printf("PMDG display select: PASS (%u checks)\n", checks);
  return 0;
}
