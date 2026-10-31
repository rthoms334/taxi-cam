#include "llvm_decoder.hpp"
#include "main_module_inventory.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {
using namespace taxi_camera::discovery;

std::string utf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  const auto size =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return "<invalid module name>";
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
  return result;
}

void json_string(const std::string& value) {
  std::cout << '"';
  for (const auto c : value) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      std::cout << '\\' << c;
    } else if (byte < 32) {
      char escaped[7];
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
      std::cout << escaped;
    } else {
      std::cout << c;
    }
  }
  std::cout << '"';
}

void report(const MainModuleInventory& result, std::int64_t elapsed_ms) {
  const auto& image = result.image;
  std::cout << std::boolalpha << "{\n  \"schema\": 1,\n  \"process_id\": " << result.process_id << ",\n  \"module_name\": ";
  json_string(utf8(result.module_name));
  std::cout << ",\n  \"candidate_module_name\": ";
  json_string(utf8(result.candidate_module_name));
  std::cout << ",\n  \"identity_method\": ";
  json_string(result.identity_method);
  std::cout << ",\n  \"main_module_verified\": " << result.main_module_verified << ",\n  \"valid_image\": " << image.valid_image
            << ",\n  \"error\": ";
  json_string(image.error);
  std::cout << ",\n  \"windows_error\": " << result.windows_error << ",\n  \"elapsed_ms\": " << elapsed_ms
            << ",\n  \"mapped_image_size\": " << result.mapped_image_size << ",\n  \"header_region_state\": " << result.header_region_state
            << ",\n  \"header_region_type\": " << result.header_region_type
            << ",\n  \"header_region_protection\": " << result.header_region_protection
            << ",\n  \"header_allocation_matches\": " << result.header_allocation_matches
            << ",\n  \"header_query_error\": " << result.header_query_error
            << ",\n  \"memory_read_attempted\": " << result.memory_read_attempted
            << ",\n  \"last_memory_read_error\": " << result.last_memory_read_error << ",\n  \"machine\": " << image.machine
            << ",\n  \"timestamp_raw\": " << image.timestamp << ",\n  \"image_size\": " << image.image_size
            << ",\n  \"checksum\": " << image.checksum << ",\n  \"section_count\": " << image.section_count
            << ",\n  \"scanned_bytes\": " << image.scanned_bytes << ",\n  \"metadata_bytes\": " << image.metadata_bytes
            << ",\n  \"skipped_bytes\": " << image.skipped_bytes << ",\n  \"read_failures\": " << image.read_failures
            << ",\n  \"malformed_exports\": " << image.malformed_exports << ",\n  \"scan_truncated\": " << image.scan_truncated
            << ",\n  \"exports_truncated\": " << image.exports_truncated << ",\n  \"record_limit_reached\": " << image.record_limit_reached
            << ",\n  \"records\": [";
  bool first = true;
  for (const auto& record : image.records) {
    if (!first) {
      std::cout << ',';
    }
    first = false;
    std::cout << "\n    {\"kind\": ";
    json_string(record.kind == RecordKind::export_name        ? "export_name"
                : record.kind == RecordKind::forwarded_export ? "forwarded_export"
                                                              : "ascii_literal");
    std::cout << ", \"rva\": " << record.rva << ", \"function_rva\": " << record.function_rva << ", \"ordinal\": " << record.ordinal
              << ", \"section\": ";
    json_string(record.section);
    std::cout << ", \"keyword\": ";
    json_string(record.keyword);
    std::cout << ", \"text\": ";
    json_string(record.text);
    std::cout << '}';
  }
  std::cout << "\n  ]\n}\n";
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--decoder-self-test") {
    std::string error;
    if (!llvm_decoder_self_test(error)) {
      std::fprintf(stderr, "FAIL: %s\n", error.c_str());
      return 1;
    }
    std::puts("PASS: pinned LLVM C decoder parsed synthetic RIP-relative LEA and RET instructions.");
    return 0;
  }
  bool self = argc == 1;
  std::uint32_t pid = 0;
  if (argc == 2 && std::string_view(argv[1]) == "--self") {
    self = true;
  } else if (argc == 3 && std::string_view(argv[1]) == "--pid") {
    const std::string_view input(argv[2]);
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), pid);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || pid == 0) {
      std::fprintf(stderr, "A nonzero decimal PID is required.\n");
      return 2;
    }
  } else if (argc != 1) {
    std::fprintf(stderr, "Usage: camera-interface-inventory.exe [--self | --pid <FlightSimulator2024 PID> | --decoder-self-test]\n");
    return 2;
  }
  try {
    const auto start = std::chrono::steady_clock::now();
    const auto result = self ? inspect_main_module(GetModuleHandleW(nullptr)) : inspect_simulator_main_module(pid);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    report(result, elapsed);
    return result.main_module_verified && result.image.valid_image ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Inventory failed: %s\n", error.what());
    return 1;
  }
}
