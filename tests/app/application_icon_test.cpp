#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
unsigned checks{};
void require(bool ok, const char* message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}
BOOL CALLBACK first_group(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR context) {
  *reinterpret_cast<HRSRC*>(context) = FindResourceW(module, name, type);
  return FALSE;
}
void resource_sizes(const wchar_t* path) {
  const auto module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  require(module != nullptr, "Read executable resources without executing application code");
  HRSRC group{};
  EnumResourceNamesW(module, RT_GROUP_ICON, first_group, reinterpret_cast<LONG_PTR>(&group));
  require(group != nullptr, "Executable contains a primary shell icon group");
  const auto size = SizeofResource(module, group);
  const auto data = static_cast<const BYTE*>(LockResource(LoadResource(module, group)));
  require(data && size >= 6, "Readable icon group header");
  WORD type{}, count{};
  std::memcpy(&type, data + 2, 2);
  std::memcpy(&count, data + 4, 2);
  require(type == 1 && count && size >= 6u + 14u * count, "Bounded icon group directory");
  for (const auto expected : {16u, 32u, 48u, 64u, 256u}) {
    bool found = false;
    for (unsigned i = 0; i < count; ++i) {
      const auto entry = data + 6 + i * 14;
      found |= (entry[0] ? entry[0] : 256u) == expected && (entry[1] ? entry[1] : 256u) == expected;
    }
    require(found, "Primary icon provides each recommended shell resolution without resampling");
  }
  FreeLibrary(module);
}
void dimensions(HICON icon, int size) {
  ICONINFO info{};
  require(GetIconInfo(icon, &info), "Shell extraction returns a native icon");
  BITMAP bitmap{};
  const bool ok = info.hbmColor && GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) && bitmap.bmWidth == size && bitmap.bmHeight == size;
  if (info.hbmColor)
    DeleteObject(info.hbmColor);
  if (info.hbmMask)
    DeleteObject(info.hbmMask);
  require(ok, "Extracted icon has the requested shell dimensions");
}
void render(HICON icon, int size, const std::wstring& destination) {
  BITMAPINFO info{};
  info.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB, static_cast<DWORD>(size * size * 4), 0, 0, 0, 0};
  void* bits{};
  const auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  require(bitmap && bits, "Create an isolated icon-rendering bitmap");
  const auto dc = CreateCompatibleDC(nullptr);
  require(dc != nullptr, "Create isolated icon-rendering DC");
  const auto previous = SelectObject(dc, bitmap);
  auto pixels = static_cast<DWORD*>(bits);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x)
      pixels[y * size + x] = ((x / 8 + y / 8) & 1) ? 0xffcccccc : 0xffeeeeee;
  require(DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL) && GdiFlush(), "Shell icon renders onto checkerboard");
  // Inside the yellow border: the marking in the upper half, and below it
  // TAXI across the tile, or up to 32 px (too small for lettering) only the
  // taxiway centre line leading up to the stop bar.
  unsigned asphalt{}, marking{}, label_left{}, label_right{}, centre_line{}, hold_short_left{}, hold_short_right{};
  const int margin = size / 8;
  for (int i = 0; i < size * size; ++i) {
    const auto color = pixels[i] & 0xffffff;
    const auto red = (color >> 16) & 255, green = (color >> 8) & 255, blue = color & 255;
    const int x = i % size, y = i / size;
    asphalt += color == 0x121417;
    // Antialiased edges blend the yellow with asphalt; include blended yellow.
    const bool yellow = red > 130 && green > 110 && blue < 90 && red > green && green > blue + 50;
    marking += yellow;
    const bool inside = x >= margin && x < size - margin && y >= margin && y < size - margin;
    const bool lower = y >= size * 5 / 8;
    label_left += yellow && inside && lower && x < size / 3;
    label_right += yellow && inside && lower && x >= size * 2 / 3;
    centre_line += yellow && inside && lower && x >= size / 3 && x < size * 2 / 3;
    hold_short_left += yellow && inside && y < size / 2 && x < size / 3;
    hold_short_right += yellow && inside && y < size / 2 && x >= size * 2 / 3;
  }
  const bool lettered = size > 32;
  const bool branding = asphalt > static_cast<unsigned>(size * size / 4) && marking > static_cast<unsigned>(size * size / 50) &&
                        hold_short_left > 0 && hold_short_right > 0 && centre_line > 0 &&
                        (lettered ? label_left > 0 && label_right > 0 : label_left == 0 && label_right == 0);
  if (!branding)
    std::fprintf(stderr,
                 "Icon %d px: asphalt=%u marking=%u label-left=%u label-right=%u centre=%u hold-short-left=%u hold-short-right=%u\n", size,
                 asphalt, marking, label_left, label_right, centre_line, hold_short_left, hold_short_right);
  require(branding, "Extracted branding retains asphalt, the hold-short marking, and TAXI or (up to 32 px) the centre line alone");
  require((pixels[0] & 0xffffff) == 0xeeeeee, "Icon corner remains transparent");
  BITMAPFILEHEADER header{};
  header.bfType = 0x4d42;
  header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
  header.bfSize = header.bfOffBits + info.bmiHeader.biSizeImage;
  auto file = _wfopen(destination.c_str(), L"wb");
  require(file != nullptr, "Open the ignored visual-check artifact");
  const bool saved = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
                     std::fwrite(&info.bmiHeader, sizeof(info.bmiHeader), 1, file) == 1 &&
                     std::fwrite(bits, info.bmiHeader.biSizeImage, 1, file) == 1;
  std::fclose(file);
  SelectObject(dc, previous);
  DeleteDC(dc);
  DeleteObject(bitmap);
  require(saved, "Save extracted icon pixels for visual inspection");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 3, "Provide executable path and ignored output prefix");
    resource_sizes(argv[1]);
    HICON large{}, small{};
    require(ExtractIconExW(argv[1], 0, &large, &small, 1) > 0 && large && small, "Standard Windows shell extracts both icon sizes");
    DestroyIcon(large);
    DestroyIcon(small);
    for (const auto size : {16, 20, 24, 32, 40, 48, 64, 128, 256}) {
      large = small = nullptr;
      require(SHDefExtractIconW(argv[1], 0, 0, &large, &small, MAKELONG(size, 16)) == S_OK && large && small,
              "Windows shell extracts the primary icon at standard and high-DPI sizes");
      dimensions(large, size);
      dimensions(small, 16);
      render(large, size, std::wstring(argv[2]) + L"-" + std::to_wstring(size) + L".bmp");
      DestroyIcon(large);
      DestroyIcon(small);
    }
    std::printf("PASS application icon: %u PE-resource, shell-extraction, size, transparency and branding checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL application icon: %s\n", error.what());
    return 1;
  }
}
