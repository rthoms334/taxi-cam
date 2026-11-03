#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace taxi_camera::standalone {
// The bug report's ZIP. Each file is one fixed-Huffman deflate block (RFC 1951
// 3.2.6), or stored when that is not smaller. No ZIP64: the bundle reads at
// most a few tens of MiB, far below 4 GiB and 65535 entries.
struct ZipEntry {
  std::string name;  // ASCII, '/' separated.
  std::string data;
};

inline std::uint32_t zip_crc32(std::string_view data) {
  static const auto table = [] {
    std::array<std::uint32_t, 256> result{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      result[i] = c;
    }
    return result;
  }();
  std::uint32_t c = 0xFFFFFFFFu;
  for (const unsigned char b : data)
    c = table[(c ^ b) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

namespace deflate_detail {
inline constexpr std::array<std::uint16_t, 29> LengthBase{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
inline constexpr std::array<std::uint8_t, 29> LengthExtra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                          2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
inline constexpr std::array<std::uint16_t, 30> DistanceBase{1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                                            33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                                            1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
inline constexpr std::array<std::uint8_t, 30> DistanceExtra{0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                            6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Deflate packs bits from the least significant end; Huffman codes go most
// significant bit first.
struct BitWriter {
  std::string out;
  std::uint64_t buffer{};
  unsigned count{};
  void put(std::uint32_t value, unsigned bits) {
    buffer |= static_cast<std::uint64_t>(value) << count;
    count += bits;
    while (count >= 8) {
      out += static_cast<char>(buffer & 0xFF);
      buffer >>= 8;
      count -= 8;
    }
  }
  void code(std::uint32_t value, unsigned bits) {
    std::uint32_t reversed = 0;
    for (unsigned i = 0; i < bits; ++i)
      reversed |= ((value >> i) & 1u) << (bits - 1 - i);
    put(reversed, bits);
  }
  void symbol(unsigned value) {
    if (value < 144)
      code(0x30 + value, 8);
    else if (value < 256)
      code(0x190 + value - 144, 9);
    else if (value < 280)
      code(value - 256, 7);
    else
      code(0xC0 + value - 280, 8);
  }
  void finish() {
    if (count)
      out += static_cast<char>(buffer & 0xFF);
    buffer = count = 0;
  }
};
}  // namespace deflate_detail

// Raw deflate stream: greedy LZ77 over a 32 KiB window with bounded hash
// chains. Logs repeat long line prefixes, so fixed codes compress them well.
inline std::string deflate_fixed(std::string_view input) {
  using namespace deflate_detail;
  constexpr std::size_t Window = 32768, MinMatch = 3, MaxMatch = 258;
  constexpr unsigned HashBits = 15, MaxChain = 48;
  std::vector<std::int64_t> head(std::size_t{1} << HashBits, -1), previous(Window, -1);
  const auto byte = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(input[i])); };
  const auto hash = [&](std::size_t i) { return ((byte(i) | byte(i + 1) << 8 | byte(i + 2) << 16) * 2654435761u) >> (32 - HashBits); };
  const auto insert = [&](std::size_t i) {
    if (i + MinMatch > input.size())
      return;
    const auto h = hash(i);
    previous[i & (Window - 1)] = head[h];
    head[h] = static_cast<std::int64_t>(i);
  };
  BitWriter w;
  w.put(1, 1);  // Final block.
  w.put(1, 2);  // Fixed Huffman codes.
  for (std::size_t i = 0; i < input.size();) {
    std::size_t best_length = 0, best_distance = 0;
    if (i + MinMatch <= input.size()) {
      const std::size_t limit = std::min(MaxMatch, input.size() - i);
      std::int64_t candidate = head[hash(i)];
      // A chain entry is still its own while within the window: the slot is
      // reused only by the position one window later, not yet inserted.
      for (unsigned chain = 0; candidate >= 0 && chain < MaxChain; ++chain) {
        const auto c = static_cast<std::size_t>(candidate);
        if (i - c > Window)
          break;
        if (input[c + best_length] == input[i + best_length] || !best_length) {
          std::size_t length = 0;
          while (length < limit && input[c + length] == input[i + length])
            ++length;
          if (length > best_length) {
            best_length = length;
            best_distance = i - c;
            if (length == limit)
              break;
          }
        }
        candidate = previous[c & (Window - 1)];
      }
    }
    if (best_length >= MinMatch) {
      std::size_t l = LengthBase.size() - 1;
      while (LengthBase[l] > best_length)
        --l;
      w.symbol(257 + static_cast<unsigned>(l));
      w.put(static_cast<std::uint32_t>(best_length - LengthBase[l]), LengthExtra[l]);
      std::size_t d = DistanceBase.size() - 1;
      while (DistanceBase[d] > best_distance)
        --d;
      w.code(static_cast<std::uint32_t>(d), 5);
      w.put(static_cast<std::uint32_t>(best_distance - DistanceBase[d]), DistanceExtra[d]);
      for (std::size_t k = 0; k < best_length; ++k)
        insert(i + k);
      i += best_length;
    } else {
      w.symbol(byte(i));
      insert(i);
      ++i;
    }
  }
  w.symbol(256);
  w.finish();
  return std::move(w.out);
}

// MS-DOS date and time fields of a local wall-clock time, as ZIP stores them.
inline std::uint16_t zip_dos_time(unsigned hour, unsigned minute, unsigned second) {
  return static_cast<std::uint16_t>(hour << 11 | minute << 5 | second / 2);
}
inline std::uint16_t zip_dos_date(unsigned year, unsigned month, unsigned day) {
  return static_cast<std::uint16_t>((year < 1980 ? 0 : year - 1980) << 9 | month << 5 | day);
}

// Empty when the archive would need ZIP64.
inline std::string zip_archive(const std::vector<ZipEntry>& entries, std::uint16_t dos_time, std::uint16_t dos_date) {
  std::string out, directory;
  const auto u16 = [](std::string& s, std::uint64_t v) {
    s += static_cast<char>(v & 0xFF);
    s += static_cast<char>(v >> 8 & 0xFF);
  };
  const auto u32 = [&](std::string& s, std::uint64_t v) {
    u16(s, v & 0xFFFF);
    u16(s, v >> 16 & 0xFFFF);
  };
  if (entries.size() > 0xFFFF)
    return {};
  for (const auto& entry : entries) {
    if (entry.name.size() > 0xFFFF || entry.data.size() > 0xFFFFFFFFu)
      return {};
    const auto crc = zip_crc32(entry.data);
    auto packed = deflate_fixed(entry.data);
    const bool deflated = packed.size() < entry.data.size();
    const std::string_view body = deflated ? std::string_view(packed) : std::string_view(entry.data);
    const std::uint64_t offset = out.size();
    const auto common = [&](std::string& s) {
      u16(s, 20);  // Version needed: deflate.
      u16(s, 0);   // Flags.
      u16(s, deflated ? 8 : 0);
      u16(s, dos_time);
      u16(s, dos_date);
      u32(s, crc);
      u32(s, body.size());
      u32(s, entry.data.size());
      u16(s, entry.name.size());
      u16(s, 0);  // Extra field length.
    };
    u32(out, 0x04034B50);
    common(out);
    out += entry.name;
    out += body;
    u32(directory, 0x02014B50);
    u16(directory, 20);  // Made by: MS-DOS attributes, ZIP 2.0.
    common(directory);
    u16(directory, 0);  // Comment length.
    u16(directory, 0);  // Disk number.
    u16(directory, 0);  // Internal attributes.
    u32(directory, 0);  // External attributes.
    u32(directory, offset);
    directory += entry.name;
    if (out.size() > 0xFFFFFFFFu)
      return {};
  }
  const std::uint64_t directory_offset = out.size();
  out += directory;
  u32(out, 0x06054B50);
  u16(out, 0);
  u16(out, 0);
  u16(out, entries.size());
  u16(out, entries.size());
  u32(out, directory.size());
  u32(out, directory_offset);
  u16(out, 0);  // Comment length.
  return out.size() > 0xFFFFFFFFu ? std::string{} : out;
}

// A private value in shared text: replaced wherever it appears, ignoring ASCII
// case. A whole-word value matches only between non-word characters, so a
// short account name does not rewrite parts of other words.
struct Redaction {
  std::string value, replacement;
  bool whole_word{};
};

inline std::string redact(std::string text, const std::vector<Redaction>& rules) {
  const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + 32) : c; };
  const auto word = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
  };
  for (const auto& rule : rules) {
    if (rule.value.empty())
      continue;
    std::string result;
    result.reserve(text.size());
    const auto first = lower(static_cast<unsigned char>(rule.value[0]));
    std::size_t copied = 0;
    for (std::size_t i = 0; i + rule.value.size() <= text.size(); ++i) {
      if (lower(static_cast<unsigned char>(text[i])) != first)
        continue;
      std::size_t k = 1;
      while (k < rule.value.size() && lower(static_cast<unsigned char>(text[i + k])) == lower(static_cast<unsigned char>(rule.value[k])))
        ++k;
      if (k < rule.value.size())
        continue;
      if (rule.whole_word &&
          ((i && word(static_cast<unsigned char>(text[i - 1]))) || (i + k < text.size() && word(static_cast<unsigned char>(text[i + k])))))
        continue;
      result.append(text, copied, i - copied);
      result += rule.replacement;
      copied = i + k;
      i = copied - 1;
    }
    result.append(text, copied, std::string::npos);
    text = std::move(result);
  }
  return text;
}

// The last `limit` bytes of a file another process may still be writing, such
// as bridge.log while MSFS runs. Empty when the file cannot be read.
inline std::string read_file_tail(const std::wstring& path, std::uint64_t limit, bool* truncated = nullptr) {
  if (truncated)
    *truncated = false;
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return {};
  LARGE_INTEGER size{};
  std::string data;
  if (GetFileSizeEx(file, &size) && size.QuadPart > 0) {
    const auto total = static_cast<std::uint64_t>(size.QuadPart);
    const auto wanted = std::min(total, limit);
    LARGE_INTEGER start{};
    start.QuadPart = static_cast<LONGLONG>(total - wanted);
    data.resize(static_cast<std::size_t>(wanted));
    DWORD read = 0;
    if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN) ||
        !ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr))
      read = 0;
    data.resize(read);
    if (truncated)
      *truncated = wanted < total;
  }
  CloseHandle(file);
  return data;
}
}  // namespace taxi_camera::standalone
