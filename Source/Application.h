#pragma once

// Application module role:
// - Exposes the runtime app element used by Main.cpp.
// - Keeps the heavy rendering/runtime implementation out of the bootstrap entry point.

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nvapp/application.hpp>
#include <nvshaders_host/sky.hpp>
#include <nvshaders_host/tonemapper.hpp>
#include <nvslang/slang.hpp>
#include <nvutils/camera_manipulator.hpp>
#include <nvvk/descriptors.hpp>
#include <nvvk/gbuffers.hpp>
#include <nvvk/graphics_pipeline.hpp>
#include <nvvk/resource_allocator.hpp>
#include <nvvk/sampler_pool.hpp>
#include <nvvk/staging.hpp>

#include <glm/vec2.hpp>

#include "Experiments/ExperimentTypes.h"
#include "Experiments/ExperimentGpuTimer.h"
#include "PathTracing/PathTracer.h"
#include "PathTracing/ReSTIR/ReSTIRDIRenderer.h"
#include "Scene/SceneAssetCatalog.h"
#include "Scene/SceneResolver.h"
#include "Scene/SceneRenderer.h"
#include "Scene/SceneRuntime.h"
#include "Shaders/ShaderIo.h"

namespace nvsamples
{
class ExperimentController;

// Core application element role:
// - Owns rendering/runtime systems and per-frame orchestration.
// - Keeps scene/UI/rendering logic isolated from Main.cpp bootstrap code.
class Application : public nvapp::IAppElement
{
  enum class RenderMode
  {
    eRasterizer = 0,
    ePathTracing,
    eReSTIRDI,
  };

  enum
  {
    eImgRendered,
    eImgTonemapped,
  };

public:
  explicit Application(const std::shared_ptr<nvutils::CameraManipulator>& cameraManip);
  ~Application() override;

  void SetExperimentController(std::shared_ptr<ExperimentController> experimentController);

  void onAttach(nvapp::Application* app) override;
  void onDetach() override;
  void onUIRender() override;
  void onResize(VkCommandBuffer cmd, const VkExtent2D& size) override;
  void onRender(VkCommandBuffer cmd) override;
  void onUIMenu() override;
  void onLastHeadlessFrame() override;

  std::shared_ptr<nvutils::CameraManipulator> GetCameraManipulator() const;
  void ApplyExperimentRun(const ExperimentRun& run);
  void SetExperimentCamera(const ExperimentCamera& camera);
  void SaveExperimentImage(const std::filesystem::path& outputPath);
  void RequestExperimentClose();
  uint32_t GetExperimentAccumulatedFrameCount() const;
  std::optional<ExperimentGpuTimings> ReadLastExperimentGpuTimings();

private:
  void DiscoverAssets();
  void RebuildSceneFromSelection();
  void PostProcess(VkCommandBuffer cmd);
  void CreateScene(bool resetCamera);
  void CreateRasterDescriptorSetLayout();
  void CreateRasterPipelineLayout();
  void UpdateTextures();
  VkShaderModuleCreateInfo CompileSlangShader(const std::filesystem::path& filename,
                                              const std::span<const uint32_t>& spirvFallback);
  void CompileAndCreateRasterShaders();
  void UpdateSceneBuffer(VkCommandBuffer cmd);
  void RasterScene(VkCommandBuffer cmd);
  void PathTraceScene(VkCommandBuffer cmd);
  void ReSTIRDIScene(VkCommandBuffer cmd);
  void InvalidateRenderHistory();
  bool SelectSceneForExperiment(const std::string& sceneLabel);
  bool SelectHdriForExperiment(const std::string& hdriLabelOrPath);
  void ApplyExperimentEnvironment(const ExperimentEnvironment& environment);
  void ApplyExperimentPathTracerSettings(const ExperimentPathTracerSettings& settings);
  void ApplyExperimentReSTIRSettings(const ExperimentReSTIRSettings& settings);
  void BeginExperimentGpuTiming(VkCommandBuffer cmd);
  void MarkExperimentRendererStart(VkCommandBuffer cmd);
  void MarkExperimentRendererEnd(VkCommandBuffer cmd);
  void EndExperimentGpuTiming(VkCommandBuffer cmd);
  bool IsPathTracerRenderMode() const;
  bool IsReSTIRDIRenderMode() const;

private:
  static constexpr uint32_t              kMaxTextureDescriptors = 4096;

  // nvapp gives Application the frame callbacks; this pointer is borrowed for the app lifetime.
  nvapp::Application*                    m_App = nullptr;

  // Shared Vulkan services used by scene upload, raster preview, path tracing, and ReSTIR DI.
  nvvk::ResourceAllocator                m_Allocator;
  nvvk::StagingUploader                  m_StagingUploader;
  nvvk::SamplerPool                      m_SamplerPool;
  nvvk::GBuffer                          m_GBuffers;
  nvslang::SlangCompiler                 m_SlangCompiler;
  std::shared_ptr<nvutils::CameraManipulator> m_CameraManip = std::make_shared<nvutils::CameraManipulator>();

  // Raster preview owns its descriptor layout and shader objects here; ray tracing renderers own theirs.
  nvvk::GraphicsPipelineState            m_RasterDynamicPipeline;
  nvvk::DescriptorPack                   m_RasterDescPack;
  VkPipelineLayout                       m_RasterPipelineLayout = VK_NULL_HANDLE;
  VkShaderEXT                            m_VertexShader = VK_NULL_HANDLE;
  VkShaderEXT                            m_FragmentShader = VK_NULL_HANDLE;

  // UI selection state. SceneRuntime receives the resolved scene and owns the uploaded GPU data.
  std::vector<nvsamples::AssetEntry>     m_ModelAssets;
  std::vector<nvsamples::AssetEntry>     m_HdriAssets;
  std::vector<nvsamples::SceneDefinition> m_SceneDefinitions;
  RenderMode                             m_RenderMode = RenderMode::eReSTIRDI;
  size_t                                 m_SelectedSceneIndex = 0;
  size_t                                 m_SelectedHdriIndex = 0;
  bool                                   m_SceneReloadRequested = false;
  bool                                   m_HdriReloadRequested = false;
  // Full-frame post processing after whichever renderer wrote eImgRendered.
  nvshaders::SkySimple                   m_SkySimple;
  nvshaders::Tonemapper                  m_Tonemapper;
  shaderio::TonemapperData               m_TonemapperData;
  glm::vec2                              m_MetallicRoughnessOverride = {-0.01f, -0.01f};

  // Application wires these systems together but keeps their responsibilities separate.
  std::unique_ptr<nvsamples::SceneAssetCatalog> m_SceneAssetCatalog;
  std::unique_ptr<nvsamples::PathTracer>        m_PathTracer;
  std::unique_ptr<nvsamples::ReSTIRDIRenderer>  m_ReSTIRDI;
  std::unique_ptr<nvsamples::SceneResolver>     m_SceneResolver;
  std::unique_ptr<nvsamples::SceneRenderer>     m_SceneRenderer;
  std::unique_ptr<nvsamples::SceneRuntime>      m_SceneRuntime;

  // Headless experiments reuse the same renderer code path but add scripted state and GPU timestamps.
  nvsamples::ExperimentGpuTimer                 m_ExperimentGpuTimer;
  std::shared_ptr<nvsamples::ExperimentController> m_ExperimentController;
};

// Creates the core renderer app element. Main owns only the interface pointer.
std::shared_ptr<nvapp::IAppElement> CreateApplicationElement(
    const std::shared_ptr<nvutils::CameraManipulator>& cameraManip);

}  // namespace nvsamples


