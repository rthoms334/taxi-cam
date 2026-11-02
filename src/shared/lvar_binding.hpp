#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <type_traits>

namespace taxi_camera::standalone {
// Cockpit L:vars the user binds to camera actions, saved per aircraft profile.
// Taxi Cam only reads them. A momentary binding works like its action's
// shortcut on every press (zero to non-zero); a latched binding turns the
// action's displays on when it becomes non-zero and off when it returns to zero.
inline constexpr std::size_t LvarNameSize = 64;
// One binding per CameraAction: left, right, both, lower, door camera.
inline constexpr std::size_t LvarBindingCount = 5;
enum class LvarTrigger : std::uint32_t { momentary, latched };
struct LvarBinding {
  char name[LvarNameSize]{};  // Without the "L:" prefix; empty when the action is not bound.
  std::uint32_t trigger{};    // LvarTrigger
};
using LvarBindings = std::array<LvarBinding, LvarBindingCount>;
static_assert(std::has_unique_object_representations_v<LvarBinding>);

// Letters, digits, underscores and dots, 1-63 characters. Nothing that could
// carry a unit, an index or a second variable into the SimConnect definition.
inline bool valid_lvar_name(const char* name) noexcept {
  const auto length = strnlen(name, LvarNameSize);
  if (!length || length >= LvarNameSize)
    return false;
  for (std::size_t i = 0; i < length; ++i) {
    const char c = name[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.'))
      return false;
  }
  return true;
}
inline bool valid_lvar_bindings(const LvarBindings& bindings) noexcept {
  for (const auto& binding : bindings) {
    if (!std::memchr(binding.name, 0, LvarNameSize) || (binding.name[0] && !valid_lvar_name(binding.name)))
      return false;
    if (binding.trigger > static_cast<std::uint32_t>(LvarTrigger::latched))
      return false;
  }
  return true;
}
// What the user typed: surrounding spaces and an "L:" prefix are dropped. An
// empty field unbinds the action. False when the rest is not a valid name.
inline bool parse_lvar_name(const wchar_t* text, char (&out)[LvarNameSize]) noexcept {
  std::memset(out, 0, sizeof(out));
  const wchar_t* begin = text;
  while (*begin == L' ' || *begin == L'\t')
    ++begin;
  const wchar_t* end = begin + std::wcslen(begin);
  while (end > begin && (end[-1] == L' ' || end[-1] == L'\t'))
    --end;
  if (end - begin >= 2 && (begin[0] == L'L' || begin[0] == L'l') && begin[1] == L':')
    begin += 2;
  if (begin == end)
    return true;
  if (end - begin >= static_cast<std::ptrdiff_t>(LvarNameSize))
    return false;
  for (std::size_t i = 0; begin + i < end; ++i)
    out[i] = begin[i] > 0x7f ? '?' : static_cast<char>(begin[i]);
  if (valid_lvar_name(out))
    return true;
  std::memset(out, 0, sizeof(out));
  return false;
}
// The value's on/off reading, for both trigger kinds. Lamps and animations
// that read 1.003 or 100 when on count as on.
inline bool lvar_on(double value) noexcept {
  return std::isfinite(value) && std::abs(value) >= 0.5;
}

// One binding as the bridge reads it (Status, protocol 27). The counters run
// for one stream (Status::lvar_generation); a new stream starts them again.
struct LvarBindingState {
  float value{};
  std::uint32_t rises{}, falls{};  // Changes to on and to off.
  std::uint32_t read{};            // 1 once this stream has a value.
};
using LvarBindingStates = std::array<LvarBindingState, LvarBindingCount>;

enum class LvarCommandKind { toggle, on, off };
struct LvarCommand {
  unsigned action{};
  LvarCommandKind kind{};
};
// Companion side: turns the bridge's counters into camera commands. The first
// reading of a binding, of a stream and after a name change only records the
// counters, so a switch already on when Taxi Cam connects does nothing.
class LvarBindingTracker {
 public:
  // Writes at most one command per binding to out and returns their number.
  std::size_t update(const LvarBindings& bindings,
                     std::uint32_t generation,
                     const LvarBindingStates& states,
                     std::array<LvarCommand, LvarBindingCount>& out) noexcept {
    if (generation != generation_) {
      generation_ = generation;
      known_ = {};
    }
    std::size_t count = 0;
    for (std::size_t i = 0; i < LvarBindingCount; ++i) {
      const auto& binding = bindings[i];
      const auto& state = states[i];
      if (std::memcmp(binding.name, names_[i].data(), LvarNameSize) != 0) {
        std::memcpy(names_[i].data(), binding.name, LvarNameSize);
        known_[i] = false;
      }
      if (!generation || !binding.name[0] || !state.read) {
        known_[i] = false;
        continue;
      }
      if (!known_[i]) {
        known_[i] = true;
        rises_[i] = state.rises;
        falls_[i] = state.falls;
        continue;
      }
      const std::uint32_t rises = state.rises - rises_[i], falls = state.falls - falls_[i];
      rises_[i] = state.rises;
      falls_[i] = state.falls;
      if (!rises && !falls)
        continue;
      if (binding.trigger == static_cast<std::uint32_t>(LvarTrigger::momentary)) {
        // Two presses between readings turn the displays on and off again.
        if (rises % 2)
          out[count++] = {static_cast<unsigned>(i), LvarCommandKind::toggle};
      } else {
        out[count++] = {static_cast<unsigned>(i), lvar_on(state.value) ? LvarCommandKind::on : LvarCommandKind::off};
      }
    }
    return count;
  }

 private:
  std::uint32_t generation_{};
  std::array<std::array<char, LvarNameSize>, LvarBindingCount> names_{};
  std::array<bool, LvarBindingCount> known_{};
  std::array<std::uint32_t, LvarBindingCount> rises_{}, falls_{};
};
}  // namespace taxi_camera::standalone
