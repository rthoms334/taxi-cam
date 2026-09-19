#include "../../src/camera/add_diffuse_slot.hpp"
#include "../../src/graphics/add_diffuse_histogram.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
using taxi_camera::engine_camera::AddDiffuseSlot;
using taxi_camera::engine_camera::MemoryReader;
std::uint32_t checks = 0;

void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

struct Reader final : MemoryReader {
  std::map<std::uint64_t, std::uint8_t> bytes;
  std::vector<std::uint64_t> addresses;

  bool read(std::uint64_t address, void* destination, std::size_t size) override {
    addresses.push_back(address);
    auto* output = static_cast<std::uint8_t*>(destination);
    for (std::size_t index = 0; index < size; ++index) {
      const auto found = bytes.find(address + index);
      if (found == bytes.end())
        return false;
      output[index] = found->second;
    }
    return true;
  }

  void put(std::uint64_t address, std::uint64_t value, std::uint32_t size = 8) {
    for (std::uint32_t index = 0; index < size; ++index)
      bytes[address + index] = static_cast<std::uint8_t>(value >> (index * 8));
  }

  void handle(std::uint64_t address, std::uint64_t control, std::uint64_t payload) {
    put(address, control);
    put(address + 8, 7, 4);
    put(address + 12, 0, 4);
    put(control + 28, 7, 4);
    put(control, payload);
  }
};

constexpr std::uint64_t kView = 0x300000;
constexpr std::uint64_t kMaterialControl = 0xb00000;
constexpr std::uint64_t kMaterial = 0x600000;
constexpr std::uint64_t kBitmapControl = 0xb10000;
constexpr std::uint64_t kBitmap = 0x700000;
constexpr std::uint64_t kRecord = 0x800000;
constexpr std::uint64_t kWrapper = 0x900000;
constexpr std::uint64_t kResource = 0xa00000;

void descriptor(Reader& reader, std::uint32_t format, std::uint64_t width, std::uint32_t height) {
  std::uint8_t bytes[56]{};
  std::memcpy(bytes + 16, &width, 8);
  std::memcpy(bytes + 24, &height, 4);
  const std::uint16_t layers = 1;
  const std::uint16_t mips = 1;
  std::memcpy(bytes + 28, &layers, 2);
  std::memcpy(bytes + 30, &mips, 2);
  std::memcpy(bytes + 32, &format, 4);
  for (std::uint32_t offset = 0; offset < sizeof(bytes); ++offset)
    reader.bytes[kWrapper + 96 + offset] = bytes[offset];
}

Reader allocated() {
  Reader reader;
  reader.put(kView + 48, 1ull << 49);
  reader.handle(kView + 144, kMaterialControl, kMaterial);
  reader.handle(kMaterial + 664, kBitmapControl, kBitmap);
  reader.put(kBitmap + 40, 640, 4);
  reader.put(kBitmap + 44, 171, 4);
  reader.put(kBitmap + 88, kRecord);
  reader.put(kRecord + 16, kWrapper);
  descriptor(reader, 10, 640, 171);
  reader.put(kWrapper + 168, kResource);
  return reader;
}

void test_present() {
  auto reader = allocated();
  const auto slot = taxi_camera::engine_camera::inspect_add_diffuse_slot(reader, kView);
  require(slot.complete && slot.present && slot.resource_present, "allocated slot 9 is present");
  require(slot.format == 10 && slot.width == 640 && slot.height == 171 && slot.mips == 1, "slot 9 format and size");
  require(slot.bit49 && slot.resource_address == kResource, "bit 49 is observed and the resource member is kept");
  require(slot.error[0] == '\0', "a complete slot has no error");
  for (auto address : reader.addresses)
    require(address != kMaterial + 520, "slot 9 inspection does not read VIEWPORT_MATERIAL_DIFFUSE");
}

void test_absent() {
  Reader reader;
  reader.put(kView + 48, 0);
  reader.put(kView + 144, 0);
  reader.put(kView + 152, 0);
  const auto slot = taxi_camera::engine_camera::inspect_add_diffuse_slot(reader, kView);
  require(slot.complete && !slot.present && !slot.resource_present && !slot.bit49, "missing material is not slot 9");
}

void test_stale() {
  auto reader = allocated();
  reader.put(kBitmapControl + 28, 9, 4);
  const auto slot = taxi_camera::engine_camera::inspect_add_diffuse_slot(reader, kView);
  require(slot.complete && !slot.present && !slot.resource_present, "a stale slot 9 handle is absent");
}

void test_histogram() {
  std::uint16_t texel[4] = {0x3c00, 0x3c00, 0x3c00, 0x3c00};
  taxi_camera::add_diffuse::Histogram histogram;
  require(taxi_camera::add_diffuse::histogram_rgba16f(texel, 1, 1, 8, histogram), "float16 histogram decodes");
  require(histogram.samples == 1 && histogram.bright == 1 && histogram.max_luminance > 0.9f && histogram.max_luminance < 1.1f,
          "unit float16 luminance is the bright bucket");
  std::uint16_t dark[4] = {};
  require(taxi_camera::add_diffuse::histogram_rgba16f(dark, 1, 1, 8, histogram), "zero histogram decodes");
  require(histogram.dark == 1 && histogram.nonzero == 0 && histogram.max_luminance == 0, "a zero texel is dark");
  const std::uint32_t lamp = (15u << 6) | ((15u << 6) << 11) | ((18u << 5) << 22);
  require(taxi_camera::add_diffuse::histogram_r11g11b10(&lamp, 1, 1, 4, histogram), "packed float histogram decodes");
  require(histogram.samples == 1 && histogram.nonzero == 1 && histogram.max_luminance > 1.f, "packed float above one is nonzero");
}
}  // namespace

int main() {
  try {
    test_present();
    test_absent();
    test_stale();
    test_histogram();
    std::printf("add-diffuse slot checks passed: %u\n", checks);
    return checks > 4 ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "add-diffuse slot check failed: %s\n", error.what());
    return 1;
  }
}
