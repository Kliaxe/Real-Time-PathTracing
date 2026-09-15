#pragma once

#include "DeviceRequirements.h"
#include "GpuExecution.h"

#include <cstdint>
#include <vector>

#include <volk.h>

namespace rtpt
{

// SwapchainStatus
// Whether the swapchain is usable as-is or must be recreated first.

enum class SwapchainStatus
{
  Ready,
  Recreate,
};

// AcquiredSwapchainImage
// Handle for one acquired swapchain image, carrying everything the frame needs to render to it and submit.
// The frame submission waits on imageAvailable and signals renderFinished, which presentation then waits on.
// The generation ties the handle to one swapchain so a handle that outlived a recreation is rejected.

struct AcquiredSwapchainImage
{
  // Recreate when acquisition found the swapchain out of date.
  SwapchainStatus status = SwapchainStatus::Recreate;
  // Acquired image. Null when acquisition failed.
  VkImage         image = VK_NULL_HANDLE;
  // Color view of the image.
  VkImageView     view = VK_NULL_HANDLE;
  // The frame slot's acquire semaphore, signalled when the image is ready.
  VkSemaphore     imageAvailable = VK_NULL_HANDLE;
  // The image's semaphore, signalled when rendering to it finishes.
  VkSemaphore     renderFinished = VK_NULL_HANDLE;
  // Swapchain extent at acquisition.
  VkExtent2D      extent {};
  // Index within the swapchain.
  uint32_t        imageIndex = UINT32_MAX;
  // Frame slot that acquired the image.
  FrameSlot       frameSlot {};
  // Swapchain generation at acquisition.
  uint64_t        generation = 0;
  // Acquisition succeeded but the swapchain no longer matches the surface exactly; the caller should mark it with RequestRecreate after presenting.
  bool            suboptimal = false;

  [[nodiscard]] explicit operator bool() const noexcept { return image != VK_NULL_HANDLE; }
};

// Swapchain
// Owns the swapchain and the per-image and per-slot synchronization objects around it.
// Acquire semaphores belong to frame slots because a semaphore must be chosen before the image index is known, and a slot's previous frame must have consumed it first.
// Present fences and image release, from swapchain maintenance, let it wait for presentation to finish and return an unpresented image during recreation, cancellation, and teardown.

class Swapchain
{
public:

  Swapchain() = default;
  Swapchain(const Swapchain&)            = delete;
  Swapchain& operator=(const Swapchain&) = delete;
  ~Swapchain();

  // Creates one acquire semaphore per frame slot of the execution. The swapchain itself is created by the first Recreate.
  void Initialize(VkPhysicalDevice physicalDevice, VkDevice device, VkSurfaceKHR surface, const QueueSelection& queues, GpuExecution& execution);
  void Destroy();

  // No image may be acquired. Returns Recreate without changing anything for a zero-size extent.
  // An out-of-date surface also returns Recreate. The old swapchain is kept when creation failed before the driver retired it, and destroyed when it failed after, since a retired swapchain cannot acquire images.
  [[nodiscard]] SwapchainStatus Recreate(VkExtent2D requestedExtent);

  // Set by an out-of-date acquisition, an out-of-date or suboptimal presentation, a failed recreation, or RequestRecreate, and cleared by a successful recreation. Callers poll it so a recreation is retried without waiting for a window resize.
  [[nodiscard]] bool RecreateRequired() const noexcept { return m_RecreateRequired; }

  // Marks the swapchain for recreation without touching it, so the caller recreates it at a point of its own choosing, together with everything sized to it.
  void RequestRecreate() noexcept { m_RecreateRequired = true; }

  // Returns an empty handle, and marks the swapchain for recreation, when it is out of date or does not currently exist.
  [[nodiscard]] AcquiredSwapchainImage Acquire(FrameSlot slot, uint64_t timeout = UINT64_MAX);

  // Records the submission that rendered the image; it consumed the slot's acquire semaphore.
  void CommitSubmission(const AcquiredSwapchainImage& acquired, CompletionPoint completion);
  [[nodiscard]] SwapchainStatus Present(VkQueue queue, const AcquiredSwapchainImage& acquired);

  // Returns an acquired, unsubmitted image without presenting it.
  void Cancel(const AcquiredSwapchainImage& acquired);
  void WaitForPresentCompletion();

  [[nodiscard]] VkFormat Format() const noexcept { return m_Format.format; }
  [[nodiscard]] VkColorSpaceKHR ColorSpace() const noexcept { return m_Format.colorSpace; }
  [[nodiscard]] VkExtent2D Extent() const noexcept { return m_Extent; }
  [[nodiscard]] uint32_t ImageCount() const noexcept { return static_cast<uint32_t>(m_Images.size()); }

private:

  // ImageState
  // Per-image resources and the image's position in the acquire, submit, present cycle.

  struct ImageState
  {
    // Swapchain-owned image.
    VkImage         image = VK_NULL_HANDLE;
    // Color view created for the image.
    VkImageView     view = VK_NULL_HANDLE;
    // Signalled by the render submission and waited on by present.
    VkSemaphore     renderFinished = VK_NULL_HANDLE;
    // Signalled when the image's last present operation completes.
    VkFence         presentFence = VK_NULL_HANDLE;
    // Submission that rendered the image while it is acquired.
    CompletionPoint renderCompletion {};
    // Acquired and not yet presented or released.
    bool            acquired = false;
    // A render submission has been committed for the acquired image.
    bool            submitted = false;
    // Presented, and the fence has not been waited on since.
    bool            presentPending = false;
    // Frame slot that acquired the image, or UINT32_MAX when not acquired.
    uint32_t        acquireSlot = UINT32_MAX;
  };

  // AcquireState
  // A frame slot's acquire semaphore and whether it is free to pass to the next acquisition.

  struct AcquireState
  {
    // Semaphore passed to vkAcquireNextImageKHR.
    VkSemaphore     semaphore = VK_NULL_HANDLE;
    // Submission that waited on the semaphore; it is reusable once this completes.
    CompletionPoint consumed {};
    // An image was acquired with the semaphore and nothing has consumed it yet.
    bool            pending = false;
  };

  // oldSwapchainRetired is set once vkCreateSwapchainKHR has been called with a non-null oldSwapchain, which retires it even if creation fails.
  [[nodiscard]] VkResult Create(VkExtent2D requestedExtent, VkSwapchainKHR oldSwapchain, bool& oldSwapchainRetired);
  void DestroyImages();

  // Destroys the views, semaphores, and fences created for a set of images. The images themselves belong to their swapchain.
  void DestroyImageObjects(std::vector<ImageState>& images);

  // Throws if the handle is stale or does not match the current acquisition state.
  void Validate(const AcquiredSwapchainImage& acquired) const;
  void ReleaseAcquiredImage(uint32_t imageIndex);

  // Physical device queried for surface capabilities.
  VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
  // Logical device.
  VkDevice         m_Device = VK_NULL_HANDLE;
  // Surface presented to. Owned elsewhere.
  VkSurfaceKHR     m_Surface = VK_NULL_HANDLE;
  // Render and present families; images are shared between them when they differ.
  QueueSelection   m_Queues {};
  // Used to consume semaphores and wait for render submissions.
  GpuExecution*    m_Execution = nullptr;
  // Current swapchain.
  VkSwapchainKHR   m_Swapchain = VK_NULL_HANDLE;
  // Chosen surface format.
  VkSurfaceFormatKHR m_Format {};
  // Current image extent.
  VkExtent2D       m_Extent {};
  // Chosen present mode.
  VkPresentModeKHR m_PresentMode = VK_PRESENT_MODE_FIFO_KHR;
  // Per-image state, indexed by swapchain image index.
  std::vector<ImageState>   m_Images;
  // Per-frame-slot acquire state, indexed by slot.
  std::vector<AcquireState> m_Acquire;
  // Incremented on each successful recreation, and when a failed one destroys the retired swapchain, to invalidate outstanding handles.
  uint64_t                  m_Generation = 0;
  // The swapchain is out of date or missing and must be recreated before the next acquisition can succeed.
  bool                      m_RecreateRequired = false;
};

}  // namespace rtpt
