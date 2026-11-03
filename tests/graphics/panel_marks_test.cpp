#include "../../src/shared/panel_marks.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
using namespace taxi_camera::panel_marks;
unsigned checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
struct Image {
  unsigned width{}, height{};
  std::vector<std::uint8_t> rgba;
  std::uint8_t* at(unsigned x, unsigned y) { return rgba.data() + (std::size_t{y} * width + x) * 4; }
};
// A dark display with grey, white and green "symbology" and a few lone
// saturated pixels, like a PFD.
Image page(unsigned width, unsigned height, unsigned seed) {
  Image image{width, height, std::vector<std::uint8_t>(std::size_t{width} * height * 4)};
  std::uint32_t state = seed;
  const auto next = [&] { return state = state * 1664525u + 1013904223u; };
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      auto* p = image.at(x, y);
      p[0] = p[1] = p[2] = 20;
      p[3] = 255;
    }
  for (int i = 0; i < 400; ++i) {
    const unsigned x0 = next() % width, y0 = next() % height, w = 1 + next() % 40, h = 1 + next() % 3;
    const std::uint8_t colours[4][3] = {{255, 255, 255}, {0, 255, 0}, {128, 128, 128}, {255, 0, 255}};
    const auto* c = colours[next() % 4];
    for (unsigned y = y0; y < y0 + h && y < height; ++y)
      for (unsigned x = x0; x < x0 + w && x < width; ++x)
        std::memcpy(image.at(x, y), c, 3);
  }
  return image;
}
// The helper's canvas: a one-cell black border round the cells.
void draw(Image& image, const Mark& m, unsigned left, unsigned top) {
  const auto cells = encode_cells(encode_payload(m));
  const unsigned rows = static_cast<unsigned>(cells.size() / kColumns), cell = m.cell;
  for (unsigned y = 0; y < (rows + 2) * cell; ++y)
    for (unsigned x = 0; x < (kColumns + 2) * cell; ++x) {
      const unsigned cx = x / cell, cy = y / cell;
      unsigned v = 0;
      if (cx >= 1 && cx <= kColumns && cy >= 1 && cy <= rows)
        v = cells[std::size_t{cy - 1} * kColumns + cx - 1];
      auto* p = image.at(left + x, top + y);
      p[0] = v & 4 ? 255 : 0;
      p[1] = v & 2 ? 255 : 0;
      p[2] = v & 1 ? 255 : 0;
    }
}
// The page rendered into a texture of another size, bilinear.
Image resample(const Image& from, unsigned width, unsigned height) {
  Image to{width, height, std::vector<std::uint8_t>(std::size_t{width} * height * 4)};
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      const double sx = (x + 0.5) * from.width / width - 0.5, sy = (y + 0.5) * from.height / height - 0.5;
      const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
      const double fx = sx - x0, fy = sy - y0;
      const auto px = [&](int xx, int yy, int c) {
        xx = xx < 0 ? 0 : xx >= static_cast<int>(from.width) ? static_cast<int>(from.width) - 1 : xx;
        yy = yy < 0 ? 0 : yy >= static_cast<int>(from.height) ? static_cast<int>(from.height) - 1 : yy;
        return double(from.rgba[(std::size_t(yy) * from.width + std::size_t(xx)) * 4 + std::size_t(c)]);
      };
      for (int c = 0; c < 4; ++c) {
        const double v = (px(x0, y0, c) * (1 - fx) + px(x0 + 1, y0, c) * fx) * (1 - fy) +
                         (px(x0, y0 + 1, c) * (1 - fx) + px(x0 + 1, y0 + 1, c) * fx) * fy;
        to.rgba[(std::size_t{y} * width + x) * 4 + std::size_t(c)] = static_cast<std::uint8_t>(v + 0.5);
      }
    }
  return to;
}
Mark gauge(const char* name,
           const char* gauge_name,
           std::uint8_t index,
           std::uint16_t x,
           std::uint16_t y,
           std::uint16_t w,
           std::uint16_t h,
           std::uint16_t pw,
           std::uint16_t ph,
           std::uint8_t cell) {
  Mark m;
  m.cell = cell;
  m.gauge = index;
  m.x = x, m.y = y, m.width = w, m.height = h, m.panel_width = pw, m.panel_height = ph;
  std::strncpy(m.name.data(), name, m.name.size() - 1);
  std::strncpy(m.gauge_name.data(), gauge_name, m.gauge_name.size() - 1);
  return m;
}
unsigned block_width(const Mark& m) {
  return (kColumns + 2) * m.cell;
}
unsigned block_height(const Mark& m) {
  return (rows_for(encode_payload(m).size()) + 2) * m.cell;
}
// Draws each mark centred in its gauge, as the helper does.
void draw_centred(Image& image, const Mark& m) {
  draw(image, m, m.x + (m.width - block_width(m)) / 2, m.y + (m.height - block_height(m)) / 2);
}
std::size_t find(const Image& image, Mark* out, std::size_t max, bool bgra = false) {
  return find_marks(image.rgba.data(), std::uint64_t{image.width} * 4, image.width, image.height, bgra, out, max);
}
const Mark* named(const Mark* marks, std::size_t count, const char* name, std::uint8_t gauge_index) {
  for (std::size_t i = 0; i < count; ++i)
    if (std::strcmp(marks[i].name.data(), name) == 0 && marks[i].gauge == gauge_index)
      return &marks[i];
  return nullptr;
}
}  // namespace

int main() {
  // The JS helper's bytes and cells for the PMDG 777 captain's inboard DU
  // (node, taxi-cam-marks.js encodePayload/encodeCells, 2026-11-03).
  {
    const auto m = gauge("DUS", "DU_LeftInboard", 1, 1058, 33, 958, 971, 2048, 2048, 10);
    const auto payload = encode_payload(m);
    std::string hex;
    for (const auto b : payload) {
      char two[3];
      std::snprintf(two, sizeof(two), "%02x", b);
      hex += two;
    }
    require(hex == "0123000a010422002103be03cb08000800034455530e44555f4c656674496e626f61726400", "payload matches the JS helper");
    std::string cells;
    for (const auto c : encode_cells(payload))
      cells += static_cast<char>('0' + c);
    require(cells ==
                "7070421635074017000221400024004074104000410073700036260400002000700321052523034402125276461453147721113346115730027114400"
                "1117300",
            "cells match the JS helper");
  }
  // One block at the page's own size.
  {
    auto image = page(512, 512, 1);
    const auto m = gauge("VCISFD", "ISFDScreen", 0, 0, 0, 512, 512, 512, 512, 4);
    draw_centred(image, m);
    Mark found[4];
    require(find(image, found, 4) == 1, "one block found at 1x");
    require(std::strcmp(found[0].name.data(), "VCISFD") == 0 && std::strcmp(found[0].gauge_name.data(), "ISFDScreen") == 0,
            "names decoded");
    require(std::fabs(found[0].pitch_x - 4) < 0.3 && std::fabs(found[0].pitch_y - 4) < 0.3, "pitch is the cell size");
  }
  // PMDG 777 DUS: four DUs and the 4x4 Core gauge (too small for a block),
  // drawn on the 2048 page and shown in textures of 2048, 1600 and 800, plus
  // 2 px corner markers at each gauge origin like another tool's.
  for (const unsigned texture : {2048u, 1600u, 800u}) {
    auto image = page(2048, 2048, 2);
    const Mark dus[] = {gauge("DUS", "DU_LeftOutboard", 0, 30, 33, 958, 971, 2048, 2048, 10),
                        gauge("DUS", "DU_LeftInboard", 1, 1058, 33, 958, 971, 2048, 2048, 10),
                        gauge("DUS", "DU_RightInboard", 2, 30, 1058, 958, 971, 2048, 2048, 10),
                        gauge("DUS", "DU_RightOutboard", 3, 1058, 1058, 958, 971, 2048, 2048, 10)};
    for (const auto& m : dus) {
      draw_centred(image, m);
      for (unsigned i = 0; i < 2; ++i) {
        auto* p = image.at(m.x + i, m.y);
        p[0] = 255, p[1] = 0, p[2] = 255;
      }
    }
    const auto shown = resample(image, texture, texture);
    Mark found[8];
    const auto count = find(shown, found, 8);
    char message[96];
    std::snprintf(message, sizeof(message), "four DU blocks found in a %u texture", texture);
    require(count == 4, message);
    for (const auto& m : dus) {
      const auto* f = named(found, count, "DUS", m.gauge);
      require(f && std::strcmp(f->gauge_name.data(), m.gauge_name.data()) == 0, "each DU by gauge index");
      Rect rect;
      require(texture_rect(*f, texture, texture, rect), "rect from panel size");
      const unsigned want_x = (m.x * texture + 1024) / 2048, want_w = (m.width * texture + 1024) / 2048;
      require(rect.x == want_x && rect.width == want_w, "gauge rect scaled to the texture");
      const double scale = f->pitch_x / f->cell;
      require(std::fabs(scale - texture / 2048.0) < 0.05, "measured pitch gives the texture's scale");
    }
  }
  // iniBuilds A350 EFIS (1644 x 1024) in BGRA, with a mark per side.
  {
    auto image = page(1644, 1024, 3);
    auto m = gauge("$EFIS_LEFT", "EFIS", 0, 0, 0, 1644, 1024, 1644, 1024, 5);
    std::strcpy(m.info.data(), "L");
    draw_centred(image, m);
    for (unsigned y = 0; y < image.height; ++y)
      for (unsigned x = 0; x < image.width; ++x)
        std::swap(image.at(x, y)[0], image.at(x, y)[2]);
    Mark found[2];
    require(find(image, found, 2, true) == 1 && std::strcmp(found[0].name.data(), "$EFIS_LEFT") == 0 &&
                std::strcmp(found[0].info.data(), "L") == 0,
            "BGRA texture decodes");
    require(find(image, found, 2, false) == 0, "BGRA read as RGBA fails the finder or CRC");
  }
  // A panel-level block with a long info text keeps within the payload.
  {
    auto image = page(2355, 1467, 4);
    Mark m = gauge("$EFB_LEFT", "VCockpit08", 255, 0, 0, 2355, 1467, 2355, 1467, 7);
    m.kind = Kind::panel;
    std::memset(m.info.data(), 'k', m.info.size() - 1);
    const auto payload = encode_payload(m);
    require(payload.size() <= kMaxPayload, "payload capped");
    draw(image, m, 2355 - block_width(m) - 14, 1467 - block_height(m) - 14);
    Mark found[2];
    require(find(image, found, 2) == 1 && found[0].kind == Kind::panel && std::strlen(found[0].info.data()) > 50, "panel block decodes");
  }
  // A damaged cell fails the CRC rather than decoding wrongly.
  {
    auto image = page(512, 512, 5);
    const auto m = gauge("ISIS_1", "ISIS", 0, 0, 0, 512, 512, 512, 512, 4);
    const unsigned left = (512 - block_width(m)) / 2, top = (512 - block_height(m)) / 2;
    draw(image, m, left, top);
    for (unsigned y = 0; y < 4; ++y)
      for (unsigned x = 0; x < 4; ++x) {
        auto* p = image.at(left + 4 * 6 + x, top + 4 * 4 + y);
        p[0] ^= 255;
      }
    Mark found[2];
    require(find(image, found, 2) == 0, "damaged block refused");
  }
  // No marks: a busy display gives nothing.
  {
    const auto image = page(2048, 2048, 6);
    Mark found[2];
    require(find(image, found, 2) == 0, "no false blocks");
  }
  std::printf("panel-marks: %u checks passed\n", checks);
  return 0;
}
