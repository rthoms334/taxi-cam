#include "../../src/app/whats_new_image.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
using namespace taxi_camera::standalone;
unsigned checks{};
void require(bool ok, const char* message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}
// 4 x 2 RGB: red, green, blue, white over four black pixels.
constexpr unsigned char Png[]{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
                              0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0xf0,
                              0xca, 0xea, 0x34, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8,
                              0xcf, 0xc0, 0xc0, 0x00, 0xc6, 0x40, 0xc0, 0x80, 0x0c, 0x00, 0x6b, 0xae, 0x05, 0xfb, 0x23,
                              0x0b, 0x08, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
// BGRA bytes of the DIB's pixel at x, y.
std::array<unsigned char, 4> pixel(HBITMAP bitmap, int x, int y) {
  DIBSECTION dib{};
  require(GetObjectW(bitmap, sizeof(dib), &dib) == sizeof(dib) && dib.dsBm.bmBits, "The picture is a DIB section");
  const auto* row = static_cast<const unsigned char*>(dib.dsBm.bmBits) + static_cast<std::size_t>(y) * dib.dsBm.bmWidthBytes;
  return {row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]};
}
}  // namespace

int main() {
  try {
    const std::string png(reinterpret_cast<const char*>(Png), sizeof(Png));
    int width{}, height{};
    HBITMAP full = decode_whats_new_image(png, 100, 100, width, height);
    require(full && width == 4 && height == 2, "A picture smaller than its space keeps its size");
    require(pixel(full, 0, 0) == std::array<unsigned char, 4>{0, 0, 255, 255} &&
                pixel(full, 2, 0) == std::array<unsigned char, 4>{255, 0, 0, 255} &&
                pixel(full, 3, 0) == std::array<unsigned char, 4>{255, 255, 255, 255} && pixel(full, 1, 1)[0] == 0,
            "Rows are top-down BGRA in the PNG's colours");
    DeleteObject(full);
    HBITMAP narrow = decode_whats_new_image(png, 2, 100, width, height);
    require(narrow && width == 2 && height == 1, "A wider picture scales down to the width, keeping its shape");
    DeleteObject(narrow);
    HBITMAP short_one = decode_whats_new_image(png, 100, 1, width, height);
    require(short_one && width == 2 && height == 1, "A taller picture scales down to the height, keeping its shape");
    DeleteObject(short_one);
    require(!decode_whats_new_image("not a picture", 100, 100, width, height) && !width && !height, "Other bytes are no picture");
    require(!decode_whats_new_image(png, 0, 100, width, height) && !decode_whats_new_image({}, 100, 100, width, height),
            "No space or no bytes is no picture");
    std::printf("PASS What's new pictures: %u decode and scale checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL What's new pictures: %s\n", error.what());
    return 1;
  }
}
