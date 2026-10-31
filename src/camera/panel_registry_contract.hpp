#pragma once
#include "relocatable_contract.hpp"
// The VCockpit panel table: a static array of up to 64 panel pointers with the
// count at +0x200, which the panel.cfg reader fills with one 0x690-byte panel
// per [VCockpitNN]/[VPaintingNN] section (display_panels.hpp). Captured
// 2026-10-31 from the Store image (PE timestamp 1792432563, SizeOfImage
// 0xE2A5000; the table was at RVA 0x0A738BC0, on 1.8.16.0 at 0x0A50B160):
// - PanelRelease: the first 106 bytes of the table's atexit release. It loads
//   the count from table+0x200, walks the pointers from the table base,
//   destroys each panel and frees it with a sized delete of 0x690 bytes, then
//   clears the slot. The template therefore proves the table address, the
//   count offset and the panel size the reader depends on.
// Only decoded external PC-relative operands vary. The table's own accessor is
// not used: it guards a function-local static through a TLS offset that moves
// with every thread_local the simulator adds.
namespace taxi_camera::native_camera::panel_registry_contract {
inline constexpr std::uint32_t PanelRelease = 0, PanelTable = 1, PanelDestructor = 2, SizedDelete = 3;
inline constexpr std::uint32_t PanelReleaseBytes = 106;
// Pointer array plus count, as the release reads them.
inline constexpr std::uint32_t PanelTableBytes = 0x208, CountOffset = 0x200, PanelBytes = 0x690;
inline const relocatable::ContractModel& model() {
  static const auto result = [] {
    relocatable::ContractModel out;
    out.symbols.push_back({"panel_table_release", relocatable::SectionKind::code, PanelReleaseBytes});
    out.symbols.push_back({"panel_table", relocatable::SectionKind::writable_data, PanelTableBytes});
    out.symbols.push_back({"panel_destructor", relocatable::SectionKind::code, 1});
    out.symbols.push_back({"sized_delete", relocatable::SectionKind::code, 1});
    out.code.push_back({PanelRelease,
                        {0x48, 0x89, 0x6c, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x05, 0x00, 0x00, 0x00, 0x00, 0x33, 0xed,
                         0x8b, 0xfd, 0x85, 0xc0, 0x7e, 0x52, 0x48, 0x89, 0x5c, 0x24, 0x30, 0x48, 0x8d, 0x1d, 0x00, 0x00, 0x00, 0x00,
                         0x48, 0x89, 0x74, 0x24, 0x38, 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x33, 0x48, 0x85, 0xf6,
                         0x74, 0x1e, 0x48, 0x8b, 0xce, 0xe8, 0x00, 0x00, 0x00, 0x00, 0xba, 0x90, 0x06, 0x00, 0x00, 0x48, 0x8b, 0xce,
                         0xe8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0x2b, 0x8b, 0x05, 0x00, 0x00, 0x00, 0x00, 0xff, 0xc7, 0x48, 0x83,
                         0xc3, 0x08, 0x3b, 0xf8, 0x7c, 0xd0, 0x48, 0x8b, 0x74, 0x24, 0x38, 0x48, 0x8b, 0x5c, 0x24, 0x30},
                        {
                            {12, 4, 16, relocatable::AddressKind::pc_relative, PanelTable, CountOffset},
                            {32, 4, 36, relocatable::AddressKind::pc_relative, PanelTable, 0},
                            {60, 4, 64, relocatable::AddressKind::pc_relative, PanelDestructor, 0},
                            {73, 4, 77, relocatable::AddressKind::pc_relative, SizedDelete, 0},
                            {82, 4, 86, relocatable::AddressKind::pc_relative, PanelTable, CountOffset},
                        }});
    return out;
  }();
  return result;
}
}  // namespace taxi_camera::native_camera::panel_registry_contract
