#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace rtpt
{

// ImageRgba8
// A decoded 8-bit image, always expanded to four channels so every consumer can treat it as RGBA.

struct ImageRgba8
{
  // Width in pixels.
  uint32_t width = 0;

  // Height in pixels.
  uint32_t height = 0;

  // Row-major RGBA bytes, width * height * 4 values.
  std::vector<uint8_t> pixels;
};

// ImageRgba32f
// A decoded floating-point image, always expanded to four channels. Used for HDR environment maps and linear output.

struct ImageRgba32f
{
  // Width in pixels.
  uint32_t width = 0;

  // Height in pixels.
  uint32_t height = 0;

  // Row-major RGBA values, width * height * 4 entries.
  std::vector<float> pixels;
};

// Decodes any stb-supported image to 8-bit RGBA. Returns nothing when the file cannot be read or decoded.
[[nodiscard]] std::optional<ImageRgba8> LoadImageRgba8(const std::filesystem::path& path);

// Decodes an image to float RGBA. HDR files keep their values; LDR files are scaled to [0, 1] without sRGB decoding.
[[nodiscard]] std::optional<ImageRgba32f> LoadImageRgba32f(const std::filesystem::path& path);

// Encodes RGBA8 pixels as PNG, creating the parent directory if needed. Throws on invalid input or write failure.
void WritePng(const std::filesystem::path& path, uint32_t width, uint32_t height, std::span<const uint8_t> rgbaPixels);

// Encodes float RGBA pixels as Radiance HDR, creating the parent directory if needed. Throws on invalid input or write failure.
void WriteHdr(const std::filesystem::path& path, uint32_t width, uint32_t height, std::span<const float> rgbaPixels);

}  // namespace rtpt
