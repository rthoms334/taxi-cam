#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace taxi_camera::add_diffuse {

// Bounded CPU histogram of one captured colour image. Not a tone map and not
// evidence of what the engine intended to draw. Callers pass the readback
// layout they already copied; this function does not touch a device.
struct Histogram {
  std::uint32_t samples = 0;
  std::uint32_t nonzero = 0;
  std::uint32_t nonfinite = 0;
  std::uint32_t dark = 0;
  std::uint32_t dim = 0;
  std::uint32_t mid = 0;
  std::uint32_t bright = 0;
  std::uint32_t lamp = 0;
  std::uint32_t spike = 0;
  float max_luminance = 0;
};

inline constexpr std::uint32_t kHistogramSampleCap = 65536;

inline float half_to_float(std::uint16_t value) noexcept {
  const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000u) << 16;
  const std::uint32_t exponent = (value >> 10) & 0x1fu;
  const std::uint32_t mantissa = value & 0x3ffu;
  std::uint32_t bits = sign;
  if (exponent == 0) {
    if (mantissa != 0) {
      std::uint32_t significand = mantissa;
      std::uint32_t normalized = 127u - 15u + 1u;
      while ((significand & 0x400u) == 0) {
        significand <<= 1;
        --normalized;
      }
      bits |= (normalized << 23) | ((significand & 0x3ffu) << 13);
    }
  } else if (exponent == 31) {
    bits |= 0x7f800000u | (mantissa << 13);
  } else {
    bits |= ((exponent + (127u - 15u)) << 23) | (mantissa << 13);
  }
  float decoded = 0;
  std::memcpy(&decoded, &bits, sizeof(decoded));
  return decoded;
}

inline float tiny_float(std::uint32_t bits, int mantissa_bits) noexcept {
  const std::uint32_t mantissa_mask = (1u << mantissa_bits) - 1u;
  const std::uint32_t mantissa = bits & mantissa_mask;
  const std::uint32_t exponent = (bits >> mantissa_bits) & 0x1fu;
  if (exponent == 31)
    return mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
  if (exponent == 0)
    return mantissa ? static_cast<float>(mantissa) * 0x1p-24f * static_cast<float>(1u << (10 - mantissa_bits)) : 0;
  // exponent 0 subnormals above use a fixed scale. Finite normals:
  const float significand = 1.f + static_cast<float>(mantissa) / static_cast<float>(1u << mantissa_bits);
  int shift = static_cast<int>(exponent) - 15;
  if (shift >= 0)
    return significand * static_cast<float>(1u << shift);
  return significand / static_cast<float>(1u << -shift);
}

inline void accumulate(Histogram& histogram, float red, float green, float blue) noexcept {
  ++histogram.samples;
  if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue)) {
    ++histogram.nonfinite;
    return;
  }
  const float luminance = 0.2126f * (red > 0 ? red : 0) + 0.7152f * (green > 0 ? green : 0) + 0.0722f * (blue > 0 ? blue : 0);
  if (luminance > 0.001f)
    ++histogram.nonzero;
  if (luminance > histogram.max_luminance)
    histogram.max_luminance = luminance;
  if (luminance < 0.001f)
    ++histogram.dark;
  else if (luminance < 0.05f)
    ++histogram.dim;
  else if (luminance < 1.f)
    ++histogram.mid;
  else if (luminance < 16.f)
    ++histogram.bright;
  else if (luminance < 256.f)
    ++histogram.lamp;
  else
    ++histogram.spike;
}

inline std::uint32_t sample_stride(std::uint32_t width, std::uint32_t height) noexcept {
  const auto pixels = static_cast<std::uint64_t>(width) * height;
  if (pixels <= kHistogramSampleCap)
    return 1;
  auto stride = static_cast<std::uint32_t>((pixels + kHistogramSampleCap - 1) / kHistogramSampleCap);
  return stride == 0 ? 1 : stride;
}

inline bool histogram_rgba16f(const void* pixels,
                              std::uint32_t width,
                              std::uint32_t height,
                              std::uint32_t row_pitch,
                              Histogram& histogram) noexcept {
  histogram = {};
  if (!pixels || !width || !height || row_pitch < width * 8u)
    return false;
  const auto stride = sample_stride(width, height);
  const auto* rows = static_cast<const std::uint8_t*>(pixels);
  for (std::uint32_t y = 0; y < height; y += stride) {
    const auto* row = rows + static_cast<std::size_t>(y) * row_pitch;
    for (std::uint32_t x = 0; x < width; x += stride) {
      const auto* texel = row + static_cast<std::size_t>(x) * 8u;
      std::uint16_t channels[4]{};
      std::memcpy(channels, texel, sizeof(channels));
      accumulate(histogram, half_to_float(channels[0]), half_to_float(channels[1]), half_to_float(channels[2]));
    }
  }
  return histogram.samples != 0;
}

inline bool histogram_r11g11b10(const void* pixels,
                                std::uint32_t width,
                                std::uint32_t height,
                                std::uint32_t row_pitch,
                                Histogram& histogram) noexcept {
  histogram = {};
  if (!pixels || !width || !height || row_pitch < width * 4u)
    return false;
  const auto stride = sample_stride(width, height);
  const auto* rows = static_cast<const std::uint8_t*>(pixels);
  for (std::uint32_t y = 0; y < height; y += stride) {
    const auto* row = rows + static_cast<std::size_t>(y) * row_pitch;
    for (std::uint32_t x = 0; x < width; x += stride) {
      std::uint32_t packed = 0;
      std::memcpy(&packed, row + static_cast<std::size_t>(x) * 4u, sizeof(packed));
      accumulate(histogram, tiny_float(packed & 0x7ffu, 6), tiny_float((packed >> 11) & 0x7ffu, 6), tiny_float(packed >> 22, 5));
    }
  }
  return histogram.samples != 0;
}

}  // namespace taxi_camera::add_diffuse
