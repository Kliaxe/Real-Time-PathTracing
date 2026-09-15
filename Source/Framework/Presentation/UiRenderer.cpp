#include "UiRenderer.h"

#include "Framework/Platform/Window.h"
#include "Framework/Vulkan/Diagnostics.h"

#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace rtpt
{
namespace
{
// Result hook the ImGui Vulkan backend calls after its own Vulkan calls. Negative codes are errors; positive status codes pass through.
// The hook has no way to return a failure to the backend, so errors are printed and the process aborts.
void CheckImGuiVkResult(VkResult result)
{
  if(result < 0)
  {
    std::fprintf(stderr, "ImGui Vulkan backend failed with %s (%d)\n", VkResultName(result), result);
    std::abort();
  }
}
}  // namespace

UiRenderer::~UiRenderer()
{
  Destroy();
}

void UiRenderer::Initialize(const UiRendererCreateInfo& createInfo)
{
  // An existing ImGui context means another UiRenderer is live; the backends keep global state and cannot be initialized twice.
  if(m_Device != VK_NULL_HANDLE || createInfo.window == nullptr || createInfo.window->Handle() == nullptr || createInfo.instance == VK_NULL_HANDLE || createInfo.physicalDevice == VK_NULL_HANDLE || createInfo.device == VK_NULL_HANDLE || createInfo.queue == VK_NULL_HANDLE || createInfo.queueFamily == VK_QUEUE_FAMILY_IGNORED || createInfo.imageCount < 2 || createInfo.colorFormat == VK_FORMAT_UNDEFINED || ImGui::GetCurrentContext() != nullptr)
  {
    throw std::invalid_argument("invalid UiRenderer initialization");
  }

  m_Device      = createInfo.device;
  m_ColorFormat = createInfo.colorFormat;

  // Any failure below runs Destroy, which releases whatever was created before rethrowing.
  try
  {
    // Descriptor pool
    // Every ImGui texture, including the font atlas and each RegisterTexture call, takes one combined image sampler set.
    // FREE_DESCRIPTOR_SET_BIT is required because textures are removed individually rather than by resetting the pool.

    constexpr VkDescriptorPoolSize poolSize { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 };

    const VkDescriptorPoolCreateInfo poolInfo {
      .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      .maxSets       = poolSize.descriptorCount,
      .poolSizeCount = 1,
      .pPoolSizes    = &poolSize,
    };

    CheckVk(vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescriptorPool), "vkCreateDescriptorPool(ImGui)");

    // ImGui context and GLFW backend
    // Passing true installs ImGui's GLFW callbacks, which chain to the ones Window already registered, so both see every event.

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    m_ContextCreated = true;

    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    if(!ImGui_ImplGlfw_InitForVulkan(createInfo.window->Handle(), true))
    {
      throw std::runtime_error("ImGui_ImplGlfw_InitForVulkan failed");
    }

    m_GlfwBackendInitialized = true;

    // Vulkan backend
    // Dynamic rendering lets Record draw straight into a swapchain image view without a render pass or framebuffer.
    // The rendering info points at m_ColorFormat, so the format has to live in a member rather than on the stack.

    ImGui_ImplVulkan_InitInfo initInfo {};

    initInfo.ApiVersion                  = createInfo.apiVersion;
    initInfo.Instance                    = createInfo.instance;
    initInfo.PhysicalDevice              = createInfo.physicalDevice;
    initInfo.Device                      = createInfo.device;
    initInfo.QueueFamily                 = createInfo.queueFamily;
    initInfo.Queue                       = createInfo.queue;
    initInfo.DescriptorPool              = m_DescriptorPool;
    initInfo.MinImageCount               = createInfo.imageCount;
    initInfo.ImageCount                  = createInfo.imageCount;
    initInfo.MSAASamples                 = VK_SAMPLE_COUNT_1_BIT;
    initInfo.UseDynamicRendering         = true;
    initInfo.PipelineRenderingCreateInfo = {
      .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount    = 1,
      .pColorAttachmentFormats = &m_ColorFormat,
    };
    initInfo.CheckVkResultFn             = CheckImGuiVkResult;

    if(!ImGui_ImplVulkan_Init(&initInfo))
    {
      throw std::runtime_error("ImGui_ImplVulkan_Init failed");
    }

    m_VulkanBackendInitialized = true;
  }
  catch(...)
  {
    Destroy();
    throw;
  }
}

void UiRenderer::Destroy()
{
  if(m_Device == VK_NULL_HANDLE)
  {
    return;
  }

  // Close an unrendered frame so ImGui's frame state is consistent before the context is destroyed.
  if(m_FrameActive)
  {
    ImGui::EndFrame();
    m_FrameActive = false;
  }

  // Backend shutdown
  // Registered textures are removed while the Vulkan backend still exists, then backends shut down before the context they belong to.
  // Destroy also cleans up a failed Initialize, so each piece is only shut down if it was actually brought up.
  // The descriptor pool goes last because the backend's sets were allocated from it.

  if(m_VulkanBackendInitialized)
  {
    for(VkDescriptorSet texture : m_Textures)
    {
      ImGui_ImplVulkan_RemoveTexture(texture);
    }

    ImGui_ImplVulkan_Shutdown();
  }

  m_Textures.clear();

  if(m_GlfwBackendInitialized)
  {
    ImGui_ImplGlfw_Shutdown();
  }

  if(m_ContextCreated)
  {
    ImGui::DestroyContext();
  }

  m_VulkanBackendInitialized = false;
  m_GlfwBackendInitialized   = false;
  m_ContextCreated           = false;

  if(m_DescriptorPool != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
  }

  m_DescriptorPool = VK_NULL_HANDLE;
  m_ColorFormat    = VK_FORMAT_UNDEFINED;
  m_Device         = VK_NULL_HANDLE;
}

void UiRenderer::BeginFrame()
{
  if(m_Device == VK_NULL_HANDLE || m_FrameActive)
  {
    throw std::logic_error("UiRenderer::BeginFrame requires an initialized idle renderer");
  }

  // Backends update first so ImGui::NewFrame sees this frame's display size and input.
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  m_FrameActive = true;
}

void UiRenderer::Record(VkCommandBuffer commandBuffer, VkImageView target, VkExtent2D extent, VkAttachmentLoadOp loadOp, VkClearColorValue clearColor)
{
  if(!m_FrameActive || commandBuffer == VK_NULL_HANDLE || target == VK_NULL_HANDLE || extent.width == 0 || extent.height == 0)
  {
    throw std::invalid_argument("invalid UiRenderer recording");
  }

  ImGui::Render();

  m_FrameActive = false;

  // Render pass
  // The caller owns the target's layout transitions; this pass only assumes COLOR_ATTACHMENT_OPTIMAL and stores the result for presentation.

  const VkRenderingAttachmentInfo colorAttachment {
    .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
    .imageView   = target,
    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    .loadOp      = loadOp,
    .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
    .clearValue  = { .color = clearColor },
  };

  const VkRenderingInfo renderingInfo {
    .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
    .renderArea           = { .extent = extent },
    .layerCount           = 1,
    .colorAttachmentCount = 1,
    .pColorAttachments    = &colorAttachment,
  };

  vkCmdBeginRendering(commandBuffer, &renderingInfo);
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
  vkCmdEndRendering(commandBuffer);
}

void UiRenderer::SetImageCount(uint32_t imageCount)
{
  // The ImGui Vulkan backend requires at least two swapchain images.
  if(m_Device == VK_NULL_HANDLE || imageCount < 2)
  {
    throw std::invalid_argument("invalid ImGui swapchain image count");
  }

  ImGui_ImplVulkan_SetMinImageCount(imageCount);
}

VkDescriptorSet UiRenderer::RegisterTexture(VkSampler sampler, VkImageView view, VkImageLayout layout)
{
  if(m_Device == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE || view == VK_NULL_HANDLE)
  {
    throw std::invalid_argument("invalid ImGui texture registration");
  }

  const VkDescriptorSet texture = ImGui_ImplVulkan_AddTexture(sampler, view, layout);

  if(texture == VK_NULL_HANDLE)
  {
    throw std::runtime_error("ImGui_ImplVulkan_AddTexture failed");
  }

  m_Textures.push_back(texture);

  return texture;
}

void UiRenderer::UnregisterTexture(VkDescriptorSet texture)
{
  const auto found = std::ranges::find(m_Textures, texture);

  // Only sets this renderer handed out may be freed, which also catches double unregistration.
  if(found == m_Textures.end())
  {
    throw std::invalid_argument("ImGui texture registration is not owned by this renderer");
  }

  ImGui_ImplVulkan_RemoveTexture(texture);

  m_Textures.erase(found);
}

}  // namespace rtpt
