#include "Framework/Platform/Window.h"
#include "Framework/Presentation/UiRenderer.h"
#include "Framework/Vulkan/Barriers.h"
#include "Framework/Vulkan/Diagnostics.h"
#include "Framework/Vulkan/GpuExecution.h"
#include "Framework/Vulkan/GpuResources.h"
#include "Framework/Vulkan/PresentationSurface.h"
#include "Framework/Vulkan/Swapchain.h"
#include "Framework/Vulkan/VulkanDevice.h"
#include "Framework/Vulkan/VulkanInstance.h"

#include <imgui.h>

#include <array>
#include <exception>
#include <iostream>

int main()
{
  try
  {
    // Window and instance
    // A hidden window still provides a real surface, so presentation runs without showing anything on screen.
    // Validation and synchronization validation are enabled; any reported error fails the test at teardown.

    rtpt::Window window;

    window.Initialize({ .title = "RTPT presentation validation", .width = 256, .height = 256, .visible = false });

    rtpt::VulkanInstance instance;
    const std::span<const char* const> windowExtensions = window.RequiredVulkanInstanceExtensions();

    instance.Initialize({ .applicationName = "RtptPresentationTests", .requiredExtensions = { windowExtensions.data(), windowExtensions.size() }, .validation = true, .synchronizationValidation = true });

    // Surface and device

    VkSurfaceKHR rawSurface = VK_NULL_HANDLE;

    rtpt::CheckVk(window.CreateVulkanSurface(instance.Handle(), rawSurface), "Window::CreateVulkanSurface");

    rtpt::PresentationSurface surface;

    surface.Initialize(instance.Handle(), rawSurface);

    rtpt::VulkanDevice device;

    device.Initialize(instance.Handle(), surface.Handle());

    rtpt::GpuExecution execution;

    execution.Initialize(device.Handle(), device.RenderQueue(), device.Queues().renderFamily, 2);

    rtpt::ResourceAllocator resources;

    resources.Initialize(instance.Handle(), device.PhysicalDevice(), device.Handle(), instance.ApiVersion(), execution);

    // Swapchain and UI
    // UiRenderer rejects fewer than two swapchain images, so the image count is checked before it is created.

    rtpt::Swapchain swapchain;

    swapchain.Initialize(device.PhysicalDevice(), device.Handle(), surface.Handle(), device.Queues(), execution);

    if(swapchain.Recreate({ 256, 256 }) != rtpt::SwapchainStatus::Ready || swapchain.ImageCount() < 2)
    {
      std::cerr << "swapchain creation did not produce a usable image set\n";
      return 1;
    }

    rtpt::UiRenderer ui;

    ui.Initialize({ .window = &window, .instance = instance.Handle(), .physicalDevice = device.PhysicalDevice(), .device = device.Handle(), .queue = device.RenderQueue(), .queueFamily = device.Queues().renderFamily, .apiVersion = instance.ApiVersion(), .imageCount = swapchain.ImageCount(), .colorFormat = swapchain.Format() });

    // UI texture
    // A 1x1 sampled image registered with ImGui covers the RegisterTexture/UnregisterTexture path the viewport display uses.
    // Its contents are never uploaded; the probe only needs a valid view in SHADER_READ_ONLY_OPTIMAL.

    const VkImageCreateInfo textureInfo {
      .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType     = VK_IMAGE_TYPE_2D,
      .format        = VK_FORMAT_R8G8B8A8_UNORM,
      .extent        = { 1, 1, 1 },
      .mipLevels     = 1,
      .arrayLayers   = 1,
      .samples       = VK_SAMPLE_COUNT_1_BIT,
      .tiling        = VK_IMAGE_TILING_OPTIMAL,
      .usage         = VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    const VkImageViewCreateInfo textureViewInfo {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = textureInfo.format,
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
    };

    rtpt::Image textureImage;

    rtpt::CheckVk(resources.CreateImage(textureImage, textureInfo, &textureViewInfo), "ResourceAllocator::CreateImage(ImGui texture)");

    const VkSamplerCreateInfo textureSamplerInfo {
      .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter    = VK_FILTER_LINEAR,
      .minFilter    = VK_FILTER_LINEAR,
      .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod       = 0.0F,
    };

    rtpt::Sampler textureSampler;

    rtpt::CheckVk(resources.CreateSampler(textureSampler, textureSamplerInfo), "ResourceAllocator::CreateSampler(ImGui texture)");

    const VkImageSubresourceRange textureRange {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .levelCount = 1,
      .layerCount = 1,
    };

    (void)execution.ExecuteAndWait([&](VkCommandBuffer commandBuffer) {
      rtpt::CmdImageBarrier(commandBuffer, textureImage.image, textureRange, { .layout = VK_IMAGE_LAYOUT_UNDEFINED }, { .access = { .stages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT }, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
    });

    const VkDescriptorSet uiTexture = ui.RegisterTexture(textureSampler.sampler, textureImage.descriptor.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Frame presentation
    // One frame builds the UI, acquires a swapchain image, clears and draws the UI into it, and presents.
    // The acquired image moves UNDEFINED to COLOR_ATTACHMENT_OPTIMAL for UiRenderer::Record, then to PRESENT_SRC_KHR.
    // The submission waits on the image's acquisition semaphore and signals the one presentation waits on.

    bool showRendererModeNames = false;

    const auto presentOneFrame = [&] {
      ui.BeginFrame();
      ImGui::Begin("Presentation probe");
      ImGui::TextUnformatted("Owned dynamic-rendering backend");

      // Opening the renderer combo introduces glyphs after the initial font atlas upload and exercises ImGui's partial texture-update path.
      if(showRendererModeNames)
      {
        ImGui::TextUnformatted("Rasterizer Path Tracer");
      }

      ImGui::Image(ImTextureID(uiTexture), ImVec2(16.0F, 16.0F));
      ImGui::End();

      const rtpt::FrameContext frame = execution.BeginFrame();
      const rtpt::AcquiredSwapchainImage acquired = swapchain.Acquire(frame.slot);

      // The window size never changes here, so a request to recreate is unexpected.
      if(!acquired)
      {
        execution.CancelFrame();
        throw std::runtime_error("presentation probe acquisition requested recreation");
      }

      const VkImageSubresourceRange colorRange { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 };

      rtpt::CmdImageBarrier(frame.commands, acquired.image, colorRange, { .layout = VK_IMAGE_LAYOUT_UNDEFINED }, { .access = { .stages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT }, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
      ui.Record(frame.commands, acquired.view, acquired.extent, VK_ATTACHMENT_LOAD_OP_CLEAR, VkClearColorValue { { 0.02F, 0.02F, 0.02F, 1.0F } });
      rtpt::CmdImageBarrier(frame.commands, acquired.image, colorRange, { .access = { .stages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT }, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, { .layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR });

      const rtpt::SemaphoreWait wait { .semaphore = acquired.imageAvailable, .stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
      const rtpt::SemaphoreSignal signal { .semaphore = acquired.renderFinished, .stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };

      const rtpt::CompletionPoint completion = execution.SubmitFrame({ .waits = std::span(&wait, 1), .signals = std::span(&signal, 1) });

      swapchain.CommitSubmission(acquired, completion);

      return swapchain.Present(device.PresentQueue(), acquired);
    };

    // The second frame adds new glyphs after the first frame has already uploaded the font atlas.
    (void)presentOneFrame();
    showRendererModeNames = true;
    (void)presentOneFrame();
    swapchain.WaitForPresentCompletion();

    // Cancelled frame
    // An acquired image can be abandoned without submitting: the frame is cancelled first, then the image is handed back to the swapchain.

    const rtpt::FrameContext cancelledFrame = execution.BeginFrame();
    const rtpt::AcquiredSwapchainImage cancelled = swapchain.Acquire(cancelledFrame.slot);

    if(!cancelled)
    {
      execution.CancelFrame();
      throw std::runtime_error("cancellation probe acquisition requested recreation");
    }

    execution.CancelFrame();
    swapchain.Cancel(cancelled);

    // Swapchain recreation
    // A zero extent, as reported for a minimized window, must defer recreation instead of creating an empty swapchain.
    // A real extent afterwards must succeed, and the UI is told the possibly changed image count.

    if(swapchain.Recreate({ 0, 0 }) != rtpt::SwapchainStatus::Recreate)
    {
      std::cerr << "zero-extent swapchain recreation was not deferred\n";
      return 1;
    }

    if(swapchain.Recreate({ 320, 180 }) != rtpt::SwapchainStatus::Ready)
    {
      std::cerr << "swapchain recreation did not produce a usable image set\n";
      return 1;
    }

    ui.SetImageCount(swapchain.ImageCount());

    // Teardown
    // The texture is unregistered from ImGui before its image and sampler are released.
    // Drain lets retired resources be freed, so a nonzero live count afterwards is a genuine leak.
    // Validation errors are read before the instance that owns the debug messenger is destroyed.

    ui.UnregisterTexture(uiTexture);
    textureSampler.Reset();
    textureImage.Reset();
    ui.Destroy();
    swapchain.Destroy();
    execution.Drain();

    if(resources.LiveResourceCount() != 0)
    {
      std::cerr << "presentation resources remained live after texture teardown\n";
      return 1;
    }

    resources.Destroy();
    execution.Destroy();
    device.Destroy();
    surface.Destroy();

    if(instance.Debug().ErrorCount() != 0)
    {
      std::cerr << "validation reported " << instance.Debug().ErrorCount() << " presentation error(s)\n";
      return 1;
    }

    instance.Destroy();
    window.Destroy();

    return 0;
  }
  catch(const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
