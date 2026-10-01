#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace taxi_camera::standalone {
// One-shot picture of a tracked display texture for PFD routing. The bridge
// copies the texture's base mip at a proven submission boundary (the display
// write path in reverse), shrinks it on its control thread and writes a BMP
// that the companion shows with the profile's display rectangles on top.
enum class DisplaySnapshotResult : std::uint32_t {
  none,
  pending,
  ready,
  unavailable,  // No tracked display texture has this ID now.
  unsupported,  // Not an 8-bit RGBA/BGRA texture, or above the size limit.
  timeout,      // No proven copy site in time; the texture may not be drawn.
  lost,         // Destroyed or reset before the copy completed.
  failed,       // GPU allocation, mapping or file write failed.
};
inline const wchar_t* display_snapshot_text(DisplaySnapshotResult result) noexcept {
  switch (result) {
    case DisplaySnapshotResult::none:
      return L"No snapshot taken.";
    case DisplaySnapshotResult::pending:
      return L"Taking a snapshot at the next safe display copy...";
    case DisplaySnapshotResult::ready:
      return L"Snapshot ready.";
    case DisplaySnapshotResult::unavailable:
      return L"That texture is no longer tracked. Refresh textures and try again.";
    case DisplaySnapshotResult::unsupported:
      return L"This texture's format or size cannot be snapshotted.";
    case DisplaySnapshotResult::timeout:
      return L"No safe copy point within 5 s. The texture may not be drawn at the moment.";
    case DisplaySnapshotResult::lost:
      return L"The texture was destroyed or the flight reset before the copy finished.";
    case DisplaySnapshotResult::failed:
      return L"The snapshot could not be copied or saved. See bridge.log.";
  }
  return L"Unknown snapshot result.";
}
inline const char* display_snapshot_name(DisplaySnapshotResult result) noexcept {
  switch (result) {
    case DisplaySnapshotResult::none:
      return "none";
    case DisplaySnapshotResult::pending:
      return "pending";
    case DisplaySnapshotResult::ready:
      return "ready";
    case DisplaySnapshotResult::unavailable:
      return "unavailable";
    case DisplaySnapshotResult::unsupported:
      return "unsupported";
    case DisplaySnapshotResult::timeout:
      return "timeout";
    case DisplaySnapshotResult::lost:
      return "lost";
    case DisplaySnapshotResult::failed:
      return "failed";
  }
  return "unknown";
}
// Base mip only: a 4096 x 4096 RGBA8 texture is the largest accepted copy.
inline constexpr std::uint64_t kDisplaySnapshotMaxBytes = 4096ull * 4096ull * 4ull;
inline constexpr unsigned kDisplaySnapshotMaxEdge = 1024;
inline constexpr std::uint64_t kDisplaySnapshotTimeoutMs = 5000;
// A planned copy that has not completed by then may have been cancelled with
// its batch; plan another. At most one copy is planned per interval.
inline constexpr std::uint64_t kDisplaySnapshotReplanMs = 500;

inline std::wstring display_snapshot_path() {
  wchar_t local[32768]{};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
  if (!n || n >= 32700)
    return {};
  return std::wstring(local) + L"\\Taxi Cam\\snapshots\\display.bmp";
}
struct SnapshotImage {
  unsigned width = 0, height = 0;
  std::vector<std::uint8_t> bgr;  // Tight rows, top-down, 3 bytes per pixel.
};
inline void fit_snapshot(unsigned width, unsigned height, unsigned max_edge, unsigned& out_width, unsigned& out_height) noexcept {
  out_width = width;
  out_height = height;
  if (!width || !height || !max_edge || std::max(width, height) <= max_edge)
    return;
  if (width >= height) {
    out_width = max_edge;
    out_height = std::max(1u, static_cast<unsigned>((std::uint64_t{height} * max_edge + width / 2) / width));
  } else {
    out_height = max_edge;
    out_width = std::max(1u, static_cast<unsigned>((std::uint64_t{width} * max_edge + height / 2) / height));
  }
}
// Box-filters 8-bit RGBA or BGRA rows into BGR. Alpha is ignored: cockpit
// displays do not use it for colour. Stored bytes are shown as they are, so
// an sRGB texture and a UNORM texture holding sRGB values both look right.
inline SnapshotImage downsample_snapshot(const std::uint8_t* pixels,
                                         std::uint64_t row_pitch,
                                         unsigned width,
                                         unsigned height,
                                         bool bgra,
                                         unsigned max_edge) {
  SnapshotImage image;
  if (!pixels || !width || !height || row_pitch < std::uint64_t{width} * 4)
    return image;
  fit_snapshot(width, height, max_edge, image.width, image.height);
  image.bgr.resize(std::size_t{image.width} * image.height * 3);
  const unsigned red = bgra ? 2 : 0, blue = bgra ? 0 : 2;
  for (unsigned y = 0; y < image.height; ++y) {
    const unsigned y0 = static_cast<unsigned>(std::uint64_t{y} * height / image.height);
    const unsigned y1 = std::max(y0 + 1, static_cast<unsigned>(std::uint64_t{y + 1} * height / image.height));
    for (unsigned x = 0; x < image.width; ++x) {
      const unsigned x0 = static_cast<unsigned>(std::uint64_t{x} * width / image.width);
      const unsigned x1 = std::max(x0 + 1, static_cast<unsigned>(std::uint64_t{x + 1} * width / image.width));
      std::uint64_t r = 0, g = 0, b = 0;
      for (unsigned sy = y0; sy < y1; ++sy) {
        const auto* row = pixels + sy * row_pitch;
        for (unsigned sx = x0; sx < x1; ++sx) {
          const auto* p = row + std::size_t{sx} * 4;
          r += p[red];
          g += p[1];
          b += p[blue];
        }
      }
      const std::uint64_t n = std::uint64_t{y1 - y0} * (x1 - x0);
      auto* out = image.bgr.data() + (std::size_t{y} * image.width + x) * 3;
      out[0] = static_cast<std::uint8_t>((b + n / 2) / n);
      out[1] = static_cast<std::uint8_t>((g + n / 2) / n);
      out[2] = static_cast<std::uint8_t>((r + n / 2) / n);
    }
  }
  return image;
}
// 24-bit bottom-up BMP.
inline std::vector<std::uint8_t> encode_snapshot_bmp(const SnapshotImage& image) {
  std::vector<std::uint8_t> file;
  if (!image.width || !image.height || image.bgr.size() != std::size_t{image.width} * image.height * 3)
    return file;
  const std::uint32_t stride = (image.width * 3 + 3) & ~3u;
  const std::uint32_t pixels = stride * image.height;
  BITMAPFILEHEADER header{};
  BITMAPINFOHEADER info{};
  header.bfType = 0x4d42;
  header.bfOffBits = sizeof(header) + sizeof(info);
  header.bfSize = header.bfOffBits + pixels;
  info.biSize = sizeof(info);
  info.biWidth = static_cast<LONG>(image.width);
  info.biHeight = static_cast<LONG>(image.height);
  info.biPlanes = 1;
  info.biBitCount = 24;
  info.biCompression = BI_RGB;
  info.biSizeImage = pixels;
  file.resize(header.bfSize);
  std::memcpy(file.data(), &header, sizeof(header));
  std::memcpy(file.data() + sizeof(header), &info, sizeof(info));
  for (unsigned y = 0; y < image.height; ++y)
    std::memcpy(file.data() + header.bfOffBits + std::size_t{image.height - 1 - y} * stride,
                image.bgr.data() + std::size_t{y} * image.width * 3, std::size_t{image.width} * 3);
  return file;
}
// Writes through a temporary file so a reader never sees a partial image.
inline bool save_snapshot_bmp(const std::wstring& path, const SnapshotImage& image) {
  const auto bytes = encode_snapshot_bmp(image);
  const auto slash = path.find_last_of(L'\\');
  if (bytes.empty() || slash == std::wstring::npos)
    return false;
  CreateDirectoryW(path.substr(0, path.find_last_of(L'\\', slash - 1)).c_str(), nullptr);
  CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
  const auto temporary = path + L".tmp";
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0;
  const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
  CloseHandle(file);
  if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(temporary.c_str());
    return false;
  }
  return true;
}
}  // namespace taxi_camera::standalone
