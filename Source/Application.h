#pragma once

#include <memory>
#include <vector>

#include "ApplicationOptions.h"
#include "Camera/CameraController.h"
#include "Framework/Platform/Window.h"
#include "Framework/Presentation/UiRenderer.h"
#include "Framework/Presentation/WindowTitle.h"
#include "Framework/Vulkan/Descriptors.h"
#include "Framework/Vulkan/GpuExecution.h"
#include "Framework/Vulkan/GpuProfiler.h"
#include "Framework/Vulkan/GpuResources.h"
#include "Framework/Vulkan/PresentationSurface.h"
#include "Framework/Vulkan/Swapchain.h"
#include "Framework/Vulkan/UploadContext.h"
#include "Framework/Vulkan/VulkanDevice.h"
#include "Framework/Vulkan/VulkanInstance.h"
#include "PathTracing/PathTracer.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTRenderer.h"
#include "PostProcessing/Tonemapper.h"
#include "Rendering/FrameTimingStatistics.h"
#include "Rendering/RasterRenderer.h"
#include "Rendering/ViewportTargets.h"
#include "Sampling/SpatiotemporalBlueNoise.h"
#include "Scene/SceneAssetCatalog.h"
#include "Scene/SceneResolver.h"
#include "Scene/SceneRuntime.h"
#include "Scene/SceneTypes.h"

namespace rtpt
{

// Application
// Top-level orchestrator: owns the window, the Vulkan context, the viewport targets, the scene, and every renderer, and drives the frame loop.
// Interactive runs draw a docked ImGui layout and present through a swapchain. Headless runs skip the window, render a fixed number of frames, and can write a capture.
// Shutdown releases the systems explicitly in reverse creation order instead of relying on member destruction order.

class Application
{
public:

  // Stores the options. Window and Vulkan setup is deferred to Run.
  explicit Application(ApplicationOptions options);

  // Releases everything through Shutdown.
  ~Application();

  // Initializes, then renders until the window closes (interactive) or until frameCount frames are done and captured (headless). Returns the exit code.
  int Run();

private:

  // Frame slots GpuExecution cycles through. BeginFrame waits for a slot's previous submission, and the renderers receive this count for their per-slot state.
  static constexpr uint32_t kFrameSlotCount = 3;

  // Capacity of the scene texture arrays in every renderer's descriptor set. SceneRuntime ignores textures beyond it with a warning.
  static constexpr uint32_t kMaxTextureDescriptors = 4096;

  // Timestamp scopes one frame may open. The busiest frame - ReSTIR PT with the sorted pre-pass and NRD, plus the frame and post scopes - opens 13, so this leaves room for new passes.
  static constexpr uint32_t kMaxProfileScopesPerFrame = 32;

  // Frames the interactive Profiler section summarizes. Two seconds at 60 Hz is long enough to steady the percentiles and short enough to follow a settings change.
  static constexpr uint32_t kInteractiveTimingWindowFrames = 120;

  // Brings up every system in dependency order. Any exception runs Shutdown before it propagates.
  void Initialize();

  // Releases every system in reverse creation order, containing individual failures so teardown always completes.
  void Shutdown() noexcept;

  // Creates blue noise, the scene systems, the three renderers, and the tonemap pipeline.
  void InitializeRendererSystems();

  void DestroyRendererSystems() noexcept;

  // Fills the model, HDRI, and scene catalogs and logs catalog warnings.
  void DiscoverAssets();

  // Applies command-line overrides that need the discovered catalog.
  void ApplyStartupOptions();

  // Rebuilds GPU scene data for the selected scene and HDRI, then refreshes texture descriptors and discards render history.
  void CreateScene(bool resetCamera);

  // Writes the scene textures into every renderer's descriptor set.
  void UpdateTextures();

  // Reallocates the viewport targets and re-registers the display texture. Empty or unchanged extents are ignored.
  void ResizeViewport(VkExtent2D extent);

  // Feeds mouse, wheel, and fly keys to the camera while the Display window is hovered, focused, or mid-drag.
  void UpdateCameraInput();

  void DrawUi();

  // Runs one frame. Returns false when the frame was skipped because no swapchain image was available.
  bool RenderFrame();

  // Writes the HDR target with the active renderer.
  void RenderScene(const rtpt::FrameContext& frame);

  // Tonemaps HDR into the LDR target.
  void PostProcess(const rtpt::FrameContext& frame);

  // Draws the UI, including the LDR image, into the acquired swapchain image and leaves it ready to present.
  void RecordPresentation(const rtpt::FrameContext& frame, const rtpt::AcquiredSwapchainImage& acquired);

  // Reads the timestamps of the last frame recorded on the slot into the frame timing statistics. That frame's submission must have completed.
  void CollectProfileResults(rtpt::FrameSlot slot);

  // The profiler renderers receive: null when the device cannot record timestamps.
  rtpt::GpuProfiler* ActiveProfiler();

  // Resolve mode of the active renderer. The rasterizer has no resolve step and reports Off.
  RenderResolveMode ActiveResolveMode() const;

  // Writes the --profile-output JSON and prints the timing table at the end of a headless run.
  void WriteProfileReport();

  // Discards temporal and accumulated history in the scene runtime and both path tracers.
  void InvalidateRenderHistory();

  // Command-line configuration, fixed for the life of the application.
  ApplicationOptions m_Options;

  // Set once Initialize completes; guards against initializing twice.
  bool                m_Initialized = false;

  // Whether the LDR target has been written since allocation, which decides the layout PostProcess transitions it from.
  bool                m_LdrInitialized = false;

  // The Display window was hovered when the UI was drawn. Gates starting drags and wheel input.
  bool                m_DisplayHovered = false;

  // The Display window had focus when the UI was drawn. Gates fly keys.
  bool                m_DisplayFocused = false;

  // Pixel size of the Display window's image area when the UI was drawn, reported in the window title. Zero while that window is collapsed or hidden.
  VkExtent2D          m_DisplaySize {};

  // A camera drag is in progress with the cursor captured. It continues after the pointer leaves the Display window.
  bool                m_CameraDragActive = false;

  // GLFW window. Never initialized in headless runs.
  rtpt::Window              m_Window;

  // Vulkan instance, with validation in debug builds.
  rtpt::VulkanInstance      m_Instance;

  // Window surface. Its handle stays null in headless runs and is passed to the device as-is.
  rtpt::PresentationSurface m_Surface;

  // Physical and logical device with the selected queues.
  rtpt::VulkanDevice        m_Device;

  // Frame command buffers, submission, completion tracking, and deferred resource retirement.
  rtpt::GpuExecution        m_Execution;

  // Timestamp queries for per-pass GPU timing. Stays unready on devices whose render queue cannot record timestamps, and everything else runs unchanged.
  rtpt::GpuProfiler         m_Profiler;

  // GPU scope and CPU frame time distributions: every sample after warm-up in headless runs, a rolling window interactively.
  FrameTimingStatistics     m_FrameTiming;

  // Slot of the most recently submitted frame. The end of a headless run reads the unread slots oldest first starting after it, which keeps frame order.
  rtpt::FrameSlot           m_LastFrameSlot {};

  // Allocator for buffers, images, and samplers.
  rtpt::ResourceAllocator   m_Resources;

  // Staging uploads for scene and blue noise data.
  rtpt::UploadContext       m_Uploads;

  // Presentation swapchain. Unused in headless runs.
  rtpt::Swapchain           m_Swapchain;

  // ImGui context and its Vulkan backend. Unused in headless runs.
  rtpt::UiRenderer          m_Ui;
  // Frame rate and viewport size in the title bar, refreshed once per second while interactive.
  rtpt::WindowTitle         m_WindowTitle;

  // HDR, LDR, and depth images at viewport resolution.
  rtpt::ViewportTargets     m_Targets;

  // Linear clamp-to-edge sampler for the tonemapper's HDR input.
  rtpt::Sampler             m_LinearSampler;

  // Nearest filtering keeps viewport scaling from periodically averaging away different amounts of path-tracing noise.
  rtpt::Sampler             m_ViewportSampler;

  // Spatiotemporal blue noise handed to the reference path tracer.
  rtpt::SpatiotemporalBlueNoise m_BlueNoise;

  // ImGui texture for the LDR target. Registered again whenever the targets are reallocated.
  VkDescriptorSet           m_ViewportTexture = VK_NULL_HANDLE;

  // Interactive camera every renderer reads.
  rtpt::CameraController m_Camera;

  // Debug override of every material's metallic and roughness, edited in the Settings window and copied into the scene uniform each frame.
  rtpt::MaterialDebugOverride m_MaterialOverride;

  // Renderer that writes the HDR target each frame.
  RenderMode             m_RenderMode = RenderMode::eReSTIRPTEnhanced;

  // GPU scene data: geometry, textures, acceleration structures, and the per-frame scene buffer. Like the renderers below, it is heap-held so it exists only between InitializeRendererSystems and DestroyRendererSystems.
  std::unique_ptr<SceneRuntime>      m_SceneRuntime;

  // Rasterizer preview.
  std::unique_ptr<RasterRenderer>    m_Raster;

  // Reference path tracer.
  std::unique_ptr<PathTracer>        m_PathTracer;

  // ReSTIR PT Enhanced renderer.
  std::unique_ptr<ReSTIRPTRenderer>  m_ReSTIRPT;

  // Model assets found by discovery.
  std::vector<AssetEntry>      m_ModelAssets;

  // HDRI assets offered in the HDRI combo.
  std::vector<AssetEntry>      m_HdriAssets;

  // Scene catalog offered in the Scene combo and indexed by --scene-index.
  std::vector<SceneDefinition> m_SceneDefinitions;

  // Selected scene. CreateScene replaces it with the index the resolver actually used.
  size_t                       m_SelectedSceneIndex = 0;

  // Selected HDRI. CreateScene replaces it with the index the resolver actually used.
  size_t                       m_SelectedHdriIndex = 0;

  // Set by the UI; RenderFrame rebuilds the scene before the next frame starts recording.
  bool                         m_SceneReloadRequested = false;

  // Set by the UI when the HDRI changes; handled together with scene reloads.
  bool                         m_HdriReloadRequested = false;

  // HDR to LDR pass, run every frame whichever renderer is active.
  Tonemapper           m_Tonemapper;

  // Tonemapper controls edited in the UI and recorded in capture metadata.
  TonemapperSettings   m_TonemapperSettings;
};

}  // namespace rtpt
