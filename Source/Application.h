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
  void CreateGraphicsDescriptorSetLayout();
  void CreateGraphicsPipelineLayout();
  void UpdateTextures();
  VkShaderModuleCreateInfo CompileSlangShader(const std::filesystem::path& filename,
                                              const std::span<const uint32_t>& spirvFallback);
  void CompileAndCreateGraphicsShaders();
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
  nvapp::Application*                    m_App = nullptr;  // Owning application
  static constexpr uint32_t              kMaxTextureDescriptors = 4096;
  nvvk::ResourceAllocator                m_Allocator;       // Vulkan allocator
  nvvk::StagingUploader                  m_StagingUploader; // Upload helper
  nvvk::SamplerPool                      m_SamplerPool;     // Sampler pool
  nvvk::GBuffer                          m_GBuffers;        // Offscreen buffers
  nvslang::SlangCompiler                 m_SlangCompiler;   // Hot reload compiler
  std::shared_ptr<nvutils::CameraManipulator> m_CameraManip = std::make_shared<nvutils::CameraManipulator>();
  nvvk::GraphicsPipelineState            m_DynamicPipeline; // Dynamic pipeline state
  nvvk::DescriptorPack                   m_DescPack;        // Descriptor pack for textures
  VkPipelineLayout                       m_GraphicPipelineLayout = VK_NULL_HANDLE;
  VkShaderEXT                            m_VertexShader = VK_NULL_HANDLE;
  VkShaderEXT                            m_FragmentShader = VK_NULL_HANDLE;
  std::vector<nvsamples::AssetEntry>     m_ModelAssets;
  std::vector<nvsamples::AssetEntry>     m_HdriAssets;
  std::vector<nvsamples::SceneDefinition> m_SceneDefinitions;
  RenderMode                             m_RenderMode = RenderMode::eReSTIRDI;
  size_t                                 m_SelectedSceneIndex = 0;
  size_t                                 m_SelectedHdriIndex = 0;
  bool                                   m_SceneReloadRequested = false;
  bool                                   m_HdriReloadRequested = false;
  nvshaders::SkySimple                   m_SkySimple;      // Sky compute
  nvshaders::Tonemapper                  m_Tonemapper;     // Tonemapper compute
  shaderio::TonemapperData               m_TonemapperData; // Tonemapper parameters
  glm::vec2                              m_MetallicRoughnessOverride = {-0.01f, -0.01f}; // UI overrides
  std::unique_ptr<nvsamples::SceneAssetCatalog> m_SceneAssetCatalog; // Asset discovery helper
  std::unique_ptr<nvsamples::PathTracer>        m_PathTracer;        // Ray tracing renderer
  std::unique_ptr<nvsamples::ReSTIRDIRenderer>  m_ReSTIRDI;          // Reservoir-based direct illumination renderer
  std::unique_ptr<nvsamples::SceneResolver>     m_SceneResolver;     // Scene selection resolver
  std::unique_ptr<nvsamples::SceneRenderer>     m_SceneRenderer;     // Raster scene renderer
  std::unique_ptr<nvsamples::SceneRuntime>      m_SceneRuntime;      // GPU scene runtime owner
  nvsamples::ExperimentGpuTimer                 m_ExperimentGpuTimer; // Experiment-only timestamp queries
  std::shared_ptr<nvsamples::ExperimentController> m_ExperimentController;
};

// Creates the core renderer app element. Main owns only the interface pointer.
std::shared_ptr<nvapp::IAppElement> CreateApplicationElement(
    const std::shared_ptr<nvutils::CameraManipulator>& cameraManip);

}  // namespace nvsamples



