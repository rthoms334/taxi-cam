#include "display_panels.hpp"

#include "local_memory.hpp"

namespace taxi_camera::display_identity {
Panels read_panels() noexcept {
  Panels result;
  try {
    // The image never changes within a process: verify its headers once.
    static const auto image = native_camera::parse_verified_main_image();
    result.timestamp = image.timestamp;
    result.image_size = image.image_size;
    if (!image.valid_image) {
      result.error = "image_unverified";
      return result;
    }
    if (image.timestamp != kTestedTimestamp || image.image_size != kTestedImageSize) {
      result.error = "unsupported_build";
      return result;
    }
    native_camera::LocalImageReader registry_reader(GetModuleHandleW(nullptr), image.image_size);
    std::array<std::uint64_t, kMaxPanels + 1> registry{};
    if (!registry_reader.read(kPanelRegistryRva, registry.data(), sizeof(registry))) {
      result.error = "registry_unreadable";
      return result;
    }
    const auto count = static_cast<std::uint32_t>(registry[kMaxPanels]);
    if (count > kMaxPanels) {
      result.error = "registry_count";
      return result;
    }
    native_camera::LocalMemoryReader reader;
    std::uint64_t signature = 0xcbf29ce484222325ull ^ count;
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto address = registry[i];
      signature = (signature ^ address) * 0x100000001b3ull;
      auto& panel = result.panels[result.count];
      unsigned char section[48]{}, texture[16]{}, name[48]{};
      std::uint64_t canvas_address = 0;
      if (!address || !reader.read(address + kCanvasOffset, &canvas_address, 8) ||
          !reader.read(address + kSectionOffset, section, sizeof(section)) ||
          !reader.read(address + kTextureOffset, texture, sizeof(texture)) || !reader.read(address + kIndexOffset, &panel.index, 4) ||
          !reader.read(address + kKindOffset, &panel.kind, 4)) {
        result.error = "panel_unreadable";
        return result;
      }
      copy_name(section, sizeof(section), panel.section);
      std::uint32_t heap = 0;
      std::memcpy(&heap, texture + (kTextureHeapFlag - kTextureOffset), 4);
      std::uint64_t text = 0;
      std::memcpy(&text, texture, 8);
      if (heap && text && reader.read(text, name, sizeof(name)))
        copy_name(name, sizeof(name), panel.texture);
      else if (!heap)
        copy_name(texture, 12, panel.texture);
      if (canvas_address) {
        std::int32_t size[2]{};
        panel.canvas = reader.read(canvas_address + 4, size, sizeof(size));
        panel.canvas_width = panel.canvas && size[0] > 0 ? static_cast<std::uint32_t>(size[0]) : 0;
        panel.canvas_height = panel.canvas && size[1] > 0 ? static_cast<std::uint32_t>(size[1]) : 0;
      }
      ++result.count;
    }
    result.signature = signature;
    result.error = nullptr;
  } catch (...) {
    result.error = "exception";
  }
  return result;
}
}  // namespace taxi_camera::display_identity
