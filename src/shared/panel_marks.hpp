#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace taxi_camera::panel_marks {
// Panel marks: a helper loaded into every cockpit panel page (taxi-cam-marks.js,
// see claudedocs/panel-mark-identification.md) paints, when the panel loads,
// one small block per gauge that names the panel and the gauge, until Taxi Cam
// has read it. Reading a display texture once then tells which panel it is and
// where each gauge sits, whatever order the simulator created the textures in.
//
// A block is a grid of square cells, 16 wide, each one of 8 colours whose
// channels are each 0 or the helper's level (3 bits: red 4, green 2, blue 1).
// The helper paints near-black (kLevel) at one texture pixel per page pixel so
// the block is hard to see; a transfer curve keeps 0 at 0 and only raises the
// level, and older bright (255) blocks still read. Row 0 is a fixed finder;
// column 0 of every later row alternates 0 and 7 (timing); the other 15 cells
// of each later row carry the payload, most significant bit first, followed by
// a CRC-16. The helper draws a one-cell 0 border around the block.
inline constexpr unsigned kColumns = 16, kDataColumns = kColumns - 1, kBitsPerCell = 3, kRowBits = kDataColumns * kBitsPerCell;
inline constexpr std::uint8_t kVersion = 1;
// The helper's channel value for a 1 bit, and its cell edge in page pixels.
inline constexpr std::uint8_t kLevel = 16;
inline constexpr unsigned kCell = 2;
inline constexpr std::array<std::uint8_t, kColumns> kFinder{7, 0, 7, 0, 4, 2, 1, 6, 3, 5, 0, 7, 4, 0, 1, 7};
// Payload bytes before the CRC: version, length of the rest, fields.
inline constexpr std::size_t kMaxPayload = 160;
inline constexpr unsigned kMaxRows = 1 + ((kMaxPayload + 2) * 8 + kRowBits - 1) / kRowBits;

enum class Kind : std::uint8_t { gauge = 0, panel = 1 };
struct Mark {
  Kind kind = Kind::gauge;
  std::uint8_t cell{};   // Cell edge in page pixels.
  std::uint8_t gauge{};  // htmlgaugeNN order in the panel.
  // Gauge rectangle and panel size in page pixels.
  std::uint16_t x{}, y{}, width{}, height{}, panel_width{}, panel_height{};
  std::array<char, 48> name{}, gauge_name{};
  std::array<char, 112> info{};
  // Where the block was found, in texture pixels.
  unsigned left{}, top{}, rows{};
  double pitch_x{}, pitch_y{};
};

inline std::uint16_t crc16(const std::uint8_t* bytes, std::size_t size) noexcept {
  std::uint16_t crc = 0xffff;  // CRC-16/CCITT-FALSE.
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= static_cast<std::uint16_t>(bytes[i] << 8);
    for (int bit = 0; bit < 8; ++bit)
      crc = static_cast<std::uint16_t>(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
  }
  return crc;
}
inline unsigned rows_for(std::size_t payload) noexcept {
  return 1 + static_cast<unsigned>(((payload + 2) * 8 + kRowBits - 1) / kRowBits);
}

// Encoder: the same bytes and cells as taxi-cam-marks.js, for tests.
inline std::vector<std::uint8_t> encode_payload(const Mark& m) {
  std::vector<std::uint8_t> out{kVersion, 0, static_cast<std::uint8_t>(m.kind), m.cell, m.gauge};
  for (const auto v : {m.x, m.y, m.width, m.height, m.panel_width, m.panel_height}) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
  }
  const auto text = [&](const char* s, std::size_t limit) {
    std::size_t n = std::strlen(s);
    if (n > limit)
      n = limit;
    out.push_back(static_cast<std::uint8_t>(n));
    out.insert(out.end(), s, s + n);
  };
  text(m.name.data(), m.name.size() - 1);
  text(m.gauge_name.data(), m.gauge_name.size() - 1);
  // The info text gives way first.
  const std::size_t room = kMaxPayload > out.size() + 1 ? kMaxPayload - out.size() - 1 : 0;
  text(m.info.data(), room < m.info.size() - 1 ? room : m.info.size() - 1);
  out[1] = static_cast<std::uint8_t>(out.size() - 2);
  return out;
}
// Cell colours, row-major, kColumns per row.
inline std::vector<std::uint8_t> encode_cells(const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> bytes = payload;
  const auto crc = crc16(payload.data(), payload.size());
  bytes.push_back(static_cast<std::uint8_t>(crc >> 8));
  bytes.push_back(static_cast<std::uint8_t>(crc));
  const unsigned rows = rows_for(payload.size());
  std::vector<std::uint8_t> cells(std::size_t{rows} * kColumns);
  for (unsigned c = 0; c < kColumns; ++c)
    cells[c] = kFinder[c];
  std::size_t bit = 0;
  for (unsigned r = 1; r < rows; ++r) {
    cells[std::size_t{r} * kColumns] = r & 1 ? 0 : 7;
    for (unsigned c = 1; c < kColumns; ++c) {
      unsigned v = 0;
      for (unsigned k = 0; k < kBitsPerCell; ++k, ++bit)
        v = v << 1 | (bit / 8 < bytes.size() ? (bytes[bit / 8] >> (7 - bit % 8)) & 1u : 0u);
      cells[std::size_t{r} * kColumns + c] = static_cast<std::uint8_t>(v);
    }
  }
  return cells;
}

// A gauge rectangle in texture pixels: page pixels scaled by the texture's
// size over the panel's. False when the mark gives no panel size.
struct Rect {
  unsigned x{}, y{}, width{}, height{};
};
inline bool texture_rect(const Mark& m, unsigned texture_width, unsigned texture_height, Rect& out) noexcept {
  if (!m.panel_width || !m.panel_height)
    return false;
  const auto scale = [](unsigned v, unsigned to, unsigned from) {
    return static_cast<unsigned>((std::uint64_t{v} * to + from / 2) / from);
  };
  out = {scale(m.x, texture_width, m.panel_width), scale(m.y, texture_height, m.panel_height), scale(m.width, texture_width, m.panel_width),
         scale(m.height, texture_height, m.panel_height)};
  return true;
}

class Decoder {
 public:
  Decoder(const std::uint8_t* pixels, std::uint64_t row_pitch, unsigned width, unsigned height, bool bgra) noexcept
      : pixels_(pixels), pitch_(row_pitch), width_(width), height_(height), red_(bgra ? 2 : 0), blue_(bgra ? 0 : 2) {}
  // Finds up to max blocks. Scans every second row: a cell is at least 2
  // texture pixels high.
  std::size_t find(Mark* out, std::size_t max) const noexcept {
    std::size_t found = 0;
    if (!pixels_ || width_ < kColumns * 2 || height_ < 4)
      return 0;
    for (unsigned y = 1; y < height_ && found < max; y += 2)
      for (unsigned x = 0; x < width_ && found < max; ++x) {
        bool inside = false;
        for (std::size_t i = 0; i < found && !inside; ++i)
          if (x >= out[i].left && x < out[i].left + static_cast<unsigned>(kColumns * out[i].pitch_x) + 1 && y + 1 >= out[i].top &&
              y <= out[i].top + static_cast<unsigned>(out[i].rows * out[i].pitch_y) + 1) {
            x = out[i].left + static_cast<unsigned>(kColumns * out[i].pitch_x) + 1;
            inside = true;
          }
        if (inside || x >= width_ || cls(x, y) != kFinder[0] || (x && cls(x - 1, y) == kFinder[0]))
          continue;
        if (decode_at(x, y, out[found]))
          ++found;
      }
    return found;
  }

 private:
  int cls(unsigned x, unsigned y) const noexcept {
    const auto* p = pixels_ + y * pitch_ + std::size_t{x} * 4;
    // 0 stays 0 under any transfer curve; the level only rises.
    const auto bit = [](std::uint8_t v) { return v >= kLevel / 2 ? 1 : v <= kLevel / 8 ? 0 : -1; };
    const int r = bit(p[red_]), g = bit(p[1]), b = bit(p[blue_]);
    return r < 0 || g < 0 || b < 0 ? -1 : r << 2 | g << 1 | b;
  }
  // Walks runs of the expected classes from start along x (vertical false) or
  // y, allowing up to 2 unclassified pixels between runs. bounds[i] is where
  // run i starts (a blurred edge counts half to each side); bounds[count] is
  // where the last one ends. Returns how many runs matched.
  template <class Expected>
  unsigned runs(unsigned x, unsigned y, bool vertical, unsigned count, Expected expected, double* bounds) const noexcept {
    const unsigned limit = vertical ? height_ : width_;
    unsigned pos = vertical ? y : x;
    bounds[0] = pos;
    for (unsigned i = 0; i < count; ++i) {
      const auto at = [&](unsigned p) { return vertical ? cls(x, p) : cls(p, y); };
      unsigned end = pos;
      while (end < limit && at(end) == expected(i))
        ++end;
      if (end - pos < 2)
        return i;
      unsigned gap = 0;
      while (end + gap < limit && gap < 3 && at(end + gap) < 0)
        ++gap;
      bounds[i + 1] = end + gap / 2.0;
      if (i + 1 < count && (gap > 2 || end + gap >= limit || at(end + gap) != expected(i + 1)))
        return i + 1;
      pos = end + gap;
    }
    return count;
  }
  // The majority class of 9 samples around a cell's centre, or -1.
  int sample(double cx, double cy, double px, double py) const noexcept {
    std::array<int, 8> votes{};
    for (int j = -1; j <= 1; ++j)
      for (int i = -1; i <= 1; ++i) {
        const double sx = cx + i * px / 4, sy = cy + j * py / 4;
        if (sx < 0 || sy < 0 || sx >= width_ || sy >= height_)
          continue;
        const int c = cls(static_cast<unsigned>(sx), static_cast<unsigned>(sy));
        if (c >= 0)
          ++votes[static_cast<std::size_t>(c)];
      }
    for (int c = 0; c < 8; ++c)
      if (votes[static_cast<std::size_t>(c)] >= 5)
        return c;
    return -1;
  }
  bool decode_at(unsigned x, unsigned y, Mark& mark) const noexcept {
    std::array<double, kColumns + 1> across{};
    if (runs(x, y, false, kColumns, [](unsigned i) { return kFinder[i]; }, across.data()) != kColumns)
      return false;
    const double px = (across[kColumns] - across[0]) / kColumns;
    if (px < kCell)
      return false;
    for (unsigned i = 0; i < kColumns; ++i) {
      const double run = across[i + 1] - across[i];
      if (run < px * 0.6 || run > px * 1.4)
        return false;
    }
    // Up column 0's centre to the top of the finder row, then down its
    // timing cells.
    const auto column = static_cast<unsigned>(across[0] + px / 2);
    unsigned top = y;
    while (top > 0 && cls(column, top - 1) == kFinder[0])
      --top;
    std::array<double, kMaxRows + 1> down{};
    const unsigned timing = runs(column, top, true, kMaxRows, [](unsigned r) { return r & 1 ? 0 : 7; }, down.data());
    if (timing < 2)
      return false;
    const double first = down[1] - down[0];
    if (first < px * 0.6 || first > px * 1.4)
      return false;
    std::array<std::uint8_t, kMaxPayload + 2> bytes{};
    std::size_t bit = 0;
    const auto read_row = [&](unsigned r, double py) {
      for (unsigned c = 1; c < kColumns; ++c) {
        const int v = sample(across[0] + (c + 0.5) * px, down[0] + (r + 0.5) * py, px, py);
        if (v < 0)
          return false;
        for (unsigned k = 0; k < kBitsPerCell; ++k, ++bit)
          if (bit / 8 < bytes.size() && (v >> (kBitsPerCell - 1 - k) & 1))
            bytes[bit / 8] |= static_cast<std::uint8_t>(0x80 >> (bit % 8));
      }
      return true;
    };
    if (!read_row(1, first) || bytes[0] != kVersion)
      return false;
    const std::size_t payload = std::size_t{bytes[1]} + 2;
    if (payload > kMaxPayload)
      return false;
    const unsigned rows = rows_for(payload);
    // The last row's timing cell may run into the black border, so its start
    // is the last boundary needed.
    if (timing < rows - 1)
      return false;
    const double py = (down[rows - 1] - down[0]) / (rows - 1);
    if (py < kCell || py < px * 0.6 || py > px * 1.6 || down[0] + rows * py > height_ + 1.0)
      return false;
    bytes = {};
    bit = 0;
    for (unsigned r = 1; r < rows; ++r)
      if (!read_row(r, py))
        return false;
    if (crc16(bytes.data(), payload) != (bytes[payload] << 8 | bytes[payload + 1]))
      return false;
    if (!parse(bytes.data(), payload, mark))
      return false;
    mark.left = static_cast<unsigned>(across[0]);
    mark.top = static_cast<unsigned>(down[0]);
    mark.rows = rows;
    mark.pitch_x = px;
    mark.pitch_y = py;
    return true;
  }
  static bool parse(const std::uint8_t* b, std::size_t size, Mark& m) noexcept {
    std::size_t at = 2;
    const auto u8 = [&](std::uint8_t& v) { return at < size ? (v = b[at++], true) : false; };
    const auto u16 = [&](std::uint16_t& v) {
      return at + 1 < size ? (v = static_cast<std::uint16_t>(b[at] << 8 | b[at + 1]), at += 2, true) : false;
    };
    const auto text = [&](auto& out) {
      std::uint8_t n = 0;
      if (!u8(n) || at + n > size || n >= out.size())
        return false;
      for (std::size_t i = 0; i < n; ++i)
        if (b[at + i] < 32 || b[at + i] > 126)
          return false;
      std::memcpy(out.data(), b + at, n);
      out[n] = 0;
      at += n;
      return true;
    };
    std::uint8_t kind = 0;
    m = {};
    return u8(kind) && kind <= 1 && (m.kind = static_cast<Kind>(kind), true) && u8(m.cell) && u8(m.gauge) && u16(m.x) && u16(m.y) &&
           u16(m.width) && u16(m.height) && u16(m.panel_width) && u16(m.panel_height) && text(m.name) && text(m.gauge_name) &&
           text(m.info) && at == size;
  }
  const std::uint8_t* pixels_;
  std::uint64_t pitch_;
  unsigned width_, height_, red_, blue_;
};
inline std::size_t find_marks(const std::uint8_t* pixels,
                              std::uint64_t row_pitch,
                              unsigned width,
                              unsigned height,
                              bool bgra,
                              Mark* out,
                              std::size_t max) noexcept {
  return Decoder(pixels, row_pitch, width, height, bgra).find(out, max);
}
}  // namespace taxi_camera::panel_marks
