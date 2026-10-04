#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace taxi_camera::display_identity {
// Display identity research (diagnostics only; nothing routes by it yet).
// Store 1.8.16.0 static decode: the panel.cfg reader (0x30F5E00) creates one
// 0x690-byte panel per [VCockpitNN]/[VPaintingNN] section and keeps up to 64
// in a static registry (getter 0x30F41F0) with the count at +0x200.
inline constexpr std::uint32_t kTestedTimestamp = 1787653788, kTestedImageSize = 0xE108600;
inline constexpr std::uint32_t kPanelRegistryRva = 0xA50B160, kPanelCountOffset = 0x200, kMaxPanels = 64;
// Panel fields: canvas object (i16 format, i32 width at +4, height at +8),
// inline section name, texture= name (small string: pointer when the u32 at
// +0xC is nonzero, else inline), index and kind (4 VCockpit, 8 VPainting).
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
// A panel the simulator draws into a texture of its own: a VCockpit panel
// with a canvas and a real texture= name. NO_TEXTURE panels get none.
inline bool textured(const Panel& p) noexcept {
  return p.kind == kKindVCockpit && p.canvas && p.texture[0] && std::strcmp(p.texture.data(), "NO_TEXTURE") != 0;
}
struct Creation {
  std::uint64_t id{}, tick{};
  std::uint32_t width{}, height{}, mips{}, format{};
};
struct Assignment {
  const Panel* panel{};
  Creation creation{};
};
// Proposed rule from two PMDG 777 sessions: panel textures are created in
// reverse panel-index order, and they are the multi-mip creations of the
// cockpit-load burst. Pairs the k-th such creation with the k-th textured
// panel counted from the highest index. complete is false when the burst has
// fewer candidates than textured panels.
inline std::size_t propose(const Panels& panels,
                           const Creation* burst,
                           std::size_t count,
                           std::array<Assignment, kMaxPanels>& out,
                           bool& complete) noexcept {
  std::array<const Panel*, kMaxPanels> order{};
  std::size_t textured_count = 0;
  for (std::uint32_t i = 0; i < panels.count && i < kMaxPanels; ++i)
    if (textured(panels.panels[i]))
      order[textured_count++] = &panels.panels[i];
  // Highest index first; insertion sort keeps this dependency-free.
  for (std::size_t i = 1; i < textured_count; ++i)
    for (std::size_t j = i; j > 0 && order[j - 1]->index < order[j]->index; --j) {
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
// the named panels' (one iniBuilds A350 flight made two more 2048 x 2048
// textures at the end of its burst) move every pairing back by the same
// count. The window of the last textured-panel-count creations therefore
// moves back one creation at a time, up to max_shift, until every non-empty
// name lands on a creation fit() accepts (the profile's display shape).
// Returns the shift used with ids and out filled, or -1 with ids zeroed.
template <class Fit>
int resolve_names(const Panels& panels,
                  const Creation* creations,
                  std::size_t count,
                  const char* const* names,
                  std::size_t name_count,
                  Fit fit,
                  std::uint64_t* ids,
                  std::array<Assignment, kMaxPanels>& out,
                  std::size_t& paired,
                  unsigned max_shift = 4) noexcept {
  std::size_t textured_count = 0, wanted = 0;
  for (std::uint32_t i = 0; i < panels.count && i < kMaxPanels; ++i)
    textured_count += textured(panels.panels[i]);
  for (std::size_t k = 0; k < name_count; ++k)
    wanted += names[k] && names[k][0];
  for (unsigned shift = 0; textured_count && wanted && shift <= max_shift && count >= textured_count + shift; ++shift) {
    bool complete = false;
    paired = propose(panels, creations + (count - textured_count - shift), textured_count, out, complete);
    bool all = complete;
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
// Live read of the current process. Refuses any other build.
Panels read_panels() noexcept;
}  // namespace taxi_camera::display_identity
