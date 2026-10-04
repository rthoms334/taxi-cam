#include "../../src/shared/display_snapshot.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
using namespace taxi_camera::standalone;
unsigned checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
// Padded rows like a D3D12 placed footprint: the pitch exceeds width * 4.
std::vector<std::uint8_t> pattern(unsigned width, unsigned height, unsigned pitch) {
  std::vector<std::uint8_t> pixels(std::size_t{pitch} * height, 0xEE);
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      auto* p = pixels.data() + std::size_t{y} * pitch + x * 4;
      p[0] = static_cast<std::uint8_t>(x * 16);
      p[1] = static_cast<std::uint8_t>(y * 16);
      p[2] = 200;
      p[3] = 7;
    }
  return pixels;
}
}  // namespace

int main() {
  unsigned w = 0, h = 0;
  fit_snapshot(800, 600, 1024, w, h);
  require(w == 800 && h == 600, "A texture within the limit keeps its size");
  fit_snapshot(4096, 4096, 1024, w, h);
  require(w == 1024 && h == 1024, "Square textures shrink to the edge limit");
  fit_snapshot(1644, 1024, 1024, w, h);
  require(w == 1024 && h == 638, "Wide A350 EFIS surface keeps its aspect");
  fit_snapshot(1560, 2340, 1024, w, h);
  require(w == 683 && h == 1024, "Tall A340-300 grid keeps its aspect");
  fit_snapshot(5000, 1, 1024, w, h);
  require(w == 1024 && h == 1, "A thin texture keeps at least one row");

  // Full size: RGBA bytes become BGR, alpha and row padding are dropped.
  const auto rgba = pattern(4, 2, 32);
  auto image = downsample_snapshot(rgba.data(), 32, 4, 2, false, 1024);
  require(image.width == 4 && image.height == 2 && image.bgr.size() == 24, "Unscaled image size");
  require(image.bgr[0] == 200 && image.bgr[1] == 0 && image.bgr[2] == 0, "RGBA red/blue swap at the origin");
  const auto* last = image.bgr.data() + (1 * 4 + 3) * 3;
  require(last[0] == 200 && last[1] == 16 && last[2] == 48, "RGBA last pixel");
  image = downsample_snapshot(rgba.data(), 32, 4, 2, true, 1024);
  require(image.bgr[0] == 0 && image.bgr[2] == 200, "BGRA bytes keep their order");

  // 4 x 2 into 2 x 1: each output pixel averages a 2 x 2 box.
  image = downsample_snapshot(rgba.data(), 32, 4, 2, false, 2);
  require(image.width == 2 && image.height == 1, "Downsampled size");
  require(image.bgr[0] == 200 && image.bgr[1] == 8 && image.bgr[2] == 8, "First box average");
  require(image.bgr[3] == 200 && image.bgr[4] == 8 && image.bgr[5] == 40, "Second box average");

  require(downsample_snapshot(nullptr, 32, 4, 2, false, 2).bgr.empty(), "Null pixels refused");
  require(downsample_snapshot(rgba.data(), 8, 4, 2, false, 2).bgr.empty(), "Pitch below the row width refused");

  // 24-bit bottom-up BMP with 4-byte row padding.
  SnapshotImage small;
  small.width = 3;
  small.height = 2;
  small.bgr = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
  const auto bmp = encode_snapshot_bmp(small);
  BITMAPFILEHEADER header{};
  BITMAPINFOHEADER info{};
  require(bmp.size() == sizeof(header) + sizeof(info) + 12 * 2, "BMP size includes padded rows");
  std::memcpy(&header, bmp.data(), sizeof(header));
  std::memcpy(&info, bmp.data() + sizeof(header), sizeof(info));
  require(header.bfType == 0x4d42 && header.bfSize == bmp.size() && header.bfOffBits == sizeof(header) + sizeof(info), "BMP file header");
  require(info.biWidth == 3 && info.biHeight == 2 && info.biBitCount == 24 && info.biCompression == BI_RGB, "BMP info header");
  const auto* rows = bmp.data() + header.bfOffBits;
  require(rows[0] == 10 && rows[8] == 18 && rows[12] == 1 && rows[20] == 9, "BMP rows are stored bottom-up");
  require(encode_snapshot_bmp({}).empty(), "Empty image refused");

  // Saving replaces the file through a temporary, creating both folders.
  wchar_t temp[MAX_PATH]{};
  require(GetTempPathW(MAX_PATH, temp) != 0, "Temporary folder");
  wchar_t unique[64];
  std::swprintf(unique, 64, L"taxi-cam-snapshot-%lu-%llu", GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()));
  const std::wstring root = std::wstring(temp) + unique;
  const std::wstring path = root + L"\\snapshots\\display.bmp";
  require(save_snapshot_bmp(path, small), "Save snapshot");
  require(save_snapshot_bmp(path, image), "Replace snapshot");
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(file != INVALID_HANDLE_VALUE, "Saved snapshot exists");
  LARGE_INTEGER size{};
  GetFileSizeEx(file, &size);
  CloseHandle(file);
  require(size.QuadPart == static_cast<LONGLONG>(encode_snapshot_bmp(image).size()), "Second save replaced the first");
  require(GetFileAttributesW((path + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "No temporary file left behind");
  require(!save_snapshot_bmp(path, {}), "Empty image is not saved");
  DeleteFileW(path.c_str());
  RemoveDirectoryW((root + L"\\snapshots").c_str());
  RemoveDirectoryW(root.c_str());

  for (auto result :
       {DisplaySnapshotResult::none, DisplaySnapshotResult::pending, DisplaySnapshotResult::ready, DisplaySnapshotResult::unavailable,
        DisplaySnapshotResult::unsupported, DisplaySnapshotResult::timeout, DisplaySnapshotResult::lost, DisplaySnapshotResult::failed})
    require(std::strcmp(display_snapshot_name(result), "unknown") != 0 && display_snapshot_text(result)[0] != 0, "Every result is named");
  std::printf("PASS: %u display snapshot checks.\n", checks);
  return 0;
}
