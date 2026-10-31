#include "display_panels.hpp"

#include "camera_contract.hpp"
#include "local_memory.hpp"
#include "panel_registry_contract.hpp"

namespace taxi_camera::display_identity {
static_assert(kPanelCountOffset == native_camera::panel_registry_contract::CountOffset);
static_assert(kMaxPanels * 8 <= kPanelCountOffset);
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
    // Resolved once per process: a full scan of the image's code for the
    // table's release function, with the default full-region reader.
    static const auto table = [] {
      const auto module = GetModuleHandleW(nullptr);
      native_camera::LocalImageReader scan(module, image.image_size);
      return native_camera::resolve_panel_table(scan, image, reinterpret_cast<std::uintptr_t>(module));
    }();
    if (!table.table) {
      result.error = table.error.c_str();
      return result;
    }
    // One private-page scope for the whole table: pages are proven from
    // allocation and working-set metadata, not VirtualQuery region scans
    // (about 1 us per MB of homogeneous region; this runs every 2 s).
    native_camera::ScopedLocalMemoryQueryCache queries(native_camera::LocalMemoryQueryMode::private_pages);
    native_camera::LocalImageReader registry_reader(GetModuleHandleW(nullptr), image.image_size, native_camera::LocalImageQueryMode::pages);
    std::array<std::uint64_t, kMaxPanels + 1> registry{};
    if (!registry_reader.read(table.table, registry.data(), sizeof(registry))) {
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
      if (!section_name(panel.section.data())) {
        result.error = "panel_layout";
        return result;
      }
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
    if (!queries.finish()) {
      result.error = "panel_changed";
      return result;
    }
    result.signature = signature;
    result.error = nullptr;
  } catch (...) {
    result.error = "exception";
  }
  return result;
}
}  // namespace taxi_camera::display_identity
