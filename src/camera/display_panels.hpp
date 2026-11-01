#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace taxi_camera::display_identity {
// The panel.cfg reader creates one 0x690-byte panel per [VCockpitNN]/
// [VPaintingNN] section and keeps up to 64 in a static table with the count at
// +0x200. The table's address is resolved per process from its release
// function (panel_registry_contract.hpp), not tied to a simulator build.
inline constexpr std::uint32_t kPanelCountOffset = 0x200, kMaxPanels = 64;
// Panel fields, decoded on Store 1.8.16.0 and unchanged on the 2026-10-31
// Store build: canvas object (i16 format, i32 width at +4, height at +8),
// inline section name, texture= name (small string: pointer when the u32 at
// +0xC is nonzero, else inline), index and kind (4 VCockpit, 8 VPainting).
// Every read checks the section name, so a layout change refuses the table.
inline constexpr std::uint32_t kCanvasOffset = 0x18, kSectionOffset = 0x88, kTextureOffset = 0x528, kTextureHeapFlag = 0x534,
                               kIndexOffset = 0x598, kKindOffset = 0x680;
inline constexpr std::uint32_t kKindVCockpit = 4;

struct Panel {
  std::uint32_t index{}, kind{}, canvas_width{}, canvas_height{};
  bool canvas{};
  std::array<char, 48> section{}, texture{};
};
struct Panels {
  const char* error = "not_read";  // nullptr when read
  std::uint32_t timestamp{}, image_size{}, count{};
  std::uint64_t signature{};  // Changes when the registry's panels change.
  std::array<Panel, kMaxPanels> panels{};
};
// Copies printable ASCII up to the first NUL into a terminated buffer.
template <std::size_t N>
inline void copy_name(const unsigned char* bytes, std::size_t size, std::array<char, N>& out) noexcept {
  std::size_t n = 0;
  for (; n < size && n + 1 < N && bytes[n] >= 32 && bytes[n] <= 126; ++n)
    out[n] = static_cast<char>(bytes[n]);
  out[n] = 0;
}
// The section names the reader creates panels for. Any other text at the
// section offset means the panel layout moved.
inline bool section_name(const char* name) noexcept {
  return std::strncmp(name, "VCockpit", 8) == 0 || std::strncmp(name, "VPainting", 9) == 0;
}
// A panel the simulator draws into a texture of its own: a VCockpit panel
// with a canvas and a real texture= name. NO_TEXTURE panels get none.
inline bool textured(const Panel& p) noexcept {
  return p.kind == kKindVCockpit && p.canvas && p.texture[0] && std::strcmp(p.texture.data(), "NO_TEXTURE") != 0;
}
struct Creation {
  std::uint64_t id{}, tick{};
  std::uint32_t width{}, height{}, mips{}, format{};
  std::uint64_t native{};  // Resource address at creation (RenderTargetSequence::Entry).
};
struct Assignment {
  const Panel* panel{};
  Creation creation{};
};
// The NN of a panel's [VCockpitNN] / [VPaintingNN] section, or -1.
inline int section_number(const Panel& p) noexcept {
  const char* digits = std::strncmp(p.section.data(), "VCockpit", 8) == 0    ? p.section.data() + 8
                       : std::strncmp(p.section.data(), "VPainting", 9) == 0 ? p.section.data() + 9
                                                                             : nullptr;
  if (!digits || !*digits)
    return -1;
  int n = 0;
  for (; *digits; ++digits) {
    if (*digits < '0' || *digits > '9' || n > 9999)
      return -1;
    n = n * 10 + (*digits - '0');
  }
  return n;
}
// The order panel textures are taken to be created in, last first: the table
// index (every catalog aircraft logged), or the panel.cfg section with the
// index within it, for a table listing a section's panels apart (the Fenix
// A320 put one VCockpit17 panel first and the other last on 2026-11-01, its
// textures still created with the section's).
enum class PanelOrder { index, section };
// Proposed rule from two PMDG 777 sessions: panel textures are created in
// reverse panel-index order, and they are the multi-mip creations of the
// cockpit-load burst. Pairs the k-th such creation with the k-th textured
// panel counted from the highest index (or section, then index). complete is
// false when the burst has fewer candidates than textured panels.
inline std::size_t propose(const Panels& panels,
                           const Creation* burst,
                           std::size_t count,
                           std::array<Assignment, kMaxPanels>& out,
                           bool& complete,
                           PanelOrder by = PanelOrder::index) noexcept {
  std::array<const Panel*, kMaxPanels> order{};
  std::size_t textured_count = 0;
  for (std::uint32_t i = 0; i < panels.count && i < kMaxPanels; ++i)
    if (textured(panels.panels[i]))
      order[textured_count++] = &panels.panels[i];
  const auto before = [by](const Panel* a, const Panel* b) {
    if (by == PanelOrder::section && section_number(*a) != section_number(*b))
      return section_number(*a) > section_number(*b);
    return a->index > b->index;
  };
  // Highest first; insertion sort keeps this dependency-free.
  for (std::size_t i = 1; i < textured_count; ++i)
    for (std::size_t j = i; j > 0 && before(order[j], order[j - 1]); --j) {
      const auto* t = order[j - 1];
      order[j - 1] = order[j];
      order[j] = t;
    }
  std::size_t n = 0;
  for (std::size_t c = 0; c < count && n < textured_count; ++c)
    if (burst[c].mips > 1)
      out[n] = {order[n], burst[c]}, ++n;
  complete = textured_count > 0 && n == textured_count;
  return n;
}
// Resolves panel names to creations. Extra multi-mip textures created after
// the named panels' (aircraft switched in flight: the iniBuilds A350 made two
// more 2048 x 2048 textures, the FlyByWire A380 a 1646 x 1024 one and four
// copies) move every pairing back by the same count. The window of the last
// textured-panel-count creations therefore moves back one creation at a time,
// up to max_shift, until every non-empty name lands on a creation fit()
// accepts (the profile's display shape) and every panel expected() gives a
// shape for lands on that shape. A repeated name is checked on its lowest
// index, the texture created last. Returns the shift used with ids and out
// filled, or -1 with ids zeroed. by is the panel order (propose).
template <class Fit, class Expected>
int resolve_names(const Panels& panels,
                  const Creation* creations,
                  std::size_t count,
                  const char* const* names,
                  std::size_t name_count,
                  Fit fit,
                  Expected expected,
                  std::uint64_t* ids,
                  std::array<Assignment, kMaxPanels>& out,
                  std::size_t& paired,
                  unsigned max_shift = 8,
                  PanelOrder by = PanelOrder::index) noexcept {
  std::size_t textured_count = 0, wanted = 0;
  for (std::uint32_t i = 0; i < panels.count && i < kMaxPanels; ++i)
    textured_count += textured(panels.panels[i]);
  for (std::size_t k = 0; k < name_count; ++k)
    wanted += names[k] && names[k][0];
  for (unsigned shift = 0; textured_count && wanted && shift <= max_shift && count >= textured_count + shift; ++shift) {
    bool complete = false;
    paired = propose(panels, creations + (count - textured_count - shift), textured_count, out, complete, by);
    bool all = complete;
    for (std::size_t i = 0; all && i < paired; ++i) {
      std::uint32_t width = 0, height = 0;
      const auto* panel = out[i].panel;
      if (!expected(panel->texture.data(), width, height))
        continue;
      bool lowest = true;
      for (std::size_t j = 0; j < paired; ++j)
        lowest = lowest && !(out[j].panel->index < panel->index && std::strcmp(out[j].panel->texture.data(), panel->texture.data()) == 0);
      all = !lowest || (out[i].creation.width == width && out[i].creation.height == height);
    }
    for (std::size_t k = 0; k < name_count; ++k) {
      ids[k] = 0;
      if (!names[k] || !names[k][0])
        continue;
      for (std::size_t i = 0; i < paired; ++i)
        if (out[i].creation.id && fit(out[i].creation) && std::strcmp(out[i].panel->texture.data(), names[k]) == 0)
          ids[k] = out[i].creation.id;
      all = all && ids[k];
    }
    if (all)
      return static_cast<int>(shift);
  }
  for (std::size_t k = 0; k < name_count; ++k)
    ids[k] = 0;
  return -1;
}
// Pairing with no profile names (aircraft discovery). Every cockpit panel
// texture logged so far has 5 mips; UI textures (EFB, toolbar: 10-12 mips)
// sometimes follow the cockpit's burst, as on a live PMDG 777 load that
// needed shift 4. Dropping only such trailing creations before pairing the
// last ones was right on all 9 clean loads replayed on 2026-10-31. Keeping
// only 5-mip creations was not: the iniBuilds A380 and FlyByWire A380 have
// 10-11 mip EFB panel textures among the cockpit's.
inline constexpr std::uint32_t kCockpitPanelMips = 5;
template <class Creations>
void trim_trailing_ui(Creations& burst) noexcept {
  while (!burst.empty() && burst.back().mips != kCockpitPanelMips)
    burst.pop_back();
}
inline std::size_t textured_count(const Panels& panels) noexcept {
  std::size_t n = 0;
  for (std::uint32_t i = 0; i < panels.count && i < kMaxPanels; ++i)
    n += textured(panels.panels[i]);
  return n;
}
// Which aircraft a table belongs to: its textured panels' names, in any
// order. The Fenix A320 rebuilt its table during one flight load with its two
// VCockpit17 panels moved from first to last (2026-10-31); that is still the
// same aircraft.
inline std::uint64_t aircraft_signature(const Panels& panels) noexcept {
  std::array<const char*, kMaxPanels> names{};
  std::size_t n = 0;
  for (std::uint32_t i = 0; i < panels.count && i < kMaxPanels; ++i)
    if (textured(panels.panels[i]))
      names[n++] = panels.panels[i].texture.data();
  for (std::size_t i = 1; i < n; ++i)
    for (std::size_t j = i; j > 0 && std::strcmp(names[j - 1], names[j]) > 0; --j) {
      const auto* t = names[j - 1];
      names[j - 1] = names[j];
      names[j] = t;
    }
  std::uint64_t hash = 0xcbf29ce484222325ull;
  for (std::size_t i = 0; i < n; ++i)
    for (const char* c = names[i];; ++c) {
      hash = (hash ^ static_cast<unsigned char>(*c)) * 0x100000001b3ull;
      if (!*c)
        break;
    }
  return hash;
}
// Cockpit tables seen in this simulator process, by aircraft_signature. A
// table is clean when it is the process's first or the same aircraft again.
// After a switch to another aircraft, leftover textures can sit among the new
// cockpit's creations (replayed loads, 2026-10-31: an A350 after a 777 needed
// shift 2 with neighbours paired wrongly), so such a pairing is not learned.
class ProcessTables {
 public:
  bool observe(std::uint64_t signature) noexcept {
    const bool clean = !last_ || last_ == signature;
    last_ = signature;
    return clean;
  }

 private:
  std::uint64_t last_{};
};
// Live read of the current process. Refuses any other build.
Panels read_panels() noexcept;
}  // namespace taxi_camera::display_identity
