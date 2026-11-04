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
// The helper's canvas: a one-cell black border round the cells, each 1 bit
// at level (the helper paints kLevel; older blocks 255).
void draw(Image& image, const Mark& m, unsigned left, unsigned top, std::uint8_t level = 255) {
  const auto cells = encode_cells(encode_payload(m));
  const unsigned rows = static_cast<unsigned>(cells.size() / kColumns), cell = m.cell;
  for (unsigned y = 0; y < (rows + 2) * cell; ++y)
    for (unsigned x = 0; x < (kColumns + 2) * cell; ++x) {
      const unsigned cx = x / cell, cy = y / cell;
      unsigned v = 0;
      if (cx >= 1 && cx <= kColumns && cy >= 1 && cy <= rows)
        v = cells[std::size_t{cy - 1} * kColumns + cx - 1];
      auto* p = image.at(left + x, top + y);
      p[0] = v & 4 ? level : 0;
      p[1] = v & 2 ? level : 0;
      p[2] = v & 1 ? level : 0;
    }
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
  // PMDG 777 DUS: four DUs on one 2048 texture, each with its dark block in
  // its top-right corner, plus 2 px markers at each gauge origin like another
  // tool's.
  {
    auto image = page(2048, 2048, 2);
    const Mark dus[] = {gauge("DUS", "DU_LeftOutboard", 0, 30, 33, 958, 971, 2048, 2048, kCell),
                        gauge("DUS", "DU_LeftInboard", 1, 1058, 33, 958, 971, 2048, 2048, kCell),
                        gauge("DUS", "DU_RightInboard", 2, 30, 1058, 958, 971, 2048, 2048, kCell),
                        gauge("DUS", "DU_RightOutboard", 3, 1058, 1058, 958, 971, 2048, 2048, kCell)};
    for (const auto& m : dus) {
      draw(image, m, m.x + m.width - block_width(m) - kCell, m.y + kCell, kLevel);
      for (unsigned i = 0; i < 2; ++i) {
        auto* p = image.at(m.x + i, m.y);
        p[0] = 255, p[1] = 0, p[2] = 255;
      }
    }
    Mark found[8];
    const auto count = find(image, found, 8);
    require(count == 4, "four DU blocks found");
    for (const auto& m : dus) {
      const auto* f = named(found, count, "DUS", m.gauge);
      require(f && std::strcmp(f->gauge_name.data(), m.gauge_name.data()) == 0, "each DU by gauge index");
      Rect rect;
      require(texture_rect(*f, 2048, 2048, rect) && rect.x == m.x && rect.y == m.y && rect.width == m.width && rect.height == m.height,
              "gauge rect in the texture");
      require(f->left > m.x + m.width / 2 && f->top < m.y + 4 * kCell, "block in the gauge's top-right corner");
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
  // The shipped helper: near-black 2 px cells in each gauge's top-right
  // corner, read at one texture pixel per page pixel (A350 2026-11-04: every
  // panel's page size was its texture size), on dark, grey and light screens.
  for (const unsigned background : {0u, 4u, 60u, 235u}) {
    auto image = page(1644, 1024, 7);
    for (unsigned y = 0; y < image.height; ++y)
      for (unsigned x = 0; x < image.width; ++x)
        if (image.at(x, y)[0] == 20 && image.at(x, y)[1] == 20)
          image.at(x, y)[0] = image.at(x, y)[1] = image.at(x, y)[2] = static_cast<std::uint8_t>(background);
    auto m = gauge("VCockpit02", "EFIS", 0, 0, 0, 1644, 1024, 1644, 1024, kCell);
    std::strcpy(m.info.data(), "c=af66");
    const unsigned left = m.x + m.width - block_width(m) - kCell, top = m.y + kCell;
    require(block_width(m) == 36 && block_height(m) <= 26, "dark block is 36 px wide and at most 26 high");
    draw(image, m, left, top, kLevel);
    Mark found[2];
    char message[96];
    std::snprintf(message, sizeof(message), "dark 2 px block found on a %u background", background);
    require(find(image, found, 2) == 1 && std::strcmp(found[0].name.data(), "VCockpit02") == 0 &&
                std::strcmp(found[0].info.data(), "c=af66") == 0 && found[0].left == left + kCell && found[0].top == top + kCell,
            message);
    require(std::fabs(found[0].pitch_x - 2) < 0.01 && std::fabs(found[0].pitch_y - 2) < 0.01, "dark block pitch is 2 px");
  }
  // A transfer curve (an sRGB display texture encoding the level again)
  // raises the level; 0 stays 0. BGRA too.
  {
    auto image = page(512, 512, 8);
    const auto m = gauge("VCockpit07", "ISIS", 0, 0, 0, 512, 512, 512, 512, kCell);
    draw(image, m, 512 - block_width(m) - kCell, kCell, 70);
    for (unsigned y = 0; y < image.height; ++y)
      for (unsigned x = 0; x < image.width; ++x)
        std::swap(image.at(x, y)[0], image.at(x, y)[2]);
    Mark found[2];
    require(find(image, found, 2, true) == 1 && std::strcmp(found[0].gauge_name.data(), "ISIS") == 0, "raised level decodes in BGRA");
  }
  // A dark block with one cell changed fails the CRC.
  {
    auto image = page(512, 512, 9);
    const auto m = gauge("VCockpit07", "ISIS", 0, 0, 0, 512, 512, 512, 512, kCell);
    const unsigned left = 512 - block_width(m) - kCell, top = kCell;
    draw(image, m, left, top, kLevel);
    for (unsigned y = 0; y < kCell; ++y)
      for (unsigned x = 0; x < kCell; ++x)
        image.at(left + kCell * 7 + x, top + kCell * 3 + y)[1] ^= kLevel;
    Mark found[2];
    require(find(image, found, 2) == 0, "damaged dark block refused");
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
