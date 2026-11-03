#include "../../src/camera/image_inventory.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace taxi_camera::discovery {
namespace {

constexpr std::uint32_t kMaximumImageSize = 0x80000000;
constexpr std::uint32_t kChunkSize = 32 * 1024;
constexpr std::uint32_t kReadable = 0x40000000;
constexpr std::uint32_t kWritable = 0x80000000;
constexpr std::uint32_t kExecutable = 0x20000000;
constexpr std::uint32_t kDiscardable = 0x02000000;

struct Section {
  std::string name;
  std::uint32_t rva;
  std::uint32_t size;
  std::uint32_t flags;

  bool inspectable() const { return (flags & kReadable) != 0 && (flags & (kWritable | kDiscardable)) == 0; }
};

bool data_before_code(const Section& a, const Section& b) {
  return (a.flags & kExecutable) < (b.flags & kExecutable);
}

bool within(std::uint32_t start, std::uint64_t size, std::uint32_t end) {
  return start <= end && size <= static_cast<std::uint64_t>(end - start);
}

std::uint16_t u16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8));
}

std::uint32_t u32(const std::uint8_t* bytes) {
  return bytes[0] | (static_cast<std::uint32_t>(bytes[1]) << 8) | (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

bool printable(std::uint8_t byte) {
  return byte >= 32 && byte <= 126;
}

std::string keyword(const std::string& text, bool is_export) {
  std::string normalized;
  normalized.reserve(text.size());
  for (const auto character : text) {
    const auto c = static_cast<unsigned char>(character);
    if (c >= 'A' && c <= 'Z') {
      normalized.push_back(static_cast<char>(c + ('a' - 'A')));
    } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      normalized.push_back(static_cast<char>(c));
    }
  }
  constexpr const char* keywords[] = {"cameratotexture", "offscreen",      "mirror",          "worldrender",
                                      "cameramanager",   "rendertarget",   "rendertotexture", "rendertexture",
                                      "camerascreen",    "cameradirector", "rendercamera",    "camerarender",
                                      "sceneview",       "viewfamily",     "secondaryview",   "externalview"};
  for (const auto* candidate : keywords) {
    if (normalized.find(candidate) != std::string::npos) {
      return candidate;
    }
  }
  if (is_export) {
    for (const auto* candidate : {"camera", "render"}) {
      if (normalized.find(candidate) != std::string::npos) {
        return candidate;
      }
    }
  }
  return {};
}

class Parser {
 public:
  Parser(ImageReader& reader, Limits limits) : reader_(reader), limits_(limits) {
    limits_.scan_bytes = std::min(limits_.scan_bytes, 64u * 1024 * 1024);
    limits_.metadata_bytes = std::min(limits_.metadata_bytes, 4u * 1024 * 1024);
    limits_.records = std::min(limits_.records, 512u);
    limits_.export_names = std::min(limits_.export_names, 16384u);
    limits_.string_bytes = std::min(limits_.string_bytes, 512u);
  }

  Inventory run() {
    if (!headers()) {
      return std::move(result_);
    }
    result_.valid_image = true;
    exports();
    std::stable_sort(sections_.begin(), sections_.end(), data_before_code);
    for (const auto& section : sections_) {
      if (section.inspectable()) {
        scan(section);
      }
    }
    return std::move(result_);
  }

 private:
  bool metadata(std::uint32_t rva, void* output, std::uint32_t size) {
    if (size > limits_.metadata_bytes - result_.metadata_bytes) {
      result_.exports_truncated = true;
      return false;
    }
    result_.metadata_bytes += size;
    if (!reader_.read(rva, output, size)) {
      ++result_.read_failures;
      return false;
    }
    return true;
  }

  bool fail(const char* message) {
    result_.error = message;
    return false;
  }

  bool headers() {
    std::array<std::uint8_t, 64> dos{};
    if (!metadata(0, dos.data(), static_cast<std::uint32_t>(dos.size()))) {
      return fail("DOS header is unreadable or exceeds the metadata budget.");
    }
    const auto pe = u32(dos.data() + 60);
    if (u16(dos.data()) != 0x5a4d || pe < dos.size() || pe > 1024 * 1024) {
      return fail("Invalid DOS signature or PE header offset.");
    }
    std::array<std::uint8_t, 24> header{};
    if (!metadata(pe, header.data(), static_cast<std::uint32_t>(header.size()))) {
      return fail("PE header is unreadable.");
    }
    if (u32(header.data()) != 0x00004550 || u16(header.data() + 4) != 0x8664) {
      return fail("Expected an AMD64 PE image.");
    }
    result_.machine = u16(header.data() + 4);
    result_.section_count = u16(header.data() + 6);
    result_.timestamp = u32(header.data() + 8);
    const auto optional_size = u16(header.data() + 20);
    if (result_.section_count == 0 || result_.section_count > 96 || optional_size < 112 || optional_size > 4096) {
      return fail("Invalid section count or optional-header size.");
    }
    std::vector<std::uint8_t> optional(optional_size);
    if (!metadata(pe + 24, optional.data(), optional_size) || u16(optional.data()) != 0x20b) {
      return fail("Expected a readable PE32+ optional header.");
    }
    result_.image_size = u32(optional.data() + 56);
    headers_size_ = u32(optional.data() + 60);
    result_.checksum = u32(optional.data() + 64);
    result_.dll_characteristics = u16(optional.data() + 70);
    const auto section_table = pe + 24 + optional_size;
    const auto section_bytes = static_cast<std::uint32_t>(result_.section_count) * 40;
    if (result_.image_size == 0 || result_.image_size > kMaximumImageSize || headers_size_ == 0 || headers_size_ > result_.image_size ||
        !within(section_table, section_bytes, headers_size_)) {
      return fail("Image, header or section-table bounds are invalid.");
    }
    const auto directory_count = u32(optional.data() + 108);
    if (directory_count > (optional_size - 112u) / 8u) {
      return fail("The declared data directories exceed the optional header.");
    }
    if (directory_count > 0) {
      if (optional_size < 120) {
        return fail("Export data-directory entry is truncated.");
      }
      export_rva_ = u32(optional.data() + 112);
      export_size_ = u32(optional.data() + 116);
    }
    if (directory_count > 3) {
      result_.exception_rva = u32(optional.data() + 112 + 3 * 8);
      result_.exception_size = u32(optional.data() + 116 + 3 * 8);
    }
    if (directory_count > 10) {
      result_.load_config_rva = u32(optional.data() + 112 + 10 * 8);
      result_.load_config_size = u32(optional.data() + 116 + 10 * 8);
    }
    std::vector<std::uint8_t> table(section_bytes);
    if (!metadata(section_table, table.data(), section_bytes)) {
      return fail("Section headers are unreadable.");
    }
    for (std::uint32_t i = 0; i < result_.section_count; ++i) {
      const auto* section = table.data() + i * 40;
      std::string name;
      for (unsigned int j = 0; j < 8 && section[j] != 0; ++j) {
        name.push_back(printable(section[j]) ? static_cast<char>(section[j]) : '?');
      }
      const auto virtual_size = u32(section + 8);
      const auto size = virtual_size != 0 ? virtual_size : u32(section + 16);
      const auto rva = u32(section + 12);
      if (size != 0 && (rva < headers_size_ || !within(rva, size, result_.image_size))) {
        return fail("A section extends outside the loaded image.");
      }
      if (size != 0) {
        sections_.push_back({name, rva, size, u32(section + 36)});
      }
    }
    std::sort(sections_.begin(), sections_.end(), [](const Section& a, const Section& b) { return a.rva < b.rva; });
    for (std::size_t i = 1; i < sections_.size(); ++i) {
      if (sections_[i].rva < sections_[i - 1].rva + sections_[i - 1].size) {
        return fail("Overlapping loaded sections are unsupported.");
      }
    }
    for (const auto& section : sections_) {
      result_.sections.push_back({section.name, section.rva, section.size, section.flags});
    }
    return true;
  }

  const Section* containing(std::uint32_t rva, std::uint64_t size) const {
    for (const auto& section : sections_) {
      if (section.inspectable() && rva >= section.rva && within(rva, size, section.rva + section.size)) {
        return &section;
      }
    }
    return nullptr;
  }

  bool literal(std::uint32_t rva, std::string& text) {
    const auto* section = containing(rva, 1);
    if (section == nullptr) {
      return false;
    }
    std::array<std::uint8_t, 64> buffer{};
    std::uint32_t offset = rva;
    while (text.size() <= limits_.string_bytes && offset < section->rva + section->size) {
      const auto remaining = std::min<std::uint32_t>(section->rva + section->size - offset,
                                                     limits_.string_bytes + 1 - static_cast<std::uint32_t>(text.size()));
      const auto window = reader_.query(offset, std::min<std::uint32_t>(remaining, buffer.size()));
      if (!window.readable || window.size == 0 || window.size > remaining || window.size > buffer.size() ||
          !metadata(offset, buffer.data(), window.size)) {
        return false;
      }
      for (std::uint32_t i = 0; i < window.size; ++i) {
        if (buffer[i] == 0) {
          return !text.empty();
        }
        if (!printable(buffer[i]) || text.size() == limits_.string_bytes) {
          return false;
        }
        text.push_back(static_cast<char>(buffer[i]));
      }
      offset += window.size;
    }
    return false;
  }

  void add(Record record) {
    if (result_.records.size() >= limits_.records) {
      result_.record_limit_reached = true;
      return;
    }
    for (const auto& previous : result_.records) {
      if (previous.rva == record.rva && previous.text == record.text) {
        return;
      }
    }
    result_.records.push_back(std::move(record));
  }

  void exports() {
    if (export_rva_ == 0 && export_size_ == 0) {
      return;
    }
    if (export_size_ < 40 || !within(export_rva_, export_size_, result_.image_size) || containing(export_rva_, 40) == nullptr) {
      ++result_.malformed_exports;
      return;
    }
    std::array<std::uint8_t, 40> directory{};
    if (!metadata(export_rva_, directory.data(), directory.size())) {
      return;
    }
    const auto ordinal_base = u32(directory.data() + 16);
    const auto functions = u32(directory.data() + 20);
    const auto names = u32(directory.data() + 24);
    const auto function_table = u32(directory.data() + 28);
    const auto name_table = u32(directory.data() + 32);
    const auto ordinal_table = u32(directory.data() + 36);
    if (names == 0) {
      return;
    }
    if (functions == 0 || containing(function_table, static_cast<std::uint64_t>(functions) * 4) == nullptr ||
        containing(name_table, static_cast<std::uint64_t>(names) * 4) == nullptr ||
        containing(ordinal_table, static_cast<std::uint64_t>(names) * 2) == nullptr) {
      ++result_.malformed_exports;
      return;
    }
    const auto count = std::min(names, limits_.export_names);
    result_.exports_truncated = names > count;
    std::vector<std::uint8_t> name_bytes(count * 4);
    std::vector<std::uint8_t> ordinal_bytes(count * 2);
    if (!metadata(name_table, name_bytes.data(), count * 4) || !metadata(ordinal_table, ordinal_bytes.data(), count * 2)) {
      return;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
      if (result_.records.size() >= limits_.records) {
        result_.record_limit_reached = true;
        result_.exports_truncated = true;
        break;
      }
      const auto name_rva = u32(name_bytes.data() + i * 4);
      const auto ordinal_index = u16(ordinal_bytes.data() + i * 2);
      if (ordinal_index >= functions || ordinal_index > std::numeric_limits<std::uint32_t>::max() - ordinal_base) {
        ++result_.malformed_exports;
        continue;
      }
      std::string name;
      if (!literal(name_rva, name)) {
        ++result_.malformed_exports;
        continue;
      }
      const auto match = keyword(name, true);
      if (match.empty()) {
        continue;
      }
      std::array<std::uint8_t, 4> function{};
      if (!metadata(function_table + ordinal_index * 4, function.data(), function.size())) {
        continue;
      }
      const auto function_rva = u32(function.data());
      if (function_rva == 0 || !within(function_rva, 1, result_.image_size)) {
        ++result_.malformed_exports;
        continue;
      }
      const bool forwarder = function_rva >= export_rva_ && function_rva < export_rva_ + export_size_;
      if (forwarder) {
        std::string forwarder_name;
        if (!literal(function_rva, forwarder_name) || !within(function_rva, forwarder_name.size() + 1, export_rva_ + export_size_)) {
          ++result_.malformed_exports;
          continue;
        }
      }
      const auto* section = containing(name_rva, name.size() + 1);
      add({forwarder ? RecordKind::forwarded_export : RecordKind::export_name, name, match, section ? section->name : "", name_rva,
           function_rva, ordinal_base + ordinal_index});
    }
  }

  void scan(const Section& section) {
    std::array<std::uint8_t, kChunkSize> buffer{};
    std::string run;
    run.reserve(limits_.string_bytes);
    std::uint32_t run_rva = 0;
    bool overlong = false;
    std::uint32_t offset = section.rva;
    while (offset < section.rva + section.size) {
      const auto examined_bytes = result_.scanned_bytes + result_.skipped_bytes;
      if (examined_bytes >= limits_.scan_bytes || result_.records.size() >= limits_.records) {
        result_.scan_truncated = true;
        result_.record_limit_reached |= result_.records.size() >= limits_.records;
        return;
      }
      const auto remaining = section.rva + section.size - offset;
      const auto maximum =
          std::min<std::uint32_t>({remaining, kChunkSize, limits_.scan_bytes - static_cast<std::uint32_t>(examined_bytes)});
      const auto window = reader_.query(offset, maximum);
      if (window.size == 0 || window.size > maximum) {
        ++result_.read_failures;
        result_.skipped_bytes += maximum;
        result_.scan_truncated = true;
        return;
      }
      if (!window.readable || !reader_.read(offset, buffer.data(), window.size)) {
        result_.read_failures += window.readable ? 1 : 0;
        result_.skipped_bytes += window.size;
        run.clear();
        overlong = false;
        offset += window.size;
        continue;
      }
      result_.scanned_bytes += window.size;
      for (std::uint32_t i = 0; i < window.size; ++i) {
        if (printable(buffer[i])) {
          if (run.empty() && !overlong) {
            run_rva = offset + i;
          }
          if (run.size() < limits_.string_bytes) {
            run.push_back(static_cast<char>(buffer[i]));
          } else {
            overlong = true;
          }
        } else {
          if (buffer[i] == 0 && !overlong && run.size() >= 6) {
            const auto match = keyword(run, false);
            if (!match.empty()) {
              add({RecordKind::ascii_literal, run, match, section.name, run_rva, 0, 0});
            }
          }
          run.clear();
          overlong = false;
        }
      }
      offset += window.size;
    }
  }

  ImageReader& reader_;
  Limits limits_;
  Inventory result_;
  std::vector<Section> sections_;
  std::uint32_t headers_size_ = 0;
  std::uint32_t export_rva_ = 0;
  std::uint32_t export_size_ = 0;
};

}

Inventory inspect_image(ImageReader& reader, Limits limits) {
  return Parser(reader, limits).run();
}

}
