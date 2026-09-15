#include "Swapchain.h"

#include "Diagnostics.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace rtpt
{
namespace
{
// Prefers 8-bit UNORM formats in the sRGB nonlinear color space; UNORM means the hardware applies no sRGB encoding on write. Falls back to the first format the surface offers.
// UNORM is preferred because Tonemap.hlsl writes its filmic output as display values into the UNORM LDR target, and the UI draws that image into the swapchain unchanged. An sRGB swapchain format would encode those values a second time.
VkSurfaceFormatKHR ChooseFormat(std::span<const VkSurfaceFormatKHR> formats)
{
  constexpr std::array preferred {
    VkFormat { VK_FORMAT_B8G8R8A8_UNORM },
    VkFormat { VK_FORMAT_R8G8B8A8_UNORM },
  };

  for(VkFormat format : preferred)
  {
    const auto found = std::ranges::find_if(formats, [format](const VkSurfaceFormatKHR& candidate) {
      return candidate.format == format && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });

    if(found != formats.end())
    {
      return *found;
    }
  }

  return formats.front();
}

// Mailbox when offered; otherwise FIFO, which every Vulkan implementation must support.
VkPresentModeKHR ChoosePresentMode(std::span<const VkPresentModeKHR> modes)
{
  return std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end() ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
}

// A current extent of UINT32_MAX means the surface lets the swapchain pick its size, so the requested size is clamped to the allowed range. Otherwise the surface size is mandatory.
VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, VkExtent2D requested)
{
  if(capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
  {
    return capabilities.currentExtent;
  }

  return {
    .width  = std::clamp(requested.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
    .height = std::clamp(requested.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
  };
}
}  // namespace

Swapchain::~Swapchain()
{
  Destroy();
}

void Swapchain::Initialize(VkPhysicalDevice physicalDevice, VkDevice device, VkSurfaceKHR surface, const QueueSelection& queues, GpuExecution& execution)
{
  if(m_Device != VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE || surface == VK_NULL_HANDLE || queues.renderFamily == VK_QUEUE_FAMILY_IGNORED || queues.presentFamily == VK_QUEUE_FAMILY_IGNORED || execution.FrameSlotCount() == 0)
  {
    throw std::invalid_argument("invalid Swapchain initialization");
  }

  m_PhysicalDevice = physicalDevice;
  m_Device         = device;
  m_Surface        = surface;
  m_Queues         = queues;
  m_Execution      = &execution;
  m_Acquire.resize(execution.FrameSlotCount());

  // Acquire semaphores
  // One binary semaphore per frame slot. The swapchain itself is created later by Recreate, once the window size is known.

  try
  {
    const VkSemaphoreCreateInfo semaphoreInfo { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

    for(AcquireState& state : m_Acquire)
    {
      CheckVk(vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &state.semaphore), "vkCreateSemaphore(swapchain acquire)");
    }
  }
  catch(...)
  {
    Destroy();
    throw;
  }
}

void Swapchain::Destroy()
{
  if(m_Device == VK_NULL_HANDLE)
  {
    return;
  }

  // Return acquired images
  // Each semaphore an acquired image left signalled must be consumed before the semaphore can be destroyed.
  // A submitted image's render-finished semaphore is consumed once its render completes. An unsubmitted image's acquire semaphore is consumed directly.
  // Either way, the image is then released back to the swapchain without being presented.

  for(uint32_t index = 0; index < m_Images.size(); ++index)
  {
    if(m_Images[index].acquired)
    {
      if(m_Images[index].submitted)
      {
        m_Execution->Wait(m_Images[index].renderCompletion);

        const SemaphoreWait renderWait { .semaphore = m_Images[index].renderFinished };
        const CompletionPoint renderSemaphoreConsumed = m_Execution->SubmitSynchronizationAndWait({ .waits = std::span(&renderWait, 1) });
        (void)renderSemaphoreConsumed;
      }
      else
      {
        const uint32_t slot = m_Images[index].acquireSlot;

        if(slot < m_Acquire.size())
        {
          const SemaphoreWait acquireWait { .semaphore = m_Acquire[slot].semaphore };
          const CompletionPoint acquireSemaphoreConsumed = m_Execution->SubmitSynchronizationAndWait({ .waits = std::span(&acquireWait, 1) });
          (void)acquireSemaphoreConsumed;

          m_Acquire[slot].pending = false;
        }
      }

      ReleaseAcquiredImage(index);
    }
  }

  // Release
  // Presents still in progress may use the images' semaphores, so their fences are waited on before anything is destroyed.

  WaitForPresentCompletion();
  DestroyImages();

  for(AcquireState& state : m_Acquire)
  {
    if(state.semaphore != VK_NULL_HANDLE)
    {
      vkDestroySemaphore(m_Device, state.semaphore, nullptr);
    }
  }

  m_Acquire.clear();

  m_PhysicalDevice   = VK_NULL_HANDLE;
  m_Device           = VK_NULL_HANDLE;
  m_Surface          = VK_NULL_HANDLE;
  m_Queues           = {};
  m_Execution        = nullptr;
  m_Format           = {};
  m_Extent           = {};
  m_PresentMode      = VK_PRESENT_MODE_FIFO_KHR;
  m_Generation       = 0;
  m_RecreateRequired = false;
}

SwapchainStatus Swapchain::Recreate(VkExtent2D requestedExtent)
{
  if(m_Device == VK_NULL_HANDLE)
  {
    throw std::logic_error("Swapchain is not initialized");
  }

  // A zero-size extent, as when the window is minimized, cannot back a swapchain.
  if(requestedExtent.width == 0 || requestedExtent.height == 0)
  {
    return SwapchainStatus::Recreate;
  }

  if(std::ranges::any_of(m_Images, [](const ImageState& image) { return image.acquired; }))
  {
    throw std::logic_error("cannot recreate a swapchain with an acquired image");
  }

  // Build the replacement
  // The old swapchain is handed to creation so the driver can reuse its resources. The previous state is saved first so a failed creation can put it back.
  // Vulkan retires oldSwapchain the moment vkCreateSwapchainKHR is called, even when that call fails, and a retired swapchain can no longer acquire images. The old state is therefore only put back when creation failed before that call; after it, the old swapchain is destroyed and a later Recreate starts from nothing.
  // An out-of-date surface reports Recreate so the caller retries later; any other failure throws. Either way RecreateRequired stays set until a recreation succeeds.

  WaitForPresentCompletion();

  const VkSwapchainKHR oldSwapchain = m_Swapchain;
  std::vector<ImageState> oldImages = std::move(m_Images);
  const VkSurfaceFormatKHR oldFormat = m_Format;
  const VkExtent2D oldExtent = m_Extent;
  const VkPresentModeKHR oldPresentMode = m_PresentMode;

  m_Swapchain = VK_NULL_HANDLE;

  // Set by Create just before it calls vkCreateSwapchainKHR.
  bool oldSwapchainRetired = false;

  // Undoes a failed creation: restores the old swapchain while it is still usable, and destroys it once it has been retired.
  const auto abandonReplacement = [&]() {
    m_RecreateRequired = true;

    if(!oldSwapchainRetired)
    {
      m_Swapchain   = oldSwapchain;
      m_Images      = std::move(oldImages);
      m_Format      = oldFormat;
      m_Extent      = oldExtent;
      m_PresentMode = oldPresentMode;

      return;
    }

    DestroyImageObjects(oldImages);

    if(oldSwapchain != VK_NULL_HANDLE)
    {
      vkDestroySwapchainKHR(m_Device, oldSwapchain, nullptr);
    }

    ++m_Generation;
  };

  VkResult result = VK_SUCCESS;

  try
  {
    result = Create(requestedExtent, oldSwapchain, oldSwapchainRetired);
  }
  catch(...)
  {
    abandonReplacement();
    throw;
  }

  if(result != VK_SUCCESS)
  {
    abandonReplacement();

    if(result == VK_ERROR_OUT_OF_DATE_KHR)
    {
      return SwapchainStatus::Recreate;
    }

    CheckVk(result, "vkCreateSwapchainKHR");
  }

  // Retire the old swapchain
  // Its per-image objects and the swapchain are destroyed only after the replacement exists. The generation bump invalidates any handle from before.

  DestroyImageObjects(oldImages);

  if(oldSwapchain != VK_NULL_HANDLE)
  {
    vkDestroySwapchainKHR(m_Device, oldSwapchain, nullptr);
  }

  m_RecreateRequired = false;
  ++m_Generation;

  return SwapchainStatus::Ready;
}

AcquiredSwapchainImage Swapchain::Acquire(FrameSlot slot, uint64_t timeout)
{
  if(slot.index >= m_Acquire.size())
  {
    throw std::out_of_range("swapchain frame slot is unavailable");
  }

  // A failed recreation can leave no swapchain at all. That is reported like an out-of-date one, so the caller skips the frame and recreates.
  if(m_Swapchain == VK_NULL_HANDLE)
  {
    m_RecreateRequired = true;
    return {};
  }

  // The slot's acquire semaphore can only be reused once the submission that waited on it has completed.
  AcquireState& acquire = m_Acquire[slot.index];

  if(acquire.pending || !m_Execution->IsComplete(acquire.consumed))
  {
    throw std::logic_error("frame-slot acquire semaphore is still in use");
  }

  // Acquire
  // Out of date yields an empty handle so the caller can recreate. Suboptimal still delivers a usable image and is reported on the handle.

  uint32_t imageIndex = UINT32_MAX;
  const VkResult result = vkAcquireNextImageKHR(m_Device, m_Swapchain, timeout, acquire.semaphore, VK_NULL_HANDLE, &imageIndex);

  if(result == VK_ERROR_OUT_OF_DATE_KHR)
  {
    m_RecreateRequired = true;
    return {};
  }

  if(result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
  {
    CheckVk(result, "vkAcquireNextImageKHR");
  }

  if(imageIndex >= m_Images.size() || m_Images[imageIndex].acquired)
  {
    throw std::runtime_error("vkAcquireNextImageKHR returned an invalid image state");
  }

  // Reuse the image
  // If the image's previous present has not been waited on, its fence is waited on and reset so the next present can use it again.

  ImageState& image = m_Images[imageIndex];

  if(image.presentPending)
  {
    CheckVk(vkWaitForFences(m_Device, 1, &image.presentFence, VK_TRUE, UINT64_MAX), "vkWaitForFences(presentation)");
    CheckVk(vkResetFences(m_Device, 1, &image.presentFence), "vkResetFences(presentation)");
    image.presentPending = false;
  }

  image.acquired    = true;
  image.acquireSlot = slot.index;
  acquire.pending   = true;

  return {
    .status         = SwapchainStatus::Ready,
    .image          = image.image,
    .view           = image.view,
    .imageAvailable = acquire.semaphore,
    .renderFinished = image.renderFinished,
    .extent         = m_Extent,
    .imageIndex     = imageIndex,
    .frameSlot      = slot,
    .generation     = m_Generation,
    .suboptimal     = result == VK_SUBOPTIMAL_KHR,
  };
}

void Swapchain::CommitSubmission(const AcquiredSwapchainImage& acquired, CompletionPoint completion)
{
  Validate(acquired);

  if(!completion || m_Images[acquired.imageIndex].submitted)
  {
    throw std::invalid_argument("invalid swapchain render submission");
  }

  // The render submission waited on the acquire semaphore, so the slot's semaphore is free again once that submission completes.
  ImageState& image = m_Images[acquired.imageIndex];
  image.submitted        = true;
  image.renderCompletion = completion;

  AcquireState& acquire = m_Acquire[acquired.frameSlot.index];
  acquire.pending  = false;
  acquire.consumed = completion;
}

SwapchainStatus Swapchain::Present(VkQueue queue, const AcquiredSwapchainImage& acquired)
{
  Validate(acquired);

  ImageState& image = m_Images[acquired.imageIndex];

  if(!image.submitted || queue == VK_NULL_HANDLE)
  {
    throw std::logic_error("swapchain image was not submitted before presentation");
  }

  // Present
  // The present waits on the render-finished semaphore and attaches the image's fence so later code can tell when this present has completed.

  const VkSwapchainPresentFenceInfoKHR fenceInfo {
    .sType          = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
    .swapchainCount = 1,
    .pFences        = &image.presentFence,
  };

  const VkPresentInfoKHR presentInfo {
    .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
    .pNext              = &fenceInfo,
    .waitSemaphoreCount = 1,
    .pWaitSemaphores    = &image.renderFinished,
    .swapchainCount     = 1,
    .pSwapchains        = &m_Swapchain,
    .pImageIndices      = &acquired.imageIndex,
  };

  const VkResult result = vkQueuePresentKHR(queue, &presentInfo);

  // Outcome
  // The image returns to the swapchain regardless of the result. Suboptimal and out-of-date both ask for recreation; other errors throw.

  image.acquired       = false;
  image.submitted      = false;
  image.presentPending = true;

  if(result == VK_SUCCESS)
  {
    return SwapchainStatus::Ready;
  }

  if(result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR)
  {
    m_RecreateRequired = true;
    return SwapchainStatus::Recreate;
  }

  CheckVk(result, "vkQueuePresentKHR");

  return SwapchainStatus::Recreate;
}

void Swapchain::Cancel(const AcquiredSwapchainImage& acquired)
{
  Validate(acquired);

  // A submitted image's render-finished semaphore will be signalled, so only presenting can consume it.
  ImageState& image = m_Images[acquired.imageIndex];

  if(image.submitted)
  {
    throw std::logic_error("a submitted swapchain image must be presented");
  }

  // Consume and release
  // No frame will wait on the acquire semaphore now, so an empty submission consumes it. The image is then released without presenting.

  AcquireState& acquire = m_Acquire[acquired.frameSlot.index];

  const SemaphoreWait wait { .semaphore = acquire.semaphore };
  acquire.consumed = m_Execution->SubmitSynchronizationAndWait({ .waits = std::span(&wait, 1) });
  acquire.pending  = false;

  ReleaseAcquiredImage(acquired.imageIndex);
}

void Swapchain::WaitForPresentCompletion()
{
  // Fences are not reset here; Acquire resets an image's fence before its next present.
  for(ImageState& image : m_Images)
  {
    if(image.presentPending)
    {
      CheckVk(vkWaitForFences(m_Device, 1, &image.presentFence, VK_TRUE, UINT64_MAX), "vkWaitForFences(presentation teardown)");
      image.presentPending = false;
    }
  }
}

VkResult Swapchain::Create(VkExtent2D requestedExtent, VkSwapchainKHR oldSwapchain, bool& oldSwapchainRetired)
{
  oldSwapchainRetired = false;

  // Surface capabilities
  // Swapchain images are created as color attachments and transfer destinations, so the surface must allow both.

  VkSurfaceCapabilitiesKHR capabilities {};
  CheckVk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_PhysicalDevice, m_Surface, &capabilities), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

  if((capabilities.supportedUsageFlags & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) != (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
  {
    throw std::runtime_error("surface does not support color-attachment and transfer-destination swapchain images");
  }

  // Formats and present modes

  uint32_t formatCount = 0;
  CheckVk(vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR(count)");

  std::vector<VkSurfaceFormatKHR> formats(formatCount);
  CheckVk(vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR");

  uint32_t modeCount = 0;
  CheckVk(vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_Surface, &modeCount, nullptr), "vkGetPhysicalDeviceSurfacePresentModesKHR(count)");

  std::vector<VkPresentModeKHR> modes(modeCount);
  CheckVk(vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_Surface, &modeCount, modes.data()), "vkGetPhysicalDeviceSurfacePresentModesKHR");

  if(formats.empty() || modes.empty())
  {
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
  }

  // Swapchain
  // One image above the surface minimum, capped by the maximum when there is one; a maxImageCount of zero means no limit.
  // When render and present use different queue families, images are shared concurrently between them.

  m_Format      = ChooseFormat(formats);
  m_PresentMode = ChoosePresentMode(modes);
  m_Extent      = ChooseExtent(capabilities, requestedExtent);

  uint32_t imageCount = capabilities.minImageCount + 1;

  if(capabilities.maxImageCount != 0)
  {
    imageCount = std::min(imageCount, capabilities.maxImageCount);
  }

  const std::array queueFamilies { m_Queues.renderFamily, m_Queues.presentFamily };
  const bool concurrent = m_Queues.renderFamily != m_Queues.presentFamily;

  const VkSwapchainCreateInfoKHR createInfo {
    .sType                 = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
    .surface               = m_Surface,
    .minImageCount         = imageCount,
    .imageFormat           = m_Format.format,
    .imageColorSpace       = m_Format.colorSpace,
    .imageExtent           = m_Extent,
    .imageArrayLayers      = 1,
    .imageUsage            = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    .imageSharingMode      = concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
    .queueFamilyIndexCount = concurrent ? static_cast<uint32_t>(queueFamilies.size()) : 0u,
    .pQueueFamilyIndices   = concurrent ? queueFamilies.data() : nullptr,
    .preTransform          = capabilities.currentTransform,
    .compositeAlpha        = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
    .presentMode           = m_PresentMode,
    .clipped               = VK_TRUE,
    .oldSwapchain          = oldSwapchain,
  };

  // The driver retires oldSwapchain as soon as this call is made, whatever it returns.
  oldSwapchainRetired = oldSwapchain != VK_NULL_HANDLE;

  VkResult result = vkCreateSwapchainKHR(m_Device, &createInfo, nullptr, &m_Swapchain);

  if(result != VK_SUCCESS)
  {
    m_Swapchain = VK_NULL_HANDLE;
    return result;
  }

  // Images
  // The driver may create more images than requested, so the actual count is queried. Any failure destroys what this call created, including the new swapchain.

  result = vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &imageCount, nullptr);

  if(result != VK_SUCCESS)
  {
    DestroyImages();
    return result;
  }

  std::vector<VkImage> images(imageCount);
  result = vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &imageCount, images.data());

  if(result != VK_SUCCESS)
  {
    DestroyImages();
    return result;
  }

  // Per-image objects
  // Each image gets a color view, a render-finished semaphore, and a present fence.

  m_Images.resize(imageCount);

  const VkSemaphoreCreateInfo semaphoreInfo { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
  const VkFenceCreateInfo fenceInfo { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

  for(uint32_t index = 0; index < imageCount; ++index)
  {
    ImageState& image = m_Images[index];
    image.image = images[index];

    const VkImageViewCreateInfo viewInfo {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image            = image.image,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = m_Format.format,
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
    };

    result = vkCreateImageView(m_Device, &viewInfo, nullptr, &image.view);

    if(result == VK_SUCCESS)
    {
      result = vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &image.renderFinished);
    }

    if(result == VK_SUCCESS)
    {
      result = vkCreateFence(m_Device, &fenceInfo, nullptr, &image.presentFence);
    }

    if(result != VK_SUCCESS)
    {
      DestroyImages();
      return result;
    }
  }

  return VK_SUCCESS;
}

void Swapchain::DestroyImages()
{
  DestroyImageObjects(m_Images);

  m_Images.clear();

  if(m_Swapchain != VK_NULL_HANDLE)
  {
    vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
    m_Swapchain = VK_NULL_HANDLE;
  }
}

void Swapchain::DestroyImageObjects(std::vector<ImageState>& images)
{
  // Swapchain images themselves belong to the swapchain; only the objects created for them are destroyed individually. Handles are cleared so a second pass over the same set is harmless.
  for(ImageState& image : images)
  {
    if(image.view != VK_NULL_HANDLE)
    {
      vkDestroyImageView(m_Device, image.view, nullptr);
      image.view = VK_NULL_HANDLE;
    }

    if(image.renderFinished != VK_NULL_HANDLE)
    {
      vkDestroySemaphore(m_Device, image.renderFinished, nullptr);
      image.renderFinished = VK_NULL_HANDLE;
    }

    if(image.presentFence != VK_NULL_HANDLE)
    {
      vkDestroyFence(m_Device, image.presentFence, nullptr);
      image.presentFence = VK_NULL_HANDLE;
    }
  }
}

void Swapchain::Validate(const AcquiredSwapchainImage& acquired) const
{
  // The handle must come from the current generation and still match the image and acquire semaphore it was issued with.
  if(!acquired || acquired.generation != m_Generation || acquired.imageIndex >= m_Images.size() || acquired.frameSlot.index >= m_Acquire.size() || !m_Images[acquired.imageIndex].acquired || acquired.image != m_Images[acquired.imageIndex].image || acquired.imageAvailable != m_Acquire[acquired.frameSlot.index].semaphore)
  {
    throw std::invalid_argument("stale or invalid acquired swapchain image");
  }
}

void Swapchain::ReleaseAcquiredImage(uint32_t imageIndex)
{
  // Release to the swapchain
  // vkReleaseSwapchainImagesKHR comes from VK_KHR_swapchain_maintenance1 and returns an acquired image without presenting it.
  // volkLoadDevice fills the pointer, and the extension is a required device extension for every windowed run, the only runs with a swapchain. A null pointer therefore means the driver did not provide the entry point, which is reported rather than called.

  if(vkReleaseSwapchainImagesKHR == nullptr)
  {
    throw std::runtime_error("vkReleaseSwapchainImagesKHR is unavailable: VK_KHR_swapchain_maintenance1 was not loaded for this device");
  }

  const VkReleaseSwapchainImagesInfoKHR releaseInfo {
    .sType           = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_KHR,
    .swapchain       = m_Swapchain,
    .imageIndexCount = 1,
    .pImageIndices   = &imageIndex,
  };

  CheckVk(vkReleaseSwapchainImagesKHR(m_Device, &releaseInfo), "vkReleaseSwapchainImagesKHR");

  // Reset the image's cycle state

  ImageState& image = m_Images[imageIndex];
  image.acquired         = false;
  image.submitted        = false;
  image.renderCompletion = {};
  image.acquireSlot      = UINT32_MAX;
}

}  // namespace rtpt
