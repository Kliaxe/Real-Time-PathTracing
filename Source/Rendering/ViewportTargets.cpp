#include "ViewportTargets.h"

#include "Framework/Vulkan/Diagnostics.h"

#include <array>
#include <stdexcept>
#include <utility>

namespace rtpt
{

ViewportTargets::~ViewportTargets()
{
  Destroy();
}

void ViewportTargets::Initialize(ResourceAllocator& resources, VkPhysicalDevice physicalDevice)
{
  if(m_Resources != nullptr || resources.Handle() == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE)
  {
    throw std::invalid_argument("invalid ViewportTargets initialization");
  }

  m_Resources      = &resources;
  m_PhysicalDevice = physicalDevice;
  m_DepthFormat    = SelectDepthFormat();

  // Destroy clears the allocator binding, so a failed initialization leaves the object uninitialized.
  if(m_DepthFormat == VK_FORMAT_UNDEFINED)
  {
    Destroy();
    throw std::runtime_error("the selected device exposes no supported depth attachment format");
  }
}

void ViewportTargets::Destroy()
{
  m_Depth.Reset();
  m_Ldr.Reset();
  m_Hdr.Reset();

  m_Resources      = nullptr;
  m_PhysicalDevice = VK_NULL_HANDLE;
  m_Extent         = {};
  m_DepthFormat    = VK_FORMAT_UNDEFINED;
}

bool ViewportTargets::Resize(VkExtent2D extent)
{
  if(m_Resources == nullptr)
  {
    throw std::logic_error("ViewportTargets is not initialized");
  }

  // Vulkan images cannot have a zero dimension.
  if(extent.width == 0 || extent.height == 0)
  {
    return false;
  }

  // Reallocating at the same size would only discard the current contents.
  if(extent.width == m_Extent.width && extent.height == m_Extent.height)
  {
    return false;
  }

  // Image descriptions
  // HDR is a color attachment for the rasterizer, a storage image for the compute and ray tracing renderers, sampled by the tonemapper,
  // and a transfer source for capture. LDR is written as storage by the tonemapper, sampled by the UI, and copied for capture.
  // Depth is only the rasterizer's depth attachment.

  const VkImageCreateInfo hdrInfo {
      .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType     = VK_IMAGE_TYPE_2D,
      .format        = HdrFormat(),
      .extent        = { extent.width, extent.height, 1 },
      .mipLevels     = 1,
      .arrayLayers   = 1,
      .samples       = VK_SAMPLE_COUNT_1_BIT,
      .tiling        = VK_IMAGE_TILING_OPTIMAL,
      .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  const VkImageViewCreateInfo hdrViewInfo {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = HdrFormat(),
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
  };

  const VkImageCreateInfo depthInfo {
      .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType     = VK_IMAGE_TYPE_2D,
      .format        = m_DepthFormat,
      .extent        = { extent.width, extent.height, 1 },
      .mipLevels     = 1,
      .arrayLayers   = 1,
      .samples       = VK_SAMPLE_COUNT_1_BIT,
      .tiling        = VK_IMAGE_TILING_OPTIMAL,
      .usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
      .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  const VkImageCreateInfo ldrInfo {
      .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType     = VK_IMAGE_TYPE_2D,
      .format        = LdrFormat(),
      .extent        = { extent.width, extent.height, 1 },
      .mipLevels     = 1,
      .arrayLayers   = 1,
      .samples       = VK_SAMPLE_COUNT_1_BIT,
      .tiling        = VK_IMAGE_TILING_OPTIMAL,
      .usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  const VkImageViewCreateInfo ldrViewInfo {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = LdrFormat(),
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
  };

  const VkImageViewCreateInfo depthViewInfo {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = m_DepthFormat,
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .levelCount = 1, .layerCount = 1 },
  };

  // Allocation
  // All three images are created before any is swapped in. If a later allocation throws, the new images are released and the current targets stay valid.

  Image nextHdr;
  Image nextLdr;
  Image nextDepth;

  CheckVk(m_Resources->CreateImage(nextHdr, hdrInfo, &hdrViewInfo), "ResourceAllocator::CreateImage(viewport HDR)");

  try
  {
    CheckVk(m_Resources->CreateImage(nextLdr, ldrInfo, &ldrViewInfo), "ResourceAllocator::CreateImage(viewport LDR)");
    CheckVk(m_Resources->CreateImage(nextDepth, depthInfo, &depthViewInfo), "ResourceAllocator::CreateImage(viewport depth)");
  }
  catch(...)
  {
    nextLdr.Reset();
    nextHdr.Reset();
    throw;
  }

  // Swap in
  // Move assignment releases the previous images before taking ownership of the new ones.

  m_Hdr    = std::move(nextHdr);
  m_Ldr    = std::move(nextLdr);
  m_Depth  = std::move(nextDepth);
  m_Extent = extent;

  return true;
}

RenderTargetView ViewportTargets::Hdr() const noexcept
{
  return { .image = m_Hdr.image, .view = m_Hdr.descriptor.imageView, .format = m_Hdr.format, .extent = m_Extent };
}

RenderTargetView ViewportTargets::Depth() const noexcept
{
  return { .image = m_Depth.image, .view = m_Depth.descriptor.imageView, .format = m_Depth.format, .extent = m_Extent };
}

RenderTargetView ViewportTargets::Ldr() const noexcept
{
  return { .image = m_Ldr.image, .view = m_Ldr.descriptor.imageView, .format = m_Ldr.format, .extent = m_Extent };
}

VkFormat ViewportTargets::SelectDepthFormat() const
{
  // Candidates
  // Tried in order, favoring 32-bit float and 24-bit depth over 16-bit. The first format usable as a depth attachment with optimal tiling wins.

  constexpr std::array candidates {
      VK_FORMAT_D32_SFLOAT,
      VK_FORMAT_X8_D24_UNORM_PACK32,
      VK_FORMAT_D24_UNORM_S8_UINT,
      VK_FORMAT_D32_SFLOAT_S8_UINT,
      VK_FORMAT_D16_UNORM,
      VK_FORMAT_D16_UNORM_S8_UINT,
  };

  for(const VkFormat candidate : candidates)
  {
    VkFormatProperties2 properties { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 };

    vkGetPhysicalDeviceFormatProperties2(m_PhysicalDevice, candidate, &properties);

    if((properties.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
    {
      return candidate;
    }
  }

  return VK_FORMAT_UNDEFINED;
}

}  // namespace rtpt
