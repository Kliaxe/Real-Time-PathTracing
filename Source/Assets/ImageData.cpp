// Implementations
// This translation unit compiles the stb image decoder and encoder. STB_IMAGE_STATIC keeps these stb symbols private, because GltfImport.cpp compiles its own copy of stb for tinygltf.

#define STB_IMAGE_STATIC
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include "ImageData.h"

#include <cstddef>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace rtpt
{
namespace
{
// Every decode and encode goes through RGBA so callers never branch on channel count.
constexpr int kRgbaComponents = 4;

// Reads a whole file into memory. Returns empty on any failure, including files too large for stb's int-sized length parameter.
std::vector<stbi_uc> ReadFile(const std::filesystem::path& path)
{
  std::ifstream stream(path, std::ios::binary | std::ios::ate);

  if(!stream)
  {
    return {};
  }

  const std::streamoff length = stream.tellg();

  if(length <= 0 || static_cast<uint64_t>(length) > static_cast<uint64_t>(std::numeric_limits<int>::max()))
  {
    return {};
  }

  std::vector<stbi_uc> bytes(static_cast<size_t>(length));
  stream.seekg(0, std::ios::beg);

  if(!stream.read(reinterpret_cast<char*>(bytes.data()), length))
  {
    return {};
  }

  return bytes;
}

// Value count for dimensions reported by the stb decoder, which uses int.
size_t RgbaValueCount(int width, int height)
{
  if(width <= 0 || height <= 0)
  {
    throw std::runtime_error("image decoder returned invalid dimensions");
  }

  const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);

  if(pixelCount > std::numeric_limits<size_t>::max() / kRgbaComponents)
  {
    throw std::overflow_error("decoded image dimensions overflow addressable memory");
  }

  return pixelCount * kRgbaComponents;
}

// Value count for caller-supplied dimensions, checking each multiplication for overflow.
size_t RgbaValueCount(uint32_t width, uint32_t height)
{
  if(width == 0 || height == 0 || static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / static_cast<size_t>(height) || static_cast<size_t>(width) * static_cast<size_t>(height) > std::numeric_limits<size_t>::max() / kRgbaComponents)
  {
    throw std::overflow_error("image dimensions overflow addressable memory");
  }

  return static_cast<size_t>(width) * static_cast<size_t>(height) * kRgbaComponents;
}

// stb write callback: appends each encoded chunk to the std::vector<std::byte> passed as context.
void AppendEncodedBytes(void* context, void* data, int size)
{
  if(context == nullptr || data == nullptr || size <= 0)
  {
    return;
  }

  auto&       bytes = *static_cast<std::vector<std::byte>*>(context);
  const auto* begin = static_cast<const std::byte*>(data);

  bytes.insert(bytes.end(), begin, begin + size);
}

// Writes encoded bytes to disk, creating the parent directory first so capture output paths need not exist in advance.
void WriteFile(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
  if(!path.parent_path().empty())
  {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    if(error)
    {
      throw std::runtime_error("failed to create image output directory: " + error.message());
    }
  }

  std::ofstream stream(path, std::ios::binary | std::ios::trunc);

  if(!stream || !stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
  {
    throw std::runtime_error("failed to write image: " + path.string());
  }
}

void ValidateOutput(uint32_t width, uint32_t height, size_t valueCount, size_t suppliedCount)
{
  if(width == 0 || height == 0 || valueCount != suppliedCount)
  {
    throw std::invalid_argument("image dimensions do not match the supplied RGBA pixels");
  }
}
}  // namespace

std::optional<ImageRgba8> LoadImageRgba8(const std::filesystem::path& path)
{
  const std::vector<stbi_uc> encoded = ReadFile(path);

  if(encoded.empty())
  {
    return std::nullopt;
  }

  // Decode
  // stb expands every source format to the requested four channels.

  int width            = 0;
  int height           = 0;
  int sourceComponents = 0;

  stbi_uc* decoded = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &sourceComponents, kRgbaComponents);

  if(decoded == nullptr)
  {
    return std::nullopt;
  }

  // Copy out
  // stb allocates the decoded pixels with malloc, so they are freed on both the success and the exception path.

  try
  {
    const size_t valueCount = RgbaValueCount(width, height);

    ImageRgba8 image { .width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height) };
    image.pixels.assign(decoded, decoded + valueCount);

    stbi_image_free(decoded);

    return image;
  }
  catch(...)
  {
    stbi_image_free(decoded);
    throw;
  }
}

std::optional<ImageRgba32f> LoadImageRgba32f(const std::filesystem::path& path)
{
  const std::vector<stbi_uc> encoded = ReadFile(path);

  if(encoded.empty())
  {
    return std::nullopt;
  }

  int width            = 0;
  int height           = 0;
  int sourceComponents = 0;

  // LDR source
  // Non-HDR formats are decoded as 8-bit and divided by 255 directly, so the values stay in their encoded space with no sRGB-to-linear conversion.
  // Using stbi_loadf here would apply stb's gamma, which this loader avoids.

  if(stbi_is_hdr_from_memory(encoded.data(), static_cast<int>(encoded.size())) == 0)
  {
    stbi_uc* decoded = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &sourceComponents, kRgbaComponents);

    if(decoded == nullptr)
    {
      return std::nullopt;
    }

    try
    {
      const size_t valueCount = RgbaValueCount(width, height);

      ImageRgba32f image { .width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height) };
      image.pixels.resize(valueCount);

      for(size_t index = 0; index < valueCount; ++index)
      {
        image.pixels[index] = static_cast<float>(decoded[index]) / 255.0F;
      }

      stbi_image_free(decoded);

      return image;
    }
    catch(...)
    {
      stbi_image_free(decoded);
      throw;
    }
  }

  // HDR source
  // Radiance values are kept as decoded.

  float* decoded = stbi_loadf_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &sourceComponents, kRgbaComponents);

  if(decoded == nullptr)
  {
    return std::nullopt;
  }

  try
  {
    const size_t valueCount = RgbaValueCount(width, height);

    ImageRgba32f image { .width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height) };
    image.pixels.assign(decoded, decoded + valueCount);

    stbi_image_free(decoded);

    return image;
  }
  catch(...)
  {
    stbi_image_free(decoded);
    throw;
  }
}

void WritePng(const std::filesystem::path& path, uint32_t width, uint32_t height, std::span<const uint8_t> rgbaPixels)
{
  const size_t valueCount = RgbaValueCount(width, height);

  ValidateOutput(width, height, valueCount, rgbaPixels.size());

  // stb takes the dimensions and the row stride, width * kRgbaComponents, as int, so the width bound leaves room for the stride.
  if(width > static_cast<uint32_t>(std::numeric_limits<int>::max() / kRgbaComponents) || height > static_cast<uint32_t>(std::numeric_limits<int>::max()))
  {
    throw std::overflow_error("PNG dimensions exceed stb limits");
  }

  // Encoding to memory first means a failed encode never leaves a truncated file behind.
  std::vector<std::byte> encoded;
  const int              result = stbi_write_png_to_func(AppendEncodedBytes, &encoded, static_cast<int>(width), static_cast<int>(height), kRgbaComponents, rgbaPixels.data(), static_cast<int>(width * kRgbaComponents));

  if(result == 0 || encoded.empty())
  {
    throw std::runtime_error("stb failed to encode PNG output");
  }

  WriteFile(path, encoded);
}

void WriteHdr(const std::filesystem::path& path, uint32_t width, uint32_t height, std::span<const float> rgbaPixels)
{
  const size_t valueCount = RgbaValueCount(width, height);

  ValidateOutput(width, height, valueCount, rgbaPixels.size());

  // stb takes dimensions as int.
  if(width > static_cast<uint32_t>(std::numeric_limits<int>::max()) || height > static_cast<uint32_t>(std::numeric_limits<int>::max()))
  {
    throw std::overflow_error("HDR dimensions exceed stb limits");
  }

  // Encoding to memory first means a failed encode never leaves a truncated file behind.
  std::vector<std::byte> encoded;
  const int              result = stbi_write_hdr_to_func(AppendEncodedBytes, &encoded, static_cast<int>(width), static_cast<int>(height), kRgbaComponents, rgbaPixels.data());

  if(result == 0 || encoded.empty())
  {
    throw std::runtime_error("stb failed to encode HDR output");
  }

  WriteFile(path, encoded);
}

}  // namespace rtpt
