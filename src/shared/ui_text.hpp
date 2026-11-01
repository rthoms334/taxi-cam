#pragma once

namespace taxi_camera {
// Marks English interface wording that another module returns and the companion
// translates where it is shown. Logs and the bridge keep the English text.
constexpr const wchar_t* ui_text(const wchar_t* english) noexcept {
  return english;
}
}  // namespace taxi_camera
