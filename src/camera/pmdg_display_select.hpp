#pragma once
#include <array>
#include <cmath>
#include <cstddef>

namespace taxi_camera::native_camera {
// PMDG 777 glareshield Display Select Panel, read through public L:vars only.
// L:GMC_L_INB, L:GMC_R_INB, L:GMC_LWR and L:GMC_DU stay 0 while the CAM page is
// shown, and the SDK data broadcast is off by default, so neither is used.
// Observed live on the 777-300ER on 2026-09-23 (read-only SimConnect):
// - Selector lamps switch_2311/2321/2331_a read about 1.003 when lit and 0
//   otherwise. Exactly one was lit at a time.
// - Momentary switch_NNN_a animations read 100 while pushed, for 0.1-0.25 s.
// - After CAM was removed from R INBD the lamp returned to LWR CTR by itself.
// Switch numbers match the 777-200ER and 777F cockpit behaviour files.
// The CAM page itself is PMDG state that Taxi Cam cannot read. This decoder
// follows the same button presses, so pages changed another way (for example
// by a display failure or reversion) can leave it out of step until CAM is
// pressed again.
// [0-2] selector lamps L INBD, R INBD, LWR CTR (display-side order); [3] CAM;
// [4-15] ENG STAT ELEC HYD FUEL AIR DOOR GEAR FCTL CHKL COMM NAV; [16-17] left
// (EVT_DSP_INDB_DSPL_L) and right INBD DSPL knobs.
inline constexpr std::array<const char*, 18> PmdgDspLvars{
    "L:switch_2311_a", "L:switch_2321_a", "L:switch_2331_a", "L:switch_243_a", "L:switch_234_a", "L:switch_235_a",
    "L:switch_236_a",  "L:switch_237_a",  "L:switch_238_a",  "L:switch_239_a", "L:switch_240_a", "L:switch_241_a",
    "L:switch_242_a",  "L:switch_244_a",  "L:switch_245_a",  "L:switch_246_a", "L:switch_315_a", "L:switch_290_a"};

class PmdgDisplaySelect {
 public:
  static constexpr std::size_t Values = PmdgDspLvars.size();
  static constexpr std::size_t Cam = 3, FirstPage = 4, LastPage = 15, LeftKnob = 16, RightKnob = 17;

  static bool valid(const std::array<double, Values>& v) noexcept {
    for (std::size_t i = 0; i < Values; ++i) {
      if (!std::isfinite(v[i]))
        return false;
      if (i < 3 && (v[i] < 0 || v[i] > 2))
        return false;
      if (i >= Cam && i <= LastPage && (v[i] < 0 || v[i] > 100))
        return false;
      if (i >= LeftKnob && std::abs(v[i]) > 1000)
        return false;
    }
    return true;
  }

  // Returns false and changes nothing for values outside the observed ranges.
  bool observe(const std::array<double, Values>& v) noexcept {
    if (!valid(v))
      return false;
    unsigned lamps = 0, lit = 0;
    for (unsigned i = 0; i < 3; ++i)
      if (v[i] > 0.5) {
        lamps |= 1u << i;
        ++lit;
      }
    if (!known_) {
      // The first sample only establishes switch positions. A CAM page left
      // on from before Taxi Cam connected needs another CAM press to show.
      known_ = true;
      previous_ = v;
      return true;
    }
    if (!lit) {
      // The panel is unpowered (or dark): PMDG shows no synoptic pages.
      mask_ = 0;
    } else if (lit == 1) {
      if (pushed(v, Cam))
        mask_ ^= lamps;
      for (auto i = FirstPage; i <= LastPage; ++i)
        if (pushed(v, i))
          mask_ &= ~lamps;
    }
    // More than one lamp is the lamp test: no single display is selected.
    if (v[LeftKnob] != previous_[LeftKnob])
      mask_ &= ~1u;
    if (v[RightKnob] != previous_[RightKnob])
      mask_ &= ~2u;
    previous_ = v;
    return true;
  }

  // Bit 0 L INBD, bit 1 R INBD, bit 2 LWR CTR.
  unsigned mask() const noexcept { return mask_; }
  void reset() noexcept { *this = {}; }

 private:
  bool pushed(const std::array<double, Values>& v, std::size_t i) const noexcept { return v[i] >= 50 && previous_[i] < 50; }

  std::array<double, Values> previous_{};
  bool known_ = false;
  unsigned mask_ = 0;
};
}  // namespace taxi_camera::native_camera
