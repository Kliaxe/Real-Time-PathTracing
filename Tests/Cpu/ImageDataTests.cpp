#include "Assets/ImageData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>

int main()
{
  // Test files
  // The file names contain a non-ASCII character so the round trip also covers wide-character path handling.

  const std::filesystem::path directory = std::filesystem::temp_directory_path() / "rtpt-image-data-tests";
  const std::filesystem::path pngPath   = directory / L"roundtrip-π.png";
  const std::filesystem::path hdrPath   = directory / L"roundtrip-π.hdr";

  // 2x2 RGBA8 pixels with distinct color and alpha values in every texel.
  constexpr std::array<uint8_t, 16> rgba8 {
    255, 0, 0, 255,
    0, 255, 0, 128,
    0, 0, 255, 64,
    17, 33, 65, 129,
  };

  // 2x2 RGBA32F pixels spanning values below 1 and well above 1, which exercises the shared RGBE exponent.
  constexpr std::array<float, 16> rgba32f {
    0.25F, 0.5F, 1.0F, 1.0F,
    2.0F, 4.0F, 8.0F, 1.0F,
    0.125F, 0.0625F, 0.03125F, 1.0F,
    16.0F, 1.5F, 0.75F, 1.0F,
  };

  // PNG round trip
  // PNG is lossless, so every byte must come back unchanged.

  rtpt::WritePng(pngPath, 2, 2, rgba8);

  const std::optional<rtpt::ImageRgba8> png = rtpt::LoadImageRgba8(pngPath);

  if(!png || png->width != 2 || png->height != 2 || !std::ranges::equal(png->pixels, rgba8))
  {
    std::cerr << "lossless PNG round trip changed dimensions or pixels\n";
    return 1;
  }

  // HDR round trip
  // Radiance HDR stores RGBE, which quantizes each channel, so values are compared with a 2% tolerance relative to the larger of 1 and the expected magnitude.

  rtpt::WriteHdr(hdrPath, 2, 2, rgba32f);

  const std::optional<rtpt::ImageRgba32f> hdr = rtpt::LoadImageRgba32f(hdrPath);

  if(!hdr || hdr->width != 2 || hdr->height != 2 || hdr->pixels.size() != rgba32f.size())
  {
    std::cerr << "HDR round trip changed dimensions or channel count\n";
    return 1;
  }

  for(size_t index = 0; index < rgba32f.size(); ++index)
  {
    const float scale = std::max(1.0F, std::abs(rgba32f[index]));

    if(std::abs(hdr->pixels[index] - rgba32f[index]) > 0.02F * scale)
    {
      std::cerr << "HDR round trip exceeded RGBE quantization tolerance\n";
      return 1;
    }
  }

  // Cleanup failures are ignored; a leftover temp directory does not invalidate the result.
  std::error_code error;

  std::filesystem::remove_all(directory, error);

  return 0;
}
