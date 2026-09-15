#include "Rendering/FrameCapture.h"

#include <cstring>
#include <fstream>
#include <iomanip>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "Assets/ImageData.h"
#include "Framework/Platform/Log.h"
#include "Framework/Vulkan/Barriers.h"
#include "Framework/Vulkan/Diagnostics.h"

namespace rtpt
{
namespace
{

// The whole single-mip, single-layer color image; both viewport targets have that shape.
constexpr VkImageSubresourceRange kColorRange {
    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel   = 0,
    .levelCount     = 1,
    .baseArrayLayer = 0,
    .layerCount     = 1,
};

// Appends the suffix as text, so dots already in the prefix are not treated as an extension.
std::filesystem::path CapturePath(const std::filesystem::path& prefix, std::string_view suffix)
{
  return std::filesystem::path(prefix.string() + std::string(suffix));
}

// Escapes backslashes, quotes, and every control character below 0x20 so the capture JSON is always valid. Common whitespace controls use their short escapes.
std::string EscapeJson(std::string_view value)
{
  std::string escaped;

  escaped.reserve(value.size());

  for(const char character : value)
  {
    switch(character)
    {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
      {
        const unsigned char code = static_cast<unsigned char>(character);

        // JSON forbids unescaped control characters, so the ones without a short escape above are written as \u00XX.
        if(code < 0x20)
        {
          constexpr const char* kHexDigits = "0123456789abcdef";

          escaped += "\\u00";
          escaped += kHexDigits[code >> 4];
          escaped += kHexDigits[code & 0x0F];
        }
        else
        {
          escaped += character;
        }

        break;
      }
    }
  }

  return escaped;
}

// Throws unless the path is a non-empty regular file, so a capture is never reported as written when a file is missing.
void RequireCapture(const std::filesystem::path& path)
{
  std::error_code error;

  if(!std::filesystem::is_regular_file(path, error) || error || std::filesystem::file_size(path, error) == 0 || error)
  {
    throw std::runtime_error("capture writer produced no data: " + path.string());
  }
}

// Copies a viewport target to CPU memory, blocking on the GPU. The image is returned to oldLayout afterwards.
std::vector<std::byte> ReadImage(const ResourceAllocator& resources, GpuExecution& execution, const RenderTargetView& image, VkImageLayout oldLayout, VkDeviceSize bytesPerPixel)
{
  // Readback buffer
  // Host-visible and persistently mapped, so the copied pixels can be read straight from the mapping.

  const VkDeviceSize size = static_cast<VkDeviceSize>(image.extent.width) * image.extent.height * bytesPerPixel;

  Buffer readback;

  CheckVk(resources.CreateBuffer(readback, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT), "ResourceAllocator::CreateBuffer(capture)");

  // Copy
  // Recorded as a one-shot submission that blocks until it completes. The image moves to TRANSFER_SRC_OPTIMAL for the copy and back to oldLayout after it, so the targets are in the same state after a capture as before.

  const CompletionPoint captureCompletion = execution.ExecuteAndWait([&](VkCommandBuffer commandBuffer) {
    CmdImageBarrier(commandBuffer, image.image, kColorRange, { { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT }, oldLayout }, { { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT }, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL });

    const VkBufferImageCopy copy {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent      = { image.extent.width, image.extent.height, 1 },
    };

    vkCmdCopyImageToBuffer(commandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);

    CmdImageBarrier(commandBuffer, image.image, kColorRange, { { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT }, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL }, { { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT }, oldLayout });
  });

  (void)captureCompletion;

  // Read
  // Invalidating the allocation makes the device's writes visible through the mapping when the memory is not host-coherent.

  CheckVk(resources.InvalidateBuffer(readback), "ResourceAllocator::InvalidateBuffer(capture)");

  std::vector<std::byte> bytes(static_cast<size_t>(size));

  std::memcpy(bytes.data(), readback.mapping, bytes.size());

  readback.Reset();

  return bytes;
}

}  // namespace

void WriteFrameCapture(const ResourceAllocator& resources, GpuExecution& execution, VkPhysicalDevice physicalDevice, const ViewportTargets& targets, const std::filesystem::path& prefix, const ApplicationOptions& options, const FrameCaptureMetadata& metadata)
{
  // Paths
  // The suffixes are appended to the prefix as text, so a prefix that already contains dots keeps them.

  const std::filesystem::path linearPath   = CapturePath(prefix, ".linear.hdr");
  const std::filesystem::path finalPath    = CapturePath(prefix, ".final.png");
  const std::filesystem::path metadataPath = CapturePath(prefix, ".json");
  const VkExtent2D extent                  = targets.Extent();

  // Images
  // After a headless frame both targets are in GENERAL. LDR is 8-bit RGBA and HDR is 32-bit float RGBA, matching the ViewportTargets formats.

  const std::vector<std::byte> ldrBytes = ReadImage(resources, execution, targets.Ldr(), VK_IMAGE_LAYOUT_GENERAL, sizeof(uint8_t) * 4);
  const std::vector<std::byte> hdrBytes = ReadImage(resources, execution, targets.Hdr(), VK_IMAGE_LAYOUT_GENERAL, sizeof(float) * 4);

  std::vector<float> hdr(hdrBytes.size() / sizeof(float));

  std::memcpy(hdr.data(), hdrBytes.data(), hdrBytes.size());

  WriteHdr(linearPath, extent.width, extent.height, hdr);
  WritePng(finalPath, extent.width, extent.height, std::span(reinterpret_cast<const uint8_t*>(ldrBytes.data()), ldrBytes.size()));

  // Metadata
  // The JSON sidecar records the renderer, resolve mode, scene, viewport, frame count, camera, tonemapper settings, and GPU and driver, so a capture can be reproduced and compared.
  // Strings that come from content, the driver, or the file system go through EscapeJson.

  VkPhysicalDeviceProperties properties {};

  vkGetPhysicalDeviceProperties(physicalDevice, &properties);

  const CameraState& camera            = metadata.camera;
  const TonemapperSettings& tonemapper = metadata.tonemapper;

  std::ofstream stream(metadataPath, std::ios::binary | std::ios::trunc);

  stream << std::setprecision(9) << "{\n  \"schema_version\": 1,\n  \"renderer\": \"" << GetRenderModeName(metadata.renderMode) << "\",\n  \"scene\": {\"index\": " << metadata.sceneIndex << ", \"label\": \"" << EscapeJson(metadata.sceneLabel) << "\"},\n  \"viewport\": {\"width\": " << extent.width << ", \"height\": " << extent.height << "},\n  \"frame_count\": " << options.frameCount << ",\n  \"resolve_mode\": " << uint32_t(metadata.resolveMode) << ",\n  \"restir_reference\": " << (options.restirReference ? "true" : "false") << ",\n  \"synchronization_validation\": " << (options.synchronizationValidation ? "true" : "false") << ",\n  \"camera\": {\"eye\": [" << camera.eye.x << ", " << camera.eye.y << ", " << camera.eye.z << "], \"center\": [" << camera.center.x << ", " << camera.center.y << ", " << camera.center.z << "], \"up\": [" << camera.up.x << ", " << camera.up.y << ", " << camera.up.z << "]},\n" << "  \"tonemapper\": {\"active\": " << tonemapper.active << ", \"method\": 0, \"exposure\": " << tonemapper.exposure << ", \"temperature\": " << tonemapper.temperature << ", \"tint\": " << tonemapper.tint << ", \"contrast\": " << tonemapper.contrast << ", \"brightness\": " << tonemapper.brightness << ", \"saturation\": " << tonemapper.saturation << ", \"vignette\": " << tonemapper.vignette << ", \"dither\": " << tonemapper.dither << "},\n" << "  \"vulkan\": {\"device_name\": \"" << EscapeJson(properties.deviceName) << "\", \"vendor_id\": " << properties.vendorID << ", \"device_id\": " << properties.deviceID << ", \"driver_version\": " << properties.driverVersion << ", \"api_version\": \"" << VK_VERSION_MAJOR(properties.apiVersion) << '.' << VK_VERSION_MINOR(properties.apiVersion) << '.' << VK_VERSION_PATCH(properties.apiVersion) << "\"},\n" << "  \"files\": {\"linear_hdr\": \"" << EscapeJson(linearPath.filename().generic_string()) << "\", \"final_png\": \"" << EscapeJson(finalPath.filename().generic_string()) << "\"}\n}\n";

  stream.close();

  // Validation
  // The metadata stream does not throw on failure, so every file is checked on disk before the capture is reported as written.

  RequireCapture(linearPath);
  RequireCapture(finalPath);
  RequireCapture(metadataPath);

  Log(LogLevel::Info, "capture written: " + prefix.string());
}

}  // namespace rtpt
