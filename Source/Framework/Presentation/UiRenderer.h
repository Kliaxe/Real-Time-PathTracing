#pragma once

#include <cstdint>
#include <vector>

#include <volk.h>

namespace rtpt
{

class Window;

// UiRendererCreateInfo
// Everything the ImGui GLFW and Vulkan backends need. UiRenderer::Initialize rejects any field left at its default except apiVersion.

struct UiRendererCreateInfo
{
  // Window whose GLFW handle ImGui reads input from. Must already be initialized.
  Window* window = nullptr;

  // Instance the device was created from.
  VkInstance instance = VK_NULL_HANDLE;

  // Physical device backing the logical device.
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

  // Device that owns the ImGui descriptor pool, pipeline, and font texture.
  VkDevice device = VK_NULL_HANDLE;

  // Queue the backend submits its texture uploads to.
  VkQueue queue = VK_NULL_HANDLE;

  // Family index of queue.
  uint32_t queueFamily = VK_QUEUE_FAMILY_IGNORED;

  // Vulkan API version the instance was created with.
  uint32_t apiVersion = VK_API_VERSION_1_3;

  // Swapchain image count. The ImGui Vulkan backend requires at least 2.
  uint32_t imageCount = 0;

  // Format of the swapchain images the UI is recorded into.
  VkFormat colorFormat = VK_FORMAT_UNDEFINED;
};

// UiRenderer
// Owns the ImGui context and its GLFW and Vulkan backends, and records the UI straight into a swapchain image with dynamic rendering.
// ImGui keeps a single global context, so only one UiRenderer may be initialized at a time.

class UiRenderer
{
public:

  UiRenderer() = default;
  UiRenderer(const UiRenderer&)            = delete;
  UiRenderer& operator=(const UiRenderer&) = delete;
  ~UiRenderer();

  // The Window must be initialized first: the GLFW backend chains to the input callbacks Window has already installed.
  void Initialize(const UiRendererCreateInfo& createInfo);

  void Destroy();

  // Starts an ImGui frame. Every BeginFrame must be followed by exactly one Record before the next BeginFrame.
  void BeginFrame();

  // Finishes the ImGui frame and draws it into target, which must already be in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL.
  void Record(VkCommandBuffer commandBuffer, VkImageView target, VkExtent2D extent, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, VkClearColorValue clearColor = {});

  // Tells the backend the swapchain image count after a swapchain recreation.
  void SetImageCount(uint32_t imageCount);

  // Makes an image drawable with ImGui::Image. The returned set stays owned by this renderer until UnregisterTexture or Destroy.
  [[nodiscard]] VkDescriptorSet RegisterTexture(VkSampler sampler, VkImageView view, VkImageLayout layout);

  // Throws if the set was not returned by RegisterTexture on this renderer.
  void UnregisterTexture(VkDescriptorSet texture);

private:

  // Null when uninitialized. Doubles as the initialization flag for every public operation.
  VkDevice m_Device = VK_NULL_HANDLE;

  // Pool the Vulkan backend allocates texture descriptor sets from.
  VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;

  // Kept as a member because the backend's pipeline rendering info stores a pointer to it.
  VkFormat m_ColorFormat = VK_FORMAT_UNDEFINED;

  // Sets handed out by RegisterTexture, so Destroy can release any the caller did not.
  std::vector<VkDescriptorSet> m_Textures;

  // True between BeginFrame and Record.
  bool m_FrameActive = false;

  // Set once ImGui::CreateContext has run, so a failed Initialize only destroys a context it created.
  bool m_ContextCreated = false;

  // Set once the GLFW backend initialized. Shutting the backend down without a matching init dereferences state that was never created.
  bool m_GlfwBackendInitialized = false;

  // Set once the Vulkan backend initialized. Its shutdown and texture removal both need the backend state that init creates.
  bool m_VulkanBackendInitialized = false;
};

}  // namespace rtpt
