#pragma once

#include <volk.h>

namespace rtpt
{

// RenderTargetView
// Non-owning view of one viewport target, handed to the renderers and passes that write or read it.
// ViewportTargets keeps ownership, so a view is only valid until the targets are resized or destroyed.

struct RenderTargetView
{
  // Image handle, used for barriers and copies.
  VkImage     image = VK_NULL_HANDLE;

  // Full-image view, used for attachments and descriptors.
  VkImageView view = VK_NULL_HANDLE;

  // Pixel format of the image.
  VkFormat    format = VK_FORMAT_UNDEFINED;

  // Image size in pixels.
  VkExtent2D  extent {};

  // Usage flags the image was created with. Libraries that bind the image themselves, such as Streamline, need them to validate the binding.
  VkImageUsageFlags usage = 0;

  // Usable only when every handle is set and the extent is non-empty.
  [[nodiscard]] explicit operator bool() const noexcept
  {
    return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE && format != VK_FORMAT_UNDEFINED && extent.width != 0 && extent.height != 0;
  }

  // Descriptor info for binding the view in the given layout. The sampler is only needed for combined image samplers.
  [[nodiscard]] VkDescriptorImageInfo Descriptor(VkImageLayout layout, VkSampler sampler = VK_NULL_HANDLE) const noexcept
  {
    return { .sampler = sampler, .imageView = view, .imageLayout = layout };
  }
};

}  // namespace rtpt
