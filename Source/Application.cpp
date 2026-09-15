#include "Application.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

#include <GLFW/glfw3.h>
#include <fmt/format.h>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include "Camera/CameraUi.h"
#include "Framework/Platform/Paths.h"
#include "Framework/Platform/Log.h"
#include "Framework/Vulkan/Barriers.h"
#include "Framework/Vulkan/Diagnostics.h"
#include "PathTracing/PathTracerUi.h"
#include "PostProcessing/TonemapperUi.h"
#include "Rendering/FrameCapture.h"
#include "Scene/SceneUi.h"

#include "Generated/Shaders/Tonemap.hlsl.main.h"

namespace rtpt
{
namespace
{

// The whole single-mip, single-layer color image; every image the application transitions has that shape.
constexpr VkImageSubresourceRange kColorRange {
    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel   = 0,
    .levelCount     = 1,
    .baseArrayLayer = 0,
    .layerCount     = 1,
};

// Frame time headless runs report to NRD: one 60 Hz frame, in milliseconds.
// Headless frames run as fast as the GPU and capture readback allow, so NRD's own wall-clock timer would make denoised captures depend on how fast a run happened to go.
// Interactive runs report zero instead, so NRD keeps measuring real frame time.
constexpr float kHeadlessFrameTimeMilliseconds = 1000.0f / 60.0f;

}  // namespace

// Blue noise only receives pointers to systems that are initialized later; its own resources are created in InitializeRendererSystems.
Application::Application(ApplicationOptions options) : m_Options(std::move(options)), m_BlueNoise({ .resources = &m_Resources, .uploads = &m_Uploads, .diagnostics = &m_Instance.Debug() })
{
}

Application::~Application()
{
  Shutdown();
}

int Application::Run()
{
  Initialize();

  // Headless runs render a fixed number of frames and optionally capture; interactive runs loop until the window closes.
  if(m_Options.headless)
  {
    for(uint32_t frame = 0; frame < m_Options.frameCount; ++frame)
    {
      if(!RenderFrame())
      {
        throw std::runtime_error("headless frame was not rendered");
      }
    }

    // Every submitted frame must finish before the targets are read back.
    m_Execution.Drain();

    if(m_Options.capturePrefix)
    {
      rtpt::CheckVk(vkQueueWaitIdle(m_Device.RenderQueue()), "vkQueueWaitIdle(before capture)");

      // The rasterizer has no resolve step, so its captures report Off.
      const RenderResolveMode resolveMode = m_RenderMode == RenderMode::ePathTracing ? m_PathTracer->GetSettings().resolveMode : m_RenderMode == RenderMode::eReSTIRPTEnhanced ? m_ReSTIRPT->GetSettings().common.resolveMode : RenderResolveMode::eOff;

      WriteFrameCapture(m_Resources, m_Execution, m_Device.PhysicalDevice(), m_Targets, *m_Options.capturePrefix, m_Options, { .renderMode = m_RenderMode, .resolveMode = resolveMode, .sceneIndex = m_SelectedSceneIndex, .sceneLabel = m_SceneDefinitions[m_SelectedSceneIndex].label, .camera = m_Camera.State(), .tonemapper = m_TonemapperSettings });
    }
  }
  else
  {
    while(!m_Window.ShouldClose())
    {
      m_Window.PollEvents();

      // A minimized window has nothing to present, so block on events instead of spinning.
      if(m_Window.Minimized())
      {
        m_Window.WaitEvents();
        continue;
      }

      RenderFrame();
    }
  }

  return 0;
}

void Application::Initialize()
{
  if(m_Initialized)
  {
    throw std::logic_error("Application is already initialized");
  }

  // A failure part-way through runs Shutdown, which releases the systems created so far, before rethrowing.
  try
  {
    // Window
    // Headless runs never create a window; the requested size then only sets the offscreen viewport.

    const VkExtent2D requestedExtent {
        m_Options.width.value_or(1920),
        m_Options.height.value_or(1080),
    };

    if(!m_Options.headless)
    {
      m_Window.Initialize({ .title = "Real-Time Path Tracing", .width = requestedExtent.width, .height = requestedExtent.height });
    }

    // Vulkan context
    // Validation follows the build type, and synchronization validation is opted into from the command line.
    // The window's instance extensions are needed before the instance exists, and the surface is created before the device so the device receives it.

#if defined(NDEBUG)
    constexpr bool validation = false;
#else
    constexpr bool validation = true;
#endif

    m_Instance.Initialize({
        .applicationName           = "RealTimePathTracing",
        .requiredExtensions        = m_Options.headless ? std::span<const char* const> {} : m_Window.RequiredVulkanInstanceExtensions(),
        .validation                = validation,
        .synchronizationValidation = m_Options.synchronizationValidation,
    });

    if(!m_Options.headless)
    {
      VkSurfaceKHR surface = VK_NULL_HANDLE;

      rtpt::CheckVk(m_Window.CreateVulkanSurface(m_Instance.Handle(), surface), "glfwCreateWindowSurface");

      m_Surface.Initialize(m_Instance.Handle(), surface);
    }

    m_Device.Initialize(m_Instance.Handle(), m_Surface.Handle());
    m_Execution.Initialize(m_Device.Handle(), m_Device.RenderQueue(), m_Device.Queues().renderFamily, kFrameSlotCount);
    m_Resources.Initialize(m_Instance.Handle(), m_Device.PhysicalDevice(), m_Device.Handle(), m_Instance.ApiVersion(), m_Execution);
    m_Uploads.Initialize(m_Resources, m_Execution);
    m_Targets.Initialize(m_Resources, m_Device.PhysicalDevice());

    // Image samplers
    // The tonemapper keeps its linear sampler. The Display panel uses nearest filtering because its non-integer scale would otherwise modulate noise variance into a grid.

    const VkSamplerCreateInfo samplerInfo {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod       = VK_LOD_CLAMP_NONE,
    };

    rtpt::CheckVk(m_Resources.CreateSampler(m_LinearSampler, samplerInfo), "ResourceAllocator::CreateSampler(viewport)");

    VkSamplerCreateInfo viewportSamplerInfo = samplerInfo;

    viewportSamplerInfo.magFilter  = VK_FILTER_NEAREST;
    viewportSamplerInfo.minFilter  = VK_FILTER_NEAREST;
    viewportSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    rtpt::CheckVk(m_Resources.CreateSampler(m_ViewportSampler, viewportSamplerInfo), "ResourceAllocator::CreateSampler(display)");

    // Presentation
    // Interactive runs size the viewport to the swapchain rather than the requested size, so the targets match the framebuffer.
    // Swapchain::Recreate returns a status, which is mapped to a VkResult so CheckVk can throw with context when the swapchain is not ready.

    VkExtent2D viewportExtent = requestedExtent;

    if(!m_Options.headless)
    {
      m_Swapchain.Initialize(m_Device.PhysicalDevice(), m_Device.Handle(), m_Surface.Handle(), m_Device.Queues(), m_Execution);

      rtpt::CheckVk(m_Swapchain.Recreate({ m_Window.FramebufferWidth(), m_Window.FramebufferHeight() }) == rtpt::SwapchainStatus::Ready ? VK_SUCCESS : VK_ERROR_OUT_OF_DATE_KHR, "Swapchain::Recreate");

      viewportExtent = m_Swapchain.Extent();

      m_Ui.Initialize({
          .window         = &m_Window,
          .instance       = m_Instance.Handle(),
          .physicalDevice = m_Device.PhysicalDevice(),
          .device         = m_Device.Handle(),
          .queue          = m_Device.RenderQueue(),
          .queueFamily    = m_Device.Queues().renderFamily,
          .apiVersion     = m_Instance.ApiVersion(),
          .imageCount     = m_Swapchain.ImageCount(),
          .colorFormat    = m_Swapchain.Format(),
      });
    }

    // Viewport targets
    // The UI shows the LDR target through a registered texture, which ResizeViewport registers again whenever the targets are reallocated.

    m_Targets.Resize(viewportExtent);
    m_Camera.SetViewport({ viewportExtent.width, viewportExtent.height });

    if(!m_Options.headless)
    {
      m_ViewportTexture = m_Ui.RegisterTexture(m_ViewportSampler.sampler, m_Targets.Ldr().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // Renderers and scene
    // Renderers exist before the scene so CreateScene can write scene textures into their descriptor sets.
    // Startup options are applied between discovery and scene build: discovery populates the catalog and picks a default scene index, which a startup override then replaces.

    InitializeRendererSystems();
    DiscoverAssets();
    ApplyStartupOptions();
    CreateScene(true);

    m_Initialized = true;
  }
  catch(...)
  {
    Shutdown();
    throw;
  }
}

void Application::Shutdown() noexcept
{
  // Teardown must reach every system even if one throws, so each release runs inside a catch-all.
  const auto release = []<typename Function>(Function&& function) noexcept
  {
    try { function(); }
    catch(...) {}
  };

  // GPU work
  // An abandoned frame is cancelled and submitted work drained first, so deferred resource retirements run while the device still exists.

  if(m_Execution.HasActiveFrame()) release([&] { m_Execution.CancelFrame(); });
  if(m_Device.Handle() != VK_NULL_HANDLE) release([&] { m_Execution.Drain(); });

  // Systems
  // Released in reverse creation order: the display texture before the UI backend that owns it, and renderers and targets before the allocator and device they were built on.
  // Shutdown also runs after a failed Initialize, so it has to cope with systems that were never created.

  if(m_ViewportTexture != VK_NULL_HANDLE)
  {
    release([&] { m_Ui.UnregisterTexture(m_ViewportTexture); });
    m_ViewportTexture = VK_NULL_HANDLE;
  }

  release([&] { m_Ui.Destroy(); });
  DestroyRendererSystems();
  release([&] { m_Targets.Destroy(); });
  release([&] { m_ViewportSampler.Reset(); });
  release([&] { m_LinearSampler.Reset(); });
  release([&] { m_Swapchain.Destroy(); });
  release([&] { m_Resources.Destroy(); });
  release([&] { m_Execution.Destroy(); });
  release([&] { m_Device.Destroy(); });
  release([&] { m_Surface.Destroy(); });
  release([&] { m_Instance.Destroy(); });
  release([&] { m_Window.Destroy(); });

  m_Initialized = false;
}

void Application::InitializeRendererSystems()
{
  // The reference path tracer receives the blue noise, so it is initialized first.
  m_BlueNoise.Initialize();

  // Scene and renderers
  // Every renderer receives the same texture descriptor capacity, and the path tracers also the frame slot count for their per-slot state.
  // The raster preview is built against the viewport target formats, which are fixed for the life of the targets.

  m_SceneRuntime      = std::make_unique<SceneRuntime>(SceneRuntime::CreateInfo { .device = &m_Device, .resources = &m_Resources, .uploads = &m_Uploads, .execution = &m_Execution });
  m_Raster            = std::make_unique<RasterRenderer>(RasterRenderer::CreateInfo { .device = &m_Device, .maxTextureDescriptors = kMaxTextureDescriptors, .colorFormat = m_Targets.HdrFormat(), .depthFormat = m_Targets.DepthFormat() });
  m_PathTracer        = std::make_unique<PathTracer>(PathTracer::CreateInfo { .device = &m_Device, .resources = &m_Resources, .diagnostics = &m_Instance.Debug(), .blueNoise = &m_BlueNoise, .frameSlotCount = kFrameSlotCount, .maxTextureDescriptors = kMaxTextureDescriptors });
  m_ReSTIRPT          = std::make_unique<ReSTIRPTRenderer>(ReSTIRPTRenderer::CreateInfo { .device = &m_Device, .resources = &m_Resources, .diagnostics = &m_Instance.Debug(), .blueNoise = &m_BlueNoise, .frameSlotCount = kFrameSlotCount, .maxTextureDescriptors = kMaxTextureDescriptors });

  m_PathTracer->Initialize();
  m_ReSTIRPT->Initialize();
  m_Raster->Initialize();

  // The tonemap shader comes from the SPIR-V embedded in its generated shader header.
  rtpt::CheckVk(m_Tonemapper.Initialize(m_Device.Handle(), std::span(Tonemap_hlsl)), "Tonemapper::Initialize");
}

void Application::DestroyRendererSystems() noexcept
{
  const auto release = []<typename Function>(Function&& function) noexcept
  {
    try { function(); }
    catch(...) {}
  };

  // Release
  // Reverse of InitializeRendererSystems. Each system is destroyed explicitly inside the catch-all, and only then are the pointers reset.

  release([&] { m_Tonemapper.Destroy(); });
  if(m_Raster) release([&] { m_Raster->Destroy(); });
  if(m_ReSTIRPT) release([&] { m_ReSTIRPT->Destroy(); });
  if(m_PathTracer) release([&] { m_PathTracer->Destroy(); });
  if(m_SceneRuntime) release([&] { m_SceneRuntime->Destroy(); });
  release([&] { m_BlueNoise.Destroy(); });

  m_ReSTIRPT.reset();
  m_PathTracer.reset();
  m_Raster.reset();
  m_SceneRuntime.reset();
}

void Application::DiscoverAssets()
{
  const SceneAssetCatalogData catalog = DiscoverSceneAssets();

  m_ModelAssets        = catalog.modelAssets;
  m_HdriAssets         = catalog.hdriAssets;
  m_SceneDefinitions   = catalog.sceneDefinitions;
  m_SelectedSceneIndex = catalog.selectedSceneIndex;
  m_SelectedHdriIndex  = catalog.selectedHdriIndex;

  // Catalog warnings are logged rather than thrown, so the rest of the catalog stays usable.
  for(const std::string& warning : catalog.warnings)
  {
    rtpt::Log(rtpt::LogLevel::Warning, warning);
  }
}

void Application::ApplyStartupOptions()
{
  m_RenderMode = m_Options.renderMode;

  // Both path tracers take the requested resolve mode, so switching renderers later keeps it. Without the option each keeps its own default.
  if(m_Options.resolveMode)
  {
    m_PathTracer->GetSettings().resolveMode      = *m_Options.resolveMode;
    m_ReSTIRPT->GetSettings().common.resolveMode = *m_Options.resolveMode;
  }

  // The comparison mode exposes the same single-sample path estimator through
  // both renderers; resampling and tiled NEE would otherwise change the samples.
  if(m_Options.restirReference)
  {
    m_ReSTIRPT->GetSettings().common.referencePathTracer = true;
    m_ReSTIRPT->GetSettings().common.resamplingMode = ReSTIRPTResamplingMode::eNone;
    m_ReSTIRPT->GetSettings().nee.enableLightTiles = 0u;
  }


  // The scene index can only be range-checked now; the catalog did not exist when the command line was parsed.
  if(m_Options.sceneIndex)
  {
    if(*m_Options.sceneIndex >= m_SceneDefinitions.size())
    {
      throw std::out_of_range(fmt::format("scene index {} is outside the catalog of {} scenes", *m_Options.sceneIndex, m_SceneDefinitions.size()));
    }

    m_SelectedSceneIndex = *m_Options.sceneIndex;
  }
}

void Application::CreateScene(bool resetCamera)
{
  // Resolve
  // ResolveSceneSelection reports the indices it actually used, which are written back so the UI selection matches the loaded scene.

  const SceneSelectionOutput resolved = ResolveSceneSelection({
      .sceneDefinitions   = m_SceneDefinitions,
      .selectedSceneIndex = m_SelectedSceneIndex,
      .hdriAssets         = m_HdriAssets,
      .selectedHdriIndex  = m_SelectedHdriIndex,
  });

  if(resolved.sceneDefinition == nullptr)
  {
    throw std::runtime_error("no scenes are available");
  }

  m_SelectedSceneIndex = resolved.resolvedSceneIndex;
  m_SelectedHdriIndex  = resolved.resolvedHdriIndex;

  // Rebuild
  // RebuildScene drains the GPU and replaces all scene data, so every descriptor set holding scene textures is rewritten and all render history discarded.

  const std::vector<std::filesystem::path> contentRoots = rtpt::ContentDirectories();

  m_SceneRuntime->RebuildScene(SceneUploader::UploadInput { .sceneDefinition = *resolved.sceneDefinition, .selectedHdriRelativePath = resolved.hdriRelativePath, .contentRoots = contentRoots }, m_Targets.Extent(), resetCamera, &m_Camera);

  UpdateTextures();
  InvalidateRenderHistory();
}

void Application::UpdateTextures()
{
  m_SceneRuntime->UpdateTextureDescriptors(m_Raster->GetDescriptorPack(), kMaxTextureDescriptors);
  m_SceneRuntime->UpdateTextureDescriptors(m_PathTracer->GetDescriptorPack(), kMaxTextureDescriptors);
  m_SceneRuntime->UpdateTextureDescriptors(m_ReSTIRPT->GetDescriptorPack(), kMaxTextureDescriptors);
}

void Application::ResizeViewport(VkExtent2D extent)
{
  // An empty extent cannot be allocated, and an unchanged one needs no work.
  if(extent.width == 0 || extent.height == 0 || (extent.width == m_Targets.Extent().width && extent.height == m_Targets.Extent().height))
  {
    return;
  }

  // Reallocate
  // In-flight frames and the UI's registered texture still reference the old targets, so work is drained and the texture unregistered before they are replaced.

  m_Execution.Drain();

  if(m_ViewportTexture != VK_NULL_HANDLE)
  {
    m_Ui.UnregisterTexture(m_ViewportTexture);
    m_ViewportTexture = VK_NULL_HANDLE;
  }

  m_Targets.Resize(extent);

  // The new LDR image has no defined layout yet, which PostProcess must transition from.
  m_LdrInitialized = false;

  m_Camera.SetViewport({ extent.width, extent.height });

  if(!m_Options.headless)
  {
    m_ViewportTexture = m_Ui.RegisterTexture(m_ViewportSampler.sampler, m_Targets.Ldr().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }

  // All render history was built at the old resolution.
  InvalidateRenderHistory();
}

void Application::UpdateCameraInput()
{
  if(m_Options.headless)
  {
    return;
  }

  // Input snapshot
  // GLFW state is translated into the windowing-free CameraInput the controller takes.

  const rtpt::InputState& input = m_Window.Input();

  const rtpt::CameraInput cameraInput {
      .leftMouse   = input.MouseButtonDown(GLFW_MOUSE_BUTTON_LEFT),
      .middleMouse = input.MouseButtonDown(GLFW_MOUSE_BUTTON_MIDDLE),
      .rightMouse  = input.MouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT),
      .shift       = (input.modifiers & GLFW_MOD_SHIFT) != 0,
      .control     = (input.modifiers & GLFW_MOD_CONTROL) != 0,
      .alt         = (input.modifiers & GLFW_MOD_ALT) != 0,
  };

  const glm::vec2 pointer { static_cast<float>(input.cursorX), static_cast<float>(input.cursorY) };

  // Drag capture
  // A drag starts only when a button the current mode drags with is down over the Display window, and captures the cursor so it keeps working outside the window.
  // Releasing every such button ends it. Starting a drag also re-anchors the controller's pointer origin at the current position.

  const bool orbitDrag      = m_Camera.Mode() == rtpt::CameraMode::Orbit && (cameraInput.leftMouse || cameraInput.middleMouse || cameraInput.rightMouse);
  const bool flyDrag        = m_Camera.Mode() == rtpt::CameraMode::Fly && (cameraInput.middleMouse || cameraInput.rightMouse);
  const bool dragButtonDown = orbitDrag || flyDrag;

  if(!m_CameraDragActive && m_DisplayHovered && dragButtonDown)
  {
    m_CameraDragActive = true;
    m_Window.SetCursorCaptured(true);
    m_Camera.SetPointerPosition(pointer);
  }
  else if(m_CameraDragActive && !dragButtonDown)
  {
    m_CameraDragActive = false;
    m_Window.SetCursorCaptured(false);
  }

  // Pointer and wheel
  // Away from the Display window and outside a drag, only the pointer origin is tracked, so interacting with other windows never moves the camera.

  if(m_DisplayHovered || m_CameraDragActive)
  {
    m_Camera.PointerMove(pointer, cameraInput);

    // The wheel only applies while the pointer is over the Display window.
    if(m_DisplayHovered && input.scrollY != 0.0)
    {
      m_Camera.Wheel(static_cast<float>(input.scrollY), cameraInput);
    }
  }
  else
  {
    m_Camera.SetPointerPosition(pointer);
  }

  // Fly keys
  // Keys only move the camera while the Display window has focus and ImGui is not taking text input.
  // The frame time is clamped to 0.1 seconds so a long frame cannot produce a large jump, and Shift moves four times faster.

  if(m_Camera.Mode() == rtpt::CameraMode::Fly && m_DisplayFocused && !ImGui::GetIO().WantTextInput)
  {
    const float right     = static_cast<float>(input.KeyDown(GLFW_KEY_D)) - static_cast<float>(input.KeyDown(GLFW_KEY_A));
    const float up        = static_cast<float>(input.KeyDown(GLFW_KEY_E)) - static_cast<float>(input.KeyDown(GLFW_KEY_Q));
    const float forward   = static_cast<float>(input.KeyDown(GLFW_KEY_W)) - static_cast<float>(input.KeyDown(GLFW_KEY_S));
    const bool boost      = input.KeyDown(GLFW_KEY_LEFT_SHIFT) || input.KeyDown(GLFW_KEY_RIGHT_SHIFT);
    const float deltaTime = std::min(ImGui::GetIO().DeltaTime, 0.1F) * (boost ? 4.0F : 1.0F);

    m_Camera.MoveFly({ right, up, forward }, deltaTime);
  }

  // Animation and history
  // Renderer history signatures decide what camera motion invalidates. ReSTIR and NRD keep temporal history during interactive rendering,
  // while an active accumulation resolve restarts because its camera signature changed.

  m_Camera.UpdateAnimation();
}

void Application::DrawUi()
{
  // Dockspace host
  // A borderless, unpadded window covers the main viewport's work area and hosts the dockspace the Settings and Display windows dock into.

  const ImGuiViewport* mainViewport = ImGui::GetMainViewport();

  ImGui::SetNextWindowPos(mainViewport->WorkPos);
  ImGui::SetNextWindowSize(mainViewport->WorkSize);
  ImGui::SetNextWindowViewport(mainViewport->ID);

  constexpr ImGuiWindowFlags dockspaceWindowFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
  ImGui::Begin("RTPT Dockspace", nullptr, dockspaceWindowFlags);
  ImGui::PopStyleVar(3);

  // Default layout
  // The split is only built when the dockspace node does not exist yet, so an existing layout is left alone.
  // Settings gets about 360 pixels on the left, clamped to between 20% and 35% of the work area width.

  const ImGuiID dockspaceId = ImGui::GetID("RTPT Main Dockspace");

  if(ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
  {
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, mainViewport->WorkSize);

    ImGuiID displayDock = dockspaceId;
    ImGuiID settingsDock = 0;

    const float settingsRatio = std::clamp(360.0F / mainViewport->WorkSize.x, 0.20F, 0.35F);

    ImGui::DockBuilderSplitNode(displayDock, ImGuiDir_Left, settingsRatio, &settingsDock, &displayDock);
    ImGui::DockBuilderDockWindow("Settings", settingsDock);
    ImGui::DockBuilderDockWindow("Display", displayDock);
    ImGui::DockBuilderFinish(dockspaceId);
  }

  ImGui::DockSpace(dockspaceId);
  ImGui::End();

  // Display
  // The viewport targets match the framebuffer, not the Display window, so the UVs crop the image symmetrically to fill the window without distorting its aspect ratio.
  // Hover and focus are recorded for UpdateCameraInput, which runs after the UI; a collapsed or hidden Display window clears both.
  // The panel's pixel size is recorded for the window title, which reports what the Display window shows rather than the whole application framebuffer.

  if(ImGui::Begin("Display"))
  {
    m_DisplayHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    m_DisplayFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    const ImVec2 available = ImGui::GetContentRegionAvail();

    // ImGui sizes are in window coordinates, so the framebuffer scale converts them to pixels on high-DPI displays.
    const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;

    m_DisplaySize = { static_cast<uint32_t>(std::max(available.x * framebufferScale.x, 0.0F) + 0.5F), static_cast<uint32_t>(std::max(available.y * framebufferScale.y, 0.0F) + 0.5F) };

    ImVec2 uvMinimum { 0.0F, 0.0F };
    ImVec2 uvMaximum { 1.0F, 1.0F };

    const VkExtent2D sourceExtent = m_Targets.Extent();

    // Both sizes must be non-empty for the aspect ratios to be defined.
    if(available.x > 0.0F && available.y > 0.0F && sourceExtent.width > 0 && sourceExtent.height > 0)
    {
      const float sourceAspect  = static_cast<float>(sourceExtent.width) / static_cast<float>(sourceExtent.height);
      const float displayAspect = available.x / available.y;

      // A window wider than the image crops its top and bottom; a narrower one crops its sides.
      if(displayAspect > sourceAspect)
      {
        const float visibleHeight = sourceAspect / displayAspect;

        uvMinimum.y = (1.0F - visibleHeight) * 0.5F;
        uvMaximum.y = 1.0F - uvMinimum.y;
      }
      else
      {
        const float visibleWidth = displayAspect / sourceAspect;

        uvMinimum.x = (1.0F - visibleWidth) * 0.5F;
        uvMaximum.x = 1.0F - uvMinimum.x;
      }
    }

    ImGui::Image(reinterpret_cast<ImTextureID>(m_ViewportTexture), available, uvMinimum, uvMaximum);
  }
  else
  {
    m_DisplayHovered = false;
    m_DisplayFocused = false;
    m_DisplaySize    = {};
  }

  ImGui::End();

  // Settings
  // Controls report whether they invalidate render history, and the history is reset once after the whole window is drawn.
  // Camera and tonemapper edits do not report invalidation: renderers detect camera changes themselves, and tonemapping runs after them.

  bool invalidateHistory = false;

  if(ImGui::Begin("Settings"))
  {
    invalidateHistory |= DrawRendererSection(m_RenderMode, *m_PathTracer, *m_ReSTIRPT);

    DrawSceneAssetsSection(m_SceneDefinitions, m_SelectedSceneIndex, m_SceneReloadRequested, m_HdriAssets, m_SelectedHdriIndex, m_HdriReloadRequested);

    invalidateHistory |= DrawSceneEnvironmentSection(m_SceneRuntime->GetSceneInfo());

    DrawCameraSection(m_Camera);
    DrawTonemapperSection(m_TonemapperSettings);

    // Only the rasterizer preview reads this override, so it does not touch path tracing history.
    ImGui::DragFloat2("Metallic/Roughness", glm::value_ptr(m_MetallicRoughnessOverride), 0.01F, -0.01F, 1.0F);
  }

  ImGui::End();

  if(invalidateHistory) InvalidateRenderHistory();
}

bool Application::RenderFrame()
{
  // Interactive frame setup
  // A window resize, or a swapchain that an earlier acquisition, presentation, or failed recreation marked for recreation, drains in-flight work, recreates the swapchain, and updates the UI image count and the viewport to match. Presentation only marks the swapchain, so the frame loop recreates it nowhere else.
  // If the swapchain is still not ready, the frame is skipped; the swapchain stays marked, so the recreation is retried next frame even without another resize.
  // The UI is drawn and camera input applied before recording, so this frame renders the latest camera.

  if(!m_Options.headless)
  {
    if(m_Window.Resized() || m_Swapchain.RecreateRequired())
    {
      m_Execution.Drain();

      if(m_Swapchain.Recreate({ m_Window.FramebufferWidth(), m_Window.FramebufferHeight() }) != rtpt::SwapchainStatus::Ready)
      {
        return false;
      }

      m_Ui.SetImageCount(m_Swapchain.ImageCount());

      ResizeViewport(m_Swapchain.Extent());
    }

    m_Ui.BeginFrame();

    DrawUi();

    // After DrawUi, which measured the Display panel this frame; the frame rate was already measured by ImGui in BeginFrame.
    m_WindowTitle.Update(m_Window, m_DisplaySize);

    UpdateCameraInput();
  }

  // Deferred scene reload
  // Requested by the UI; performed before BeginFrame because rebuilding drains the GPU and destroys the old scene resources. The camera is kept.

  if(m_SceneReloadRequested || m_HdriReloadRequested)
  {
    CreateScene(false);

    m_SceneReloadRequested = false;
    m_HdriReloadRequested  = false;
  }

  // Frame recording
  // BeginFrame waits for this frame slot's previous submission. Interactive frames then acquire a swapchain image, and the frame is cancelled when none is available.

  const rtpt::FrameContext frame = m_Execution.BeginFrame();

  rtpt::AcquiredSwapchainImage acquired;

  if(!m_Options.headless)
  {
    acquired = m_Swapchain.Acquire(frame.slot);

    if(!acquired)
    {
      m_Execution.CancelFrame();
      return false;
    }
  }

  // If anything throws while the frame is active, the frame and the acquired image are released before rethrowing.
  try
  {
    RenderScene(frame);
    PostProcess(frame.commands);

    if(!m_Options.headless) RecordPresentation(frame.commands, acquired);

    // Submission
    // Interactive submissions wait on the image-available semaphore and signal render-finished, which Present waits on. Headless submissions need no synchronization.

    std::array<rtpt::SemaphoreWait, 1> waits {};
    std::array<rtpt::SemaphoreSignal, 1> signals {};
    rtpt::SubmissionSync sync {};

    if(!m_Options.headless)
    {
      waits[0]   = { .semaphore = acquired.imageAvailable, .stages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT };
      signals[0] = { .semaphore = acquired.renderFinished, .stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
      sync       = { .waits = waits, .signals = signals };
    }

    const rtpt::CompletionPoint completion = m_Execution.SubmitFrame(sync);

    // Presentation
    // An out-of-date or suboptimal swapchain is only marked for recreation here. Recreating it on the spot would skip the UI image count and viewport updates, so the frame setup at the top of the next frame recreates it together with them.
    // Present marks the swapchain itself when it reports out of date or suboptimal; an acquisition that was suboptimal but presented cleanly is marked explicitly.

    if(!m_Options.headless)
    {
      m_Swapchain.CommitSubmission(acquired, completion);

      if(m_Swapchain.Present(m_Device.PresentQueue(), acquired) == rtpt::SwapchainStatus::Recreate || acquired.suboptimal)
      {
        m_Swapchain.RequestRecreate();
      }
    }

    return true;
  }
  catch(...)
  {
    if(m_Execution.HasActiveFrame()) m_Execution.CancelFrame();
    if(acquired) m_Swapchain.Cancel(acquired);
    throw;
  }
}

void Application::RenderScene(const rtpt::FrameContext& frame)
{
  // Camera
  // The scene buffer carries this frame's camera to every renderer, so it is updated before any of them records.

  const glm::mat4 view       = m_Camera.View();
  const glm::mat4 projection = m_Camera.Projection();

  m_SceneRuntime->UpdateSceneBuffer(frame.commands, view, projection, m_Camera.State().eye, m_Targets.Extent());

  // Renderer
  // Whichever renderer is active writes the HDR target. The path tracers also receive the frame slot index and the frame time NRD adapts its history to.
  // Headless runs report a fixed frame time so captures are reproducible; interactive runs report zero so NRD times frames itself.

  const float frameTimeMilliseconds = m_Options.headless ? kHeadlessFrameTimeMilliseconds : 0.0f;

  if(m_RenderMode == RenderMode::ePathTracing)
  {
    m_PathTracer->Render({ .cmd = frame.commands, .sceneResource = &m_SceneRuntime->GetSceneResource(), .sceneInfo = &m_SceneRuntime->GetSceneInfo(), .topLevelAS = &m_SceneRuntime->GetTopLevelAccelerationStructure(), .output = m_Targets.Hdr(), .frameSlot = frame.slot.index, .frameTimeMilliseconds = frameTimeMilliseconds });
  }
  else if(m_RenderMode == RenderMode::eReSTIRPTEnhanced)
  {
    m_ReSTIRPT->Render({ .cmd = frame.commands, .sceneResource = &m_SceneRuntime->GetSceneResource(), .sceneInfo = &m_SceneRuntime->GetSceneInfo(), .topLevelAS = &m_SceneRuntime->GetTopLevelAccelerationStructure(), .output = m_Targets.Hdr(), .frameSlot = frame.slot.index, .frameTimeMilliseconds = frameTimeMilliseconds });
  }
  else
  {
    m_Raster->Render({ .cmd = frame.commands, .sceneResource = &m_SceneRuntime->GetSceneResource(), .sceneInfo = &m_SceneRuntime->GetSceneInfo(), .viewMatrix = view, .projectionMatrix = projection, .metallicRoughnessOverride = m_MetallicRoughnessOverride, .colorTarget = m_Targets.Hdr(), .depthTarget = m_Targets.Depth() });
  }
}

void Application::PostProcess(VkCommandBuffer commandBuffer)
{
  // LDR layout
  // The LDR target's previous state depends on how the last frame ended: headless frames leave it as the tonemapper's storage output,
  // interactive frames leave it shader-readable for the UI. A newly allocated image has no contents to keep, so it starts from the default scope.

  rtpt::ImageAccessScope before {};

  if(m_LdrInitialized)
  {
    before = m_Options.headless ? rtpt::ImageAccessScope { { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT }, VK_IMAGE_LAYOUT_GENERAL } : rtpt::ImageAccessScope { { VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT }, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
  }

  rtpt::CmdImageBarrier(commandBuffer, m_Targets.Ldr().image, kColorRange, before, { { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT }, VK_IMAGE_LAYOUT_GENERAL });

  // Tonemap
  // HDR is sampled in the GENERAL layout through the linear sampler, and LDR is written as a storage image.

  m_Tonemapper.Run(commandBuffer, m_Targets.Extent(), m_TonemapperSettings, m_Targets.Hdr().Descriptor(VK_IMAGE_LAYOUT_GENERAL, m_LinearSampler.sampler), m_Targets.Ldr().Descriptor(VK_IMAGE_LAYOUT_GENERAL));

  m_LdrInitialized = true;
}

void Application::RecordPresentation(VkCommandBuffer commandBuffer, const rtpt::AcquiredSwapchainImage& acquired)
{
  // Display texture
  // The UI samples the LDR target inside the Display window, so it moves from the tonemapper's storage write to shader-read.

  rtpt::CmdImageBarrier(commandBuffer, m_Targets.Ldr().image, kColorRange, { { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT }, VK_IMAGE_LAYOUT_GENERAL }, { { VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT }, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });

  // UI pass
  // The UI pass clears the whole swapchain image, so it transitions from the default scope without preserving contents.
  // The dark clear color shows wherever no window covers the framebuffer.

  rtpt::CmdImageBarrier(commandBuffer, acquired.image, kColorRange, {}, { { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT }, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });

  m_Ui.Record(commandBuffer, acquired.view, acquired.extent, VK_ATTACHMENT_LOAD_OP_CLEAR, VkClearColorValue { { 0.02F, 0.02F, 0.025F, 1.0F } });

  // Present
  // The finished image moves to the layout vkQueuePresentKHR requires.

  rtpt::CmdImageBarrier(commandBuffer, acquired.image, kColorRange, { { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT }, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, { { VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE }, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR });
}

void Application::InvalidateRenderHistory()
{
  // The systems only exist between InitializeRendererSystems and DestroyRendererSystems, so every call is guarded.

  if(m_SceneRuntime) m_SceneRuntime->InvalidateFrameHistory();
  if(m_PathTracer) m_PathTracer->InvalidateHistory();
  if(m_ReSTIRPT) m_ReSTIRPT->InvalidateHistory();
}

}  // namespace rtpt
