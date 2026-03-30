/*
 * Copyright (c) 2023-2026, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2023-2026, NVIDIA CORPORATION.
 * SPDX-License-Identifier: Apache-2.0
 */

// RealTimePathTracing - Rasterizer
// - Simple rasterized GLTF scene (teapot + plane)
// - Offscreen HDR render + tonemapping
// - Slang hot reload (F5) with precompiled fallback

// Enable the use of Nsight Aftermath for crash tracking and shader debugging
// #define USE_NSIGHT_AFTERMATH

#define TINYGLTF_IMPLEMENTATION         // TinyGLTF implementation (exactly once)
#define STB_IMAGE_IMPLEMENTATION        // stb_image implementation (exactly once)
#define STB_IMAGE_WRITE_IMPLEMENTATION  // stb_image_write implementation (exactly once)
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1  // VMA: use dynamic Vulkan functions
#define VMA_IMPLEMENTATION              // VMA implementation (exactly once)

#define VMA_LEAK_LOG_FORMAT(format, ...)                                                                               \
  {                                                                                                                    \
    printf((format), __VA_ARGS__);                                                                                     \
    printf("\n");                                                                                                      \
  }

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui/backends/imgui_impl_vulkan.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <memory>
#include <span>
#include <string>

#include "Application.h"

#include "Shaders/ShaderIo.h"

// Pre-compiled shaders (generated at build time).
#include "_autogen/sky_simple.slang.h"   // From nvpro_core2
#include "_autogen/tonemapper.slang.h"   // From nvpro_core2
#include "_autogen/Rasterizer.slang.h"   // From Source/Shaders/Rasterizer.slang

#include <nvaftermath/aftermath.hpp>
#include <nvapp/application.hpp>
#include <nvapp/elem_camera.hpp>
#include <nvapp/elem_default_menu.hpp>
#include <nvapp/elem_default_title.hpp>
#include <nvgui/camera.hpp>
#include <nvgui/sky.hpp>
#include <nvgui/tonemapper.hpp>
#include <nvshaders_host/sky.hpp>
#include <nvshaders_host/tonemapper.hpp>
#include <nvslang/slang.hpp>
#include <nvutils/camera_manipulator.hpp>
#include <nvutils/logger.hpp>
#include <nvutils/parameter_parser.hpp>
#include <nvutils/timers.hpp>
#include <nvvk/context.hpp>
#include <nvvk/default_structs.hpp>
#include <nvvk/descriptors.hpp>
#include <nvvk/formats.hpp>
#include <nvvk/gbuffers.hpp>
#include <nvvk/graphics_pipeline.hpp>
#include <nvvk/sampler_pool.hpp>
#include <nvvk/validation_settings.hpp>

#include "Common/GltfUtils.hpp"
#include "Common/PathUtils.hpp"
#include "Common/Utils.hpp"
#include "Scene/SceneAssetCatalog.h"
#include "Scene/SceneResolver.h"
#include "Scene/SceneRenderer.h"
#include "Scene/SceneRuntime.h"

// ---------------------------------------------------------------------------------------------------------------------
// Rasterizer app element
//

namespace nvsamples
{
namespace
{

bool DrawReSTIRCommonControls(ReSTIRMethodSettings& settings, const char* resamplingLabel)
{
  bool changed = false;

  bool accumulate = settings.accumulate;
  if(ImGui::Checkbox("Accumulate", &accumulate))
  {
    settings.accumulate = accumulate;
    changed             = true;
  }

  int resamplingMode = static_cast<int>(settings.resamplingMode);
  const char* resamplingModes[] = {"None", "Temporal", "Spatial", "Temporal + Spatial"};
  if(ImGui::Combo(resamplingLabel, &resamplingMode, resamplingModes, IM_ARRAYSIZE(resamplingModes)))
  {
    settings.resamplingMode = static_cast<nvsamples::ReSTIRResamplingMode>(resamplingMode);
    changed                 = true;
  }

  return changed;
}

bool DrawBounceLimitControl(const char* label, uint32_t& settingValue, uint32_t bounceLimit)
{
  int value = static_cast<int>(settingValue);
  if(!ImGui::SliderInt(label, &value, 0, static_cast<int>(bounceLimit)))
  {
    return false;
  }

  settingValue = static_cast<uint32_t>(value);
  return true;
}

void DrawTransmissionBounceHint(uint32_t bounceCount)
{
  if(bounceCount < 2)
  {
    ImGui::TextWrapped(
        "Solid transmissive objects need at least 2 bounces to show through-lighting: one refraction to enter the shape and one more to exit it.");
  }
}

bool DrawReSTIRTemporalControls(shaderio::ReSTIRTemporalResamplingParameters& settings)
{
  bool changed = false;

  int maxHistoryLength = static_cast<int>(settings.maxHistoryLength);
  if(ImGui::SliderInt("Max History Length", &maxHistoryLength, 1, 64))
  {
    settings.maxHistoryLength = static_cast<uint32_t>(maxHistoryLength);
    changed                   = true;
  }

  changed |= ImGui::SliderFloat("Temporal Depth Threshold", &settings.depthThreshold, 0.0f, 1.0f, "%.3f");
  changed |= ImGui::SliderFloat("Temporal Normal Threshold", &settings.normalThreshold, 0.0f, 1.0f, "%.2f");
  return changed;
}

bool DrawReSTIRTemporalControls(ReSTIRDITemporalResamplingParameters& settings)
{
  bool changed = false;

  int maxHistoryLength = static_cast<int>(settings.maxHistoryLength);
  if(ImGui::SliderInt("Max History Length", &maxHistoryLength, 1, 64))
  {
    settings.maxHistoryLength = static_cast<uint32_t>(maxHistoryLength);
    changed                   = true;
  }

  changed |= ImGui::SliderFloat("Temporal Depth Threshold", &settings.depthThreshold, 0.0f, 1.0f, "%.3f");
  changed |= ImGui::SliderFloat("Temporal Normal Threshold", &settings.normalThreshold, 0.0f, 1.0f, "%.2f");
  return changed;
}

bool DrawReSTIRTemporalControls(ReSTIRGITemporalResamplingParameters& settings)
{
  bool changed = false;

  int maxHistoryLength = static_cast<int>(settings.maxHistoryLength);
  if(ImGui::SliderInt("Max History Length", &maxHistoryLength, 1, 64))
  {
    settings.maxHistoryLength = static_cast<uint32_t>(maxHistoryLength);
    changed                   = true;
  }

  int maxReservoirAge = static_cast<int>(settings.maxReservoirAge);
  if(ImGui::SliderInt("Max Reservoir Age", &maxReservoirAge, 1, 64))
  {
    settings.maxReservoirAge = static_cast<uint32_t>(maxReservoirAge);
    changed                  = true;
  }

  bool enableFallbackSampling = (settings.enableFallbackSampling != 0u);
  if(ImGui::Checkbox("Fallback Sampling", &enableFallbackSampling))
  {
    settings.enableFallbackSampling = enableFallbackSampling ? 1u : 0u;
    changed                         = true;
  }

  bool enablePermutationSampling = (settings.enablePermutationSampling != 0u);
  if(ImGui::Checkbox("Permutation Sampling", &enablePermutationSampling))
  {
    settings.enablePermutationSampling = enablePermutationSampling ? 1u : 0u;
    changed                            = true;
  }

  changed |= ImGui::SliderFloat("Temporal Depth Threshold", &settings.depthThreshold, 0.0f, 1.0f, "%.3f");
  changed |= ImGui::SliderFloat("Temporal Normal Threshold", &settings.normalThreshold, 0.0f, 1.0f, "%.2f");
  return changed;
}

bool DrawReSTIRSpatialControls(shaderio::ReSTIRSpatialResamplingParameters& settings, float maxRadius)
{
  bool changed = false;

  int sampleCount = static_cast<int>(settings.numSpatialSamples);
  if(ImGui::SliderInt("Spatial Sample Count", &sampleCount, 1, 32))
  {
    settings.numSpatialSamples = static_cast<uint32_t>(sampleCount);
    changed                    = true;
  }

  changed |= ImGui::SliderFloat("Spatial Radius", &settings.samplingRadius, 0.0f, maxRadius, "%.1f");
  changed |= ImGui::SliderFloat("Spatial Depth Threshold", &settings.depthThreshold, 0.0f, 1.0f, "%.3f");
  changed |= ImGui::SliderFloat("Spatial Normal Threshold", &settings.normalThreshold, 0.0f, 1.0f, "%.2f");
  return changed;
}

bool DrawReSTIRSpatialControls(ReSTIRDISpatialResamplingParameters& settings, float maxRadius)
{
  bool changed = false;

  int sampleCount = static_cast<int>(settings.numSamples);
  if(ImGui::SliderInt("Spatial Sample Count", &sampleCount, 1, 32))
  {
    settings.numSamples = static_cast<uint32_t>(sampleCount);
    changed             = true;
  }

  changed |= ImGui::SliderFloat("Spatial Radius", &settings.samplingRadius, 0.0f, maxRadius, "%.1f");
  changed |= ImGui::SliderFloat("Spatial Depth Threshold", &settings.depthThreshold, 0.0f, 1.0f, "%.3f");
  changed |= ImGui::SliderFloat("Spatial Normal Threshold", &settings.normalThreshold, 0.0f, 1.0f, "%.2f");
  return changed;
}

bool DrawReSTIRSpatialControls(ReSTIRGISpatialResamplingParameters& settings, float maxRadius)
{
  bool changed = false;

  int sampleCount = static_cast<int>(settings.numSamples);
  if(ImGui::SliderInt("Spatial Sample Count", &sampleCount, 1, 32))
  {
    settings.numSamples = static_cast<uint32_t>(sampleCount);
    changed             = true;
  }

  changed |= ImGui::SliderFloat("Spatial Radius", &settings.samplingRadius, 0.0f, maxRadius, "%.1f");
  changed |= ImGui::SliderFloat("Spatial Depth Threshold", &settings.depthThreshold, 0.0f, 1.0f, "%.3f");
  changed |= ImGui::SliderFloat("Spatial Normal Threshold", &settings.normalThreshold, 0.0f, 1.0f, "%.2f");
  return changed;
}

bool DrawReSTIRResamplingSection(const char* treeLabel, shaderio::ReSTIRTemporalResamplingParameters& temporalSettings,
                                 shaderio::ReSTIRSpatialResamplingParameters& spatialSettings, float maxRadius)
{
  if(!ImGui::TreeNodeEx(treeLabel))
  {
    return false;
  }

  bool changed = false;
  changed |= DrawReSTIRTemporalControls(temporalSettings);
  changed |= DrawReSTIRSpatialControls(spatialSettings, maxRadius);
  ImGui::TreePop();
  return changed;
}

bool DrawReSTIRResamplingSection(const char* treeLabel, ReSTIRDITemporalResamplingParameters& temporalSettings,
                                 ReSTIRDISpatialResamplingParameters& spatialSettings, float maxRadius)
{
  if(!ImGui::TreeNodeEx(treeLabel))
  {
    return false;
  }

  bool changed = false;
  changed |= DrawReSTIRTemporalControls(temporalSettings);
  changed |= DrawReSTIRSpatialControls(spatialSettings, maxRadius);
  ImGui::TreePop();
  return changed;
}

bool DrawReSTIRResamplingSection(const char* treeLabel, ReSTIRGITemporalResamplingParameters& temporalSettings,
                                 ReSTIRGISpatialResamplingParameters& spatialSettings, float maxRadius)
{
  if(!ImGui::TreeNodeEx(treeLabel))
  {
    return false;
  }

  bool changed = false;
  changed |= DrawReSTIRTemporalControls(temporalSettings);
  changed |= DrawReSTIRSpatialControls(spatialSettings, maxRadius);
  ImGui::TreePop();
  return changed;
}

void DrawReSTIRAccumulationStatus(uint32_t accumulatedFrames)
{
  ImGui::Text("Accumulated Frames: %u", accumulatedFrames);
}

void DrawReSTIRMethodFooter(const char* description, uint32_t accumulatedFrames)
{
  ImGui::TextWrapped("%s", description);
  DrawReSTIRAccumulationStatus(accumulatedFrames);
}

}  // namespace

Application::Application(const std::shared_ptr<nvutils::CameraManipulator>& cameraManip)
  {
    if(cameraManip)
    {
      m_CameraManip = cameraManip;
    }
  }
Application::~Application() = default;

void Application::onAttach(nvapp::Application* app)
  {
    m_App = app;

    // VMA allocator init.
    VmaAllocatorCreateInfo allocatorInfo = {
        .flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice   = app->getPhysicalDevice(),
        .device           = app->getDevice(),
        .instance         = app->getInstance(),
        .vulkanApiVersion = VK_API_VERSION_1_4,
    };
    m_Allocator.init(allocatorInfo);

    // Staging uploader uses the same allocator.
    m_StagingUploader.init(&m_Allocator, true);

    // Slang compiler init (hot reload).
    m_SlangCompiler.addSearchPaths(nvsamples::GetShaderDirs());
    m_SlangCompiler.defaultTarget();
    m_SlangCompiler.defaultOptions();
    m_SlangCompiler.addOption({slang::CompilerOptionName::DebugInformation,
                               {slang::CompilerOptionValueKind::Int, SLANG_DEBUG_INFO_LEVEL_MAXIMAL}});

#if defined(AFTERMATH_AVAILABLE)
    // Aftermath: register SPIR-V binaries for crash dumps.
    m_SlangCompiler.setCompileCallback(
        [&](const std::filesystem::path& sourceFile, const uint32_t* spirvCode, size_t spirvSize) {
          std::span<const uint32_t> data(spirvCode, spirvSize / sizeof(uint32_t));
          AftermathCrashTracker::getInstance().addShaderBinary(data);
        });
#endif

    // GBuffer sampler.
    m_SamplerPool.init(app->getDevice());
    VkSampler linearSampler{};
    NVVK_CHECK(m_SamplerPool.acquireSampler(linearSampler));
    NVVK_DBG_NAME(linearSampler);

    // GBuffer init (HDR render target + tonemapped output).
    nvvk::GBufferInitInfo gBufferInit = {
        .allocator      = &m_Allocator,
        .colorFormats   = {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM},
        .depthFormat    = nvvk::findDepthFormat(m_App->getPhysicalDevice()),
        .imageSampler   = linearSampler,
        .descriptorPool = m_App->getTextureDescriptorPool(),
    };
    m_GBuffers.init(gBufferInit);

    m_SceneAssetCatalog = std::make_unique<nvsamples::SceneAssetCatalog>();
    m_SceneResolver     = std::make_unique<nvsamples::SceneResolver>();
    m_SceneRenderer     = std::make_unique<nvsamples::SceneRenderer>();
    m_PathTracer        = std::make_unique<nvsamples::PathTracer>(nvsamples::PathTracer::CreateInfo{
        .app                   = m_App,
        .allocator             = &m_Allocator,
        .maxTextureDescriptors = kMaxTextureDescriptors,
    });
    m_ReSTIRDI         = std::make_unique<nvsamples::ReSTIRDIRenderer>(nvsamples::ReSTIRDIRenderer::CreateInfo{
        .app                   = m_App,
        .allocator             = &m_Allocator,
        .maxTextureDescriptors = kMaxTextureDescriptors,
    });
    m_ReSTIRGI         = std::make_unique<nvsamples::ReSTIRGIRenderer>(nvsamples::ReSTIRGIRenderer::CreateInfo{
        .app                   = m_App,
        .allocator             = &m_Allocator,
        .maxTextureDescriptors = kMaxTextureDescriptors,
    });
    m_ReSTIRPT         = std::make_unique<nvsamples::ReSTIRPTRenderer>(nvsamples::ReSTIRPTRenderer::CreateInfo{
        .app                   = m_App,
        .allocator             = &m_Allocator,
        .maxTextureDescriptors = kMaxTextureDescriptors,
    });
    m_SceneRuntime      = std::make_unique<nvsamples::SceneRuntime>(nvsamples::SceneRuntime::CreateInfo{
        .app             = m_App,
        .allocator       = &m_Allocator,
        .stagingUploader = &m_StagingUploader,
        .samplerPool     = &m_SamplerPool,
    });
    m_PathTracer->Initialize();
    m_ReSTIRDI->Initialize();
    m_ReSTIRGI->Initialize();
    m_ReSTIRPT->Initialize();
    DiscoverAssets();
    CreateScene(true);
    CreateGraphicsDescriptorSetLayout();
    CreateGraphicsPipelineLayout();
    CompileAndCreateGraphicsShaders();
    UpdateTextures();

    // Init sky + tonemapper from precompiled shaders.
    m_SkySimple.init(&m_Allocator, std::span(sky_simple_slang));
    m_Tonemapper.init(&m_Allocator, std::span(tonemapper_slang));
  }

void Application::onDetach()
  {
    NVVK_CHECK(vkQueueWaitIdle(m_App->getQueue(0).queue));

    VkDevice device = m_App->getDevice();

    m_DescPack.deinit();
    vkDestroyPipelineLayout(device, m_GraphicPipelineLayout, nullptr);
    vkDestroyShaderEXT(device, m_VertexShader, nullptr);
    vkDestroyShaderEXT(device, m_FragmentShader, nullptr);

    m_PathTracer->Destroy();
    m_ReSTIRDI->Destroy();
    m_ReSTIRGI->Destroy();
    m_ReSTIRPT->Destroy();
    m_SceneRuntime->Destroy();

    m_GBuffers.deinit();
    m_StagingUploader.deinit();
    m_SkySimple.deinit();
    m_Tonemapper.deinit();
    m_SamplerPool.deinit();
    m_Allocator.deinit();
  }

void Application::onUIRender()
  {
    namespace PE = nvgui::PropertyEditor;

    // Viewport.
    if(ImGui::Begin("Viewport"))
    {
      ImGui::Image(ImTextureID(m_GBuffers.getDescriptorSet(eImgTonemapped)), ImGui::GetContentRegionAvail());
    }
    ImGui::End();

    // Settings.
    if(ImGui::Begin("Settings"))
    {
      shaderio::GltfSceneInfo& sceneInfo = m_SceneRuntime->GetSceneInfo();
      bool                     invalidatePathTracingHistory = false;

      if(ImGui::CollapsingHeader("Renderer", ImGuiTreeNodeFlags_DefaultOpen))
      {
        int renderMode = static_cast<int>(m_RenderMode);
        const char* renderModes[] = {"Rasterizer", "Path Tracing", "Path Tracing ReSTIR DI", "Path Tracing ReSTIR GI",
                                     "Path Tracing ReSTIR PT"};
        if(ImGui::Combo("Mode", &renderMode, renderModes, IM_ARRAYSIZE(renderModes)))
        {
          m_RenderMode = static_cast<RenderMode>(renderMode);
          invalidatePathTracingHistory = true;
        }

        if(m_RenderMode == RenderMode::eRasterizer)
        {
          ImGui::TextWrapped("Rasterizer mode uses the existing graphics pipeline path.");
        }
        else if(m_RenderMode == RenderMode::ePathTracing)
        {
          if(m_PathTracer == nullptr || !m_PathTracer->IsReady())
          {
            ImGui::TextWrapped("Path tracing mode is present in the UI, but the renderer is not ready yet.");
          }
          else
          {
            nvsamples::PathTracer::Settings& pathTracingSettings = m_PathTracer->GetSettings();
            const uint32_t                   bounceLimit         = m_PathTracer->GetPipelineBounceLimit();

            bool accumulate = pathTracingSettings.accumulate;
            if(ImGui::Checkbox("Accumulate", &accumulate))
            {
              pathTracingSettings.accumulate = accumulate;
              invalidatePathTracingHistory   = true;
            }

            if(DrawBounceLimitControl("Max Bounces", pathTracingSettings.maxBounces, bounceLimit))
            {
              invalidatePathTracingHistory   = true;
            }

            DrawTransmissionBounceHint(pathTracingSettings.maxBounces);

            ImGui::SameLine();
            if(ImGui::Button("Reset Accumulation"))
            {
              invalidatePathTracingHistory = true;
            }

            ImGui::Text("Accumulated Frames: %u", m_PathTracer->GetAccumulatedFrameCount());
          }
        }
        else if(m_RenderMode == RenderMode::ePathTracingReSTIRDI)
        {
          if(m_ReSTIRDI == nullptr || !m_ReSTIRDI->IsReady())
          {
            ImGui::TextWrapped("Path Tracing ReSTIR DI mode is present in the UI, but the renderer is not ready yet.");
          }
          else
          {
            nvsamples::ReSTIRDISettings& restirDiSettings = m_ReSTIRDI->GetSettings();

            invalidatePathTracingHistory |= DrawReSTIRCommonControls(restirDiSettings.common, "Resampling##ReSTIRDI_Mode");
            invalidatePathTracingHistory |= DrawReSTIRResamplingSection("Resampling##ReSTIRDI_Settings",
                                                                        restirDiSettings.temporalResampling,
                                                                        restirDiSettings.spatialResampling, 128.0f);

            if(ImGui::TreeNodeEx("Initial Sampling", ImGuiTreeNodeFlags_DefaultOpen))
            {
              int localLightSamples = static_cast<int>(restirDiSettings.initialSampling.numLocalLightSamples);
              if(ImGui::SliderInt("Emissive Light Samples", &localLightSamples, 0, 32))
              {
                restirDiSettings.initialSampling.numLocalLightSamples = static_cast<uint32_t>(localLightSamples);
                invalidatePathTracingHistory                           = true;
              }

              if(restirDiSettings.initialSampling.numInfiniteLightSamples != 0)
              {
                restirDiSettings.initialSampling.numInfiniteLightSamples = 0;
                invalidatePathTracingHistory                             = true;
              }

              int environmentSamples = static_cast<int>(restirDiSettings.initialSampling.numEnvironmentSamples);
              if(ImGui::SliderInt("Environment Samples", &environmentSamples, 0, 8))
              {
                restirDiSettings.initialSampling.numEnvironmentSamples = static_cast<uint32_t>(environmentSamples);
                invalidatePathTracingHistory                           = true;
              }

              int brdfSamples = static_cast<int>(restirDiSettings.initialSampling.numBrdfSamples);
              if(ImGui::SliderInt("BRDF Samples", &brdfSamples, 0, 8))
              {
                restirDiSettings.initialSampling.numBrdfSamples = static_cast<uint32_t>(brdfSamples);
                invalidatePathTracingHistory                     = true;
              }

              bool enableInitialVisibility = (restirDiSettings.initialSampling.enableInitialVisibility != 0);
              if(ImGui::Checkbox("Initial Visibility", &enableInitialVisibility))
              {
                restirDiSettings.initialSampling.enableInitialVisibility = enableInitialVisibility ? 1u : 0u;
                invalidatePathTracingHistory                              = true;
              }

              ImGui::TextDisabled("Initial candidates come from emissive triangles, the environment, and BRDF-guided rays.");

              ImGui::TreePop();
            }

            if(ImGui::TreeNodeEx("Final Visibility"))
            {
              bool enableFinalVisibility = (restirDiSettings.shading.enableFinalVisibility != 0);
              if(ImGui::Checkbox("Enable Final Visibility", &enableFinalVisibility))
              {
                restirDiSettings.shading.enableFinalVisibility = enableFinalVisibility ? 1u : 0u;
                invalidatePathTracingHistory                   = true;
              }

              ImGui::BeginDisabled(!enableFinalVisibility);
              bool reuseFinalVisibility = (restirDiSettings.shading.reuseFinalVisibility != 0);
              if(ImGui::Checkbox("Reuse Final Visibility", &reuseFinalVisibility))
              {
                restirDiSettings.shading.reuseFinalVisibility = reuseFinalVisibility ? 1u : 0u;
                invalidatePathTracingHistory                  = true;
              }

              int finalVisibilityMaxAge = static_cast<int>(restirDiSettings.shading.finalVisibilityMaxAge);
              if(ImGui::SliderInt("Final Visibility Max Age", &finalVisibilityMaxAge, 0, 16))
              {
                restirDiSettings.shading.finalVisibilityMaxAge = static_cast<uint32_t>(finalVisibilityMaxAge);
                invalidatePathTracingHistory                   = true;
              }

              invalidatePathTracingHistory |=
                  ImGui::SliderFloat("Final Visibility Max Distance", &restirDiSettings.shading.finalVisibilityMaxDistance, 0.0f, 64.0f, "%.2f");
              ImGui::EndDisabled();

              ImGui::TreePop();
            }

            if(ImGui::TreeNodeEx("Continuation"))
            {
              const uint32_t bounceLimit = m_ReSTIRDI->GetPipelineBounceLimit();
              if(DrawBounceLimitControl("Continuation Max Bounces", restirDiSettings.continuationMaxBounces, bounceLimit))
              {
                invalidatePathTracingHistory            = true;
              }

              ImGui::TextDisabled("Direct lighting comes from DI reservoirs, while the continuation path covers reflections and indirect transport.");
              ImGui::TreePop();
            }

            DrawReSTIRMethodFooter(
                "This DI mode uses ReSTIR direct-light reservoirs and a path-traced continuation for reflections and indirect transport.",
              m_ReSTIRDI->GetAccumulatedFrameCount());
          }
        }
        else if(m_RenderMode == RenderMode::ePathTracingReSTIRGI)
        {
          if(m_ReSTIRGI == nullptr || !m_ReSTIRGI->IsReady())
          {
            ImGui::TextWrapped("Path Tracing ReSTIR GI mode is present in the UI, but the renderer is not ready yet.");
          }
          else
          {
            nvsamples::ReSTIRGISettings& restirGiSettings = m_ReSTIRGI->GetSettings();

            invalidatePathTracingHistory |= DrawReSTIRCommonControls(restirGiSettings.common, "Resampling##ReSTIRGI_Mode");
            invalidatePathTracingHistory |= DrawReSTIRResamplingSection("Resampling##ReSTIRGI_Settings",
                                                                        restirGiSettings.temporalResampling,
                                                                        restirGiSettings.spatialResampling, 128.0f);

            if(ImGui::TreeNodeEx("Final Shading", ImGuiTreeNodeFlags_DefaultOpen))
            {
              bool enableFinalVisibility = (restirGiSettings.shading.enableFinalVisibility != 0u);
              if(ImGui::Checkbox("Enable Final Visibility", &enableFinalVisibility))
              {
                restirGiSettings.shading.enableFinalVisibility = enableFinalVisibility ? 1u : 0u;
                invalidatePathTracingHistory                   = true;
              }

              ImGui::TextDisabled("GI reuses one secondary indirect sample and optionally rechecks its visibility at the primary surface.");
              ImGui::TreePop();
            }

            if(ImGui::TreeNodeEx("Continuation"))
            {
              const uint32_t bounceLimit = m_ReSTIRGI->GetPipelineBounceLimit();
              if(DrawBounceLimitControl("Continuation Max Bounces", restirGiSettings.continuationMaxBounces, bounceLimit))
              {
                invalidatePathTracingHistory = true;
              }

              ImGui::TextDisabled("Indirect lighting comes from GI reservoirs, while the continuation path covers glossy reflections and further transport.");
              ImGui::TreePop();
            }

            DrawReSTIRMethodFooter(
                "This GI mode keeps direct lighting in the primary base radiance, reuses one indirect sample through temporal and spatial resampling, and adds a path-traced continuation for glossy reflections and further transport.",
                m_ReSTIRGI->GetAccumulatedFrameCount());
          }
        }
        else if(m_RenderMode == RenderMode::ePathTracingReSTIRPT)
        {
          if(m_ReSTIRPT == nullptr || !m_ReSTIRPT->IsReady())
          {
            ImGui::TextWrapped("Path Tracing ReSTIR PT mode is present in the UI, but the renderer is not ready yet.");
          }
          else
          {
            nvsamples::ReSTIRPTSettings& restirSettings = m_ReSTIRPT->GetSettings();
            const uint32_t               bounceLimit    = m_ReSTIRPT->GetPipelineBounceLimit();

            invalidatePathTracingHistory |= DrawReSTIRCommonControls(restirSettings.common, "Resampling");
            invalidatePathTracingHistory |= DrawReSTIRResamplingSection("Resampling##ReSTIRPT_Settings",
                                                                        restirSettings.common.temporalResampling,
                                                                        restirSettings.common.spatialResampling, 100.0f);

            if(ImGui::TreeNodeEx("Baseline", ImGuiTreeNodeFlags_DefaultOpen))
            {
              int initialSampleCount = static_cast<int>(restirSettings.common.initialSampling.numInitialSamples);
              if(ImGui::SliderInt("Initial Sample Count", &initialSampleCount, 1, 16))
              {
                restirSettings.common.initialSampling.numInitialSamples = static_cast<uint32_t>(initialSampleCount);
                invalidatePathTracingHistory                            = true;
              }

              if(DrawBounceLimitControl("Max Bounce Depth", restirSettings.common.initialSampling.maxBounceDepth, bounceLimit))
              {
                invalidatePathTracingHistory                         = true;
              }

              invalidatePathTracingHistory |=
                  ImGui::SliderFloat("Roughness Threshold", &restirSettings.reconnection.roughnessThreshold, 0.0f, 1.0f, "%.2f");
              invalidatePathTracingHistory |=
                  ImGui::SliderFloat("Distance Threshold", &restirSettings.reconnection.distanceThreshold, 0.0f, 20.0f, "%.2f");

              DrawTransmissionBounceHint(restirSettings.common.initialSampling.maxBounceDepth);

              ImGui::TreePop();
            }

            if(ImGui::TreeNodeEx("Advanced"))
            {
              bool enableVisibilityValidation = restirSettings.common.enableVisibilityValidation;
              if(ImGui::Checkbox("Enable Visibility Validation", &enableVisibilityValidation))
              {
                restirSettings.common.enableVisibilityValidation = enableVisibilityValidation;
                invalidatePathTracingHistory                     = true;
              }

              int maxReservoirAge = static_cast<int>(restirSettings.common.temporalResampling.maxReservoirAge);
              if(ImGui::SliderInt("Max Reservoir Age", &maxReservoirAge, 1, 64))
              {
                restirSettings.common.temporalResampling.maxReservoirAge = static_cast<uint32_t>(maxReservoirAge);
                invalidatePathTracingHistory                             = true;
              }

              bool enablePermutationSampling = restirSettings.common.temporalResampling.enablePermutationSampling != 0;
              if(ImGui::Checkbox("Enable Temporal Permutation Sampling", &enablePermutationSampling))
              {
                restirSettings.common.temporalResampling.enablePermutationSampling = enablePermutationSampling ? 1u : 0u;
                invalidatePathTracingHistory                                       = true;
              }

              bool enableFallbackSampling = restirSettings.common.temporalResampling.enableFallbackSampling != 0;
              if(ImGui::Checkbox("Enable Temporal Fallback Sample", &enableFallbackSampling))
              {
                restirSettings.common.temporalResampling.enableFallbackSampling = enableFallbackSampling ? 1u : 0u;
                invalidatePathTracingHistory                                    = true;
              }

              ImGui::TextWrapped("The thesis baseline uses fixed-threshold reconnection plus optional ray-query visibility validation.");
              ImGui::TreePop();
            }

            if(ImGui::TreeNodeEx("Debug"))
            {
              int debugView = static_cast<int>(restirSettings.common.debugView);
              const char* debugViews[] = {"Disabled", "Candidate Kind", "Target PDF", "Reservoir Weight", "Reservoir Age",
                                          "Temporal Status", "Spatial Status", "Shift Jacobian", "Reuse Count"};
              if(ImGui::Combo("Debug View", &debugView, debugViews, IM_ARRAYSIZE(debugViews)))
              {
                restirSettings.common.debugView = static_cast<shaderio::ReSTIRDebugView>(debugView);
              }

              ImGui::TreePop();
            }

            DrawReSTIRMethodFooter(
                "The baseline path tracer remains the ground truth. This mode keeps a separate ReSTIR PT implementation so we can compare reuse behavior and quality directly.",
                m_ReSTIRPT->GetAccumulatedFrameCount());
          }
        }
        else
        {
          ImGui::TextWrapped("Unknown renderer mode.");
        }
      }
      if(ImGui::CollapsingHeader("Camera"))
      {
        nvgui::CameraWidget(m_CameraManip);
      }

      if(ImGui::CollapsingHeader("Assets"))
      {
        if(!m_SceneDefinitions.empty())
        {
          const char* activeSceneLabel =
              m_SceneDefinitions[m_SelectedSceneIndex].label.empty() ? "<unnamed scene>" : m_SceneDefinitions[m_SelectedSceneIndex].label.c_str();
          if(ImGui::BeginCombo("Scene", activeSceneLabel))
          {
            for(size_t i = 0; i < m_SceneDefinitions.size(); ++i)
            {
              ImGui::PushID(static_cast<int>(i));
              const bool selected = (m_SelectedSceneIndex == i);
              const char* sceneLabel = m_SceneDefinitions[i].label.empty() ? "<unnamed scene>" : m_SceneDefinitions[i].label.c_str();
              if(ImGui::Selectable(sceneLabel, selected))
              {
                m_SelectedSceneIndex   = i;
                m_SceneReloadRequested = true;
              }
              if(selected)
              {
                ImGui::SetItemDefaultFocus();
              }
              ImGui::PopID();
            }
            ImGui::EndCombo();
          }
        }
        else
        {
          ImGui::TextUnformatted("No scenes available");
        }

        if(!m_HdriAssets.empty())
        {
          const char* activeHdriLabel =
              m_HdriAssets[m_SelectedHdriIndex].label.empty() ? "<unnamed HDRI>" : m_HdriAssets[m_SelectedHdriIndex].label.c_str();
          if(ImGui::BeginCombo("HDRI", activeHdriLabel))
          {
            for(size_t i = 0; i < m_HdriAssets.size(); ++i)
            {
              ImGui::PushID(static_cast<int>(i));
              const bool selected = (m_SelectedHdriIndex == i);
              const char* hdriLabel = m_HdriAssets[i].label.empty() ? "<unnamed HDRI>" : m_HdriAssets[i].label.c_str();
              if(ImGui::Selectable(hdriLabel, selected))
              {
                m_SelectedHdriIndex    = i;
                m_HdriReloadRequested  = true;
              }
              if(selected)
              {
                ImGui::SetItemDefaultFocus();
              }
              ImGui::PopID();
            }
            ImGui::EndCombo();
          }
        }
      }

      if(ImGui::CollapsingHeader("Environment"))
      {
        bool useHdri = (sceneInfo.useHdrEnv != 0);
        if(ImGui::Checkbox("Use HDRI", &useHdri))
        {
          sceneInfo.useHdrEnv          = useHdri ? 1 : 0;
          invalidatePathTracingHistory = true;
        }

        bool useSky = (sceneInfo.useSky != 0);
        if(ImGui::Checkbox("Use Sky", &useSky))
        {
          sceneInfo.useSky             = useSky ? 1 : 0;
          invalidatePathTracingHistory = true;
        }

        if(sceneInfo.useHdrEnv != 0)
        {
          if(m_HdriAssets.empty())
          {
            ImGui::TextUnformatted("No HDRIs found in Content/HDRI");
          }
          else
          {
            const char* activeHdriLabel =
                m_HdriAssets[m_SelectedHdriIndex].label.empty() ? "<unnamed HDRI>" : m_HdriAssets[m_SelectedHdriIndex].label.c_str();
            ImGui::Text("Active HDRI: %s", activeHdriLabel);
          }
        }
        else if(sceneInfo.useSky != 0)
        {
          invalidatePathTracingHistory |= nvgui::skySimpleParametersUI(sceneInfo.skySimpleParam);
        }
        else
        {
          PE::begin();
          invalidatePathTracingHistory |= PE::ColorEdit3("Background", (float*)&sceneInfo.backgroundColor);
          PE::end();

          // Light.
          PE::begin();
          if(sceneInfo.punctualLights[0].type == shaderio::GltfLightType::ePoint
             || sceneInfo.punctualLights[0].type == shaderio::GltfLightType::eSpot)
          {
            PE::DragFloat3("Light Position", glm::value_ptr(sceneInfo.punctualLights[0].position), 1.0f, -20.0f,
                           20.0f, "%.2f", ImGuiSliderFlags_None, "Position of the light");
          }
          if(sceneInfo.punctualLights[0].type == shaderio::GltfLightType::eDirectional
             || sceneInfo.punctualLights[0].type == shaderio::GltfLightType::eSpot)
          {
            PE::SliderFloat3("Light Direction", glm::value_ptr(sceneInfo.punctualLights[0].direction), -1.0f, 1.0f,
                             "%.2f", ImGuiSliderFlags_None, "Direction of the light");
          }

          PE::SliderFloat("Light Intensity", &sceneInfo.punctualLights[0].intensity, 0.0f, 1000.0f, "%.2f",
                          ImGuiSliderFlags_Logarithmic, "Intensity of the light");
          PE::ColorEdit3("Light Color", glm::value_ptr(sceneInfo.punctualLights[0].color), ImGuiColorEditFlags_NoInputs,
                         "Color of the light");
          PE::Combo("Light Type", (int*)&sceneInfo.punctualLights[0].type, "Point\0Spot\0Directional\0", 3,
                    "Type of the light (Point, Spot, Directional)");
          if(sceneInfo.punctualLights[0].type == shaderio::GltfLightType::eSpot)
          {
            PE::SliderAngle("Cone Angle", &sceneInfo.punctualLights[0].coneAngle, 0.f, 90.f, "%.2f",
                            ImGuiSliderFlags_AlwaysClamp, "Cone angle of the spot light");
          }
          PE::end();
        }
      }

      if(ImGui::CollapsingHeader("Tonemapper"))
      {
        nvgui::tonemapperWidget(m_TonemapperData);
      }

      ImGui::Separator();
      PE::begin();
      PE::SliderFloat2("Metallic/Roughness", glm::value_ptr(m_MetallicRoughnessOverride), -0.01f, 1.0f, "%.2f",
                       ImGuiSliderFlags_AlwaysClamp, "Override all material metallic and roughness");
      PE::end();

      if(invalidatePathTracingHistory)
      {
        InvalidatePathTracingHistory();
      }
    }
    ImGui::End();
  }
void Application::onResize(VkCommandBuffer cmd, const VkExtent2D& size)
  {
    NVVK_CHECK(m_GBuffers.update(cmd, size));
    InvalidatePathTracingHistory();
  }

void Application::onRender(VkCommandBuffer cmd)
  {
    NVVK_DBG_SCOPE(cmd);

    if(m_SceneReloadRequested || m_HdriReloadRequested)
    {
      RebuildSceneFromSelection();
      m_SceneReloadRequested = false;
      m_HdriReloadRequested  = false;
    }
    if(!m_SceneRuntime->IsReady())
    {
      return;
    }

    UpdateSceneBuffer(cmd);
    if(IsPathTracerRenderMode() && m_PathTracer != nullptr && m_PathTracer->IsReady())
    {
      PathTraceScene(cmd);
    }
    else if(IsReSTIRDIRenderMode() && m_ReSTIRDI != nullptr && m_ReSTIRDI->IsReady())
    {
      ReSTIRDIScene(cmd);
    }
    else if(IsReSTIRGIRenderMode() && m_ReSTIRGI != nullptr && m_ReSTIRGI->IsReady())
    {
      ReSTIRGIScene(cmd);
    }
    else if(IsReSTIRPTRenderMode() && m_ReSTIRPT != nullptr && m_ReSTIRPT->IsReady())
    {
      ReSTIRPTScene(cmd);
    }
    else
    {
      RasterScene(cmd);
    }
    PostProcess(cmd);
  }

void Application::onUIMenu()
  {
    bool reload = false;
    if(ImGui::BeginMenu("Tools"))
    {
      reload |= ImGui::MenuItem("Reload Shaders", "F5");
      ImGui::EndMenu();
    }

    reload |= ImGui::IsKeyPressed(ImGuiKey_F5);
    if(reload)
    {
      vkQueueWaitIdle(m_App->getQueue(0).queue);
      CompileAndCreateGraphicsShaders();
    }
  }

void Application::onLastHeadlessFrame()
  {
    m_App->saveImageToFile(m_GBuffers.getColorImage(eImgTonemapped), m_GBuffers.getSize(),
                           nvutils::getExecutablePath().replace_extension(".jpg").string());
  }

std::shared_ptr<nvutils::CameraManipulator> Application::GetCameraManipulator() const
  {
    return m_CameraManip;
  }

void Application::DiscoverAssets()
  {
    const nvsamples::SceneAssetCatalogData catalogData = m_SceneAssetCatalog->Discover();
    m_ModelAssets                                 = catalogData.modelAssets;
    m_HdriAssets                                  = catalogData.hdriAssets;
    m_SceneDefinitions                            = catalogData.sceneDefinitions;
    m_SelectedSceneIndex                          = catalogData.selectedSceneIndex;
    m_SelectedHdriIndex                           = catalogData.selectedHdriIndex;

    // Emit one clear startup warning per missing asset reference.
    for(const std::string& warning : catalogData.warnings)
    {
      LOGW("%s\n", warning.c_str());
    }
  }

void Application::RebuildSceneFromSelection()
  {
    CreateScene(false);
    UpdateTextures();
    InvalidatePathTracingHistory();
  }

void Application::PostProcess(VkCommandBuffer cmd)
  {
    NVVK_DBG_SCOPE(cmd);

    m_Tonemapper.runCompute(cmd, m_GBuffers.getSize(), m_TonemapperData, m_GBuffers.getDescriptorImageInfo(eImgRendered),
                            m_GBuffers.getDescriptorImageInfo(eImgTonemapped));

    // Barrier: make sure the tonemapped image is ready for display.
    nvvk::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);
  }

void Application::CreateScene(bool resetCamera)
  {
    SCOPED_TIMER(__FUNCTION__);

    const nvsamples::SceneResolver::Input resolveInput{
        .sceneDefinitions   = m_SceneDefinitions,
        .selectedSceneIndex = m_SelectedSceneIndex,
        .hdriAssets         = m_HdriAssets,
        .selectedHdriIndex  = m_SelectedHdriIndex,
    };
    const nvsamples::SceneResolver::Output resolved = m_SceneResolver->Resolve(resolveInput);
    if(resolved.sceneDefinition == nullptr)
    {
      LOGE("No scenes available\n");
      return;
    }
    m_SelectedSceneIndex = resolved.resolvedSceneIndex;
    m_SelectedHdriIndex  = resolved.resolvedHdriIndex;

    m_SceneRuntime->RebuildScene(
        m_App->getQueue(0).queue,
        nvsamples::SceneUploader::UploadInput{.sceneDefinition = *resolved.sceneDefinition, .selectedHdriRelativePath = resolved.hdriRelativePath},
        resetCamera, m_CameraManip.get());

    InvalidatePathTracingHistory();
  }
void Application::CreateGraphicsDescriptorSetLayout()
  {
    nvvk::DescriptorBindings bindings;
    bindings.addBinding({.binding         = shaderio::BindingPoints::eTextures,
                         .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         .descriptorCount = kMaxTextureDescriptors,
                         .stageFlags      = VK_SHADER_STAGE_ALL},
                        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
                            | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);

    m_DescPack.init(bindings, m_App->getDevice(), 1, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                    VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);

    NVVK_DBG_NAME(m_DescPack.getLayout());
    NVVK_DBG_NAME(m_DescPack.getPool());
    NVVK_DBG_NAME(m_DescPack.getSet(0));
  }

void Application::CreateGraphicsPipelineLayout()
  {
    const VkPushConstantRange pushConstantRange = {
        .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
        .offset     = 0,
        .size       = sizeof(shaderio::TutoPushConstant),
    };

    const VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = m_DescPack.getLayoutPtr(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pushConstantRange,
    };

    NVVK_CHECK(vkCreatePipelineLayout(m_App->getDevice(), &pipelineLayoutInfo, nullptr, &m_GraphicPipelineLayout));
    NVVK_DBG_NAME(m_GraphicPipelineLayout);
  }

void Application::UpdateTextures()
  {
    m_SceneRuntime->UpdateTextureDescriptors(m_App->getDevice(), m_DescPack, kMaxTextureDescriptors);
    if(m_PathTracer != nullptr && m_PathTracer->IsReady())
    {
      m_SceneRuntime->UpdateTextureDescriptors(m_App->getDevice(), m_PathTracer->GetDescriptorPack(), kMaxTextureDescriptors);
    }
    if(m_ReSTIRDI != nullptr && m_ReSTIRDI->IsReady())
    {
      m_SceneRuntime->UpdateTextureDescriptors(m_App->getDevice(), m_ReSTIRDI->GetDescriptorPack(), kMaxTextureDescriptors);
    }
    if(m_ReSTIRGI != nullptr && m_ReSTIRGI->IsReady())
    {
      m_SceneRuntime->UpdateTextureDescriptors(m_App->getDevice(), m_ReSTIRGI->GetDescriptorPack(), kMaxTextureDescriptors);
    }
    if(m_ReSTIRPT != nullptr && m_ReSTIRPT->IsReady())
    {
      m_SceneRuntime->UpdateTextureDescriptors(m_App->getDevice(), m_ReSTIRPT->GetDescriptorPack(), kMaxTextureDescriptors);
    }
  }

VkShaderModuleCreateInfo Application::CompileSlangShader(const std::filesystem::path& filename, const std::span<const uint32_t>& spirvFallback)
  {
    SCOPED_TIMER(__FUNCTION__);

    VkShaderModuleCreateInfo shaderCode = nvsamples::GetShaderModuleCreateInfo(spirvFallback);

    const std::filesystem::path shaderSource = nvutils::findFile(filename, nvsamples::GetShaderDirs());
    if(m_SlangCompiler.compileFile(shaderSource))
    {
      shaderCode.codeSize = m_SlangCompiler.getSpirvSize();
      shaderCode.pCode    = m_SlangCompiler.getSpirv();
    }
    else
    {
      LOGE("Error compiling shaders: %s\n%s\n", shaderSource.string().c_str(), m_SlangCompiler.getLastDiagnosticMessage().c_str());
    }

    return shaderCode;
  }

void Application::CompileAndCreateGraphicsShaders()
  {
    SCOPED_TIMER(__FUNCTION__);

    VkShaderModuleCreateInfo shaderCode = CompileSlangShader("Rasterizer.slang", Rasterizer_slang);

    vkDestroyShaderEXT(m_App->getDevice(), m_VertexShader, nullptr);
    vkDestroyShaderEXT(m_App->getDevice(), m_FragmentShader, nullptr);

    const VkPushConstantRange pushConstantRange = {
        .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
        .offset     = 0,
        .size       = sizeof(shaderio::TutoPushConstant),
    };

    VkShaderCreateInfoEXT shaderInfo = {
        .sType                  = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
        .codeType               = VK_SHADER_CODE_TYPE_SPIRV_EXT,
        .pName                  = "main",
        .setLayoutCount         = 1,
        .pSetLayouts            = m_DescPack.getLayoutPtr(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pushConstantRange,
    };

    // Vertex shader.
    shaderInfo.stage     = VK_SHADER_STAGE_VERTEX_BIT;
    shaderInfo.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderInfo.pName     = "vertexMain";
    shaderInfo.codeSize  = shaderCode.codeSize;
    shaderInfo.pCode     = shaderCode.pCode;
    vkCreateShadersEXT(m_App->getDevice(), 1U, &shaderInfo, nullptr, &m_VertexShader);
    NVVK_DBG_NAME(m_VertexShader);

    // Fragment shader.
    shaderInfo.stage     = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderInfo.nextStage = 0;
    shaderInfo.pName     = "fragmentMain";
    shaderInfo.codeSize  = shaderCode.codeSize;
    shaderInfo.pCode     = shaderCode.pCode;
    vkCreateShadersEXT(m_App->getDevice(), 1U, &shaderInfo, nullptr, &m_FragmentShader);
    NVVK_DBG_NAME(m_FragmentShader);
  }

void Application::UpdateSceneBuffer(VkCommandBuffer cmd)
  {
    const glm::mat4& viewMatrix = m_CameraManip->getViewMatrix();
    const glm::mat4& projMatrix = m_CameraManip->getPerspectiveMatrix();
    m_SceneRuntime->UpdateSceneBuffer(cmd, viewMatrix, projMatrix, m_CameraManip->getEye(), m_App->getViewportSize());
  }

void Application::RasterScene(VkCommandBuffer cmd)
  {
    m_SceneRenderer->Render(nvsamples::SceneRenderer::RenderInput{
        .cmd                       = cmd,
        .sceneResource             = &m_SceneRuntime->GetSceneResource(),
        .sceneInfo                 = &m_SceneRuntime->GetSceneInfo(),
        .metallicRoughnessOverride = &m_MetallicRoughnessOverride,
        .viewportSize              = &m_App->getViewportSize(),
        .cameraManip               = m_CameraManip,
        .skySimple                 = &m_SkySimple,
        .gBuffers                  = &m_GBuffers,
        .dynamicPipeline           = &m_DynamicPipeline,
        .descPack                  = &m_DescPack,
        .graphicsPipelineLayout    = m_GraphicPipelineLayout,
        .vertexShader              = m_VertexShader,
        .fragmentShader            = m_FragmentShader,
        .renderedImageIndex        = eImgRendered,
    });
  }

void Application::PathTraceScene(VkCommandBuffer cmd)
  {
    m_PathTracer->Render(nvsamples::PathTracer::RenderInput{
        .cmd                = cmd,
        .sceneResource      = &m_SceneRuntime->GetSceneResource(),
        .sceneInfo          = &m_SceneRuntime->GetSceneInfo(),
        .topLevelAS         = &m_SceneRuntime->GetTopLevelAccelerationStructure(),
        .gBuffers           = &m_GBuffers,
        .renderedImageIndex = eImgRendered,
    });
  }

void Application::ReSTIRPTScene(VkCommandBuffer cmd)
  {
    m_ReSTIRPT->Render(nvsamples::ReSTIRPTRenderer::RenderInput{
        .cmd                = cmd,
        .sceneResource      = &m_SceneRuntime->GetSceneResource(),
        .sceneInfo          = &m_SceneRuntime->GetSceneInfo(),
        .topLevelAS         = &m_SceneRuntime->GetTopLevelAccelerationStructure(),
        .gBuffers           = &m_GBuffers,
        .renderedImageIndex = eImgRendered,
    });
  }

void Application::ReSTIRGIScene(VkCommandBuffer cmd)
  {
    m_ReSTIRGI->Render(nvsamples::ReSTIRGIRenderer::RenderInput{
        .cmd                = cmd,
        .sceneResource      = &m_SceneRuntime->GetSceneResource(),
        .sceneInfo          = &m_SceneRuntime->GetSceneInfo(),
        .topLevelAS         = &m_SceneRuntime->GetTopLevelAccelerationStructure(),
        .gBuffers           = &m_GBuffers,
        .renderedImageIndex = eImgRendered,
    });
  }

void Application::ReSTIRDIScene(VkCommandBuffer cmd)
  {
    m_ReSTIRDI->Render(nvsamples::ReSTIRDIRenderer::RenderInput{
        .cmd                = cmd,
        .sceneResource      = &m_SceneRuntime->GetSceneResource(),
        .sceneInfo          = &m_SceneRuntime->GetSceneInfo(),
        .topLevelAS         = &m_SceneRuntime->GetTopLevelAccelerationStructure(),
        .gBuffers           = &m_GBuffers,
        .renderedImageIndex = eImgRendered,
    });
  }

void Application::InvalidatePathTracingHistory()
  {
    m_SceneRuntime->InvalidateFrameHistory();
    if(m_PathTracer != nullptr && m_PathTracer->IsReady())
    {
      m_PathTracer->InvalidateAccumulation();
    }
    if(m_ReSTIRDI != nullptr && m_ReSTIRDI->IsReady())
    {
      m_ReSTIRDI->InvalidateHistory();
    }
    if(m_ReSTIRGI != nullptr && m_ReSTIRGI->IsReady())
    {
      m_ReSTIRGI->InvalidateHistory();
    }
    if(m_ReSTIRPT != nullptr && m_ReSTIRPT->IsReady())
    {
      m_ReSTIRPT->InvalidateHistory();
    }
  }

bool Application::IsPathTracerRenderMode() const
  {
    return m_RenderMode == RenderMode::ePathTracing;
  }

bool Application::IsReSTIRDIRenderMode() const
  {
    return m_RenderMode == RenderMode::ePathTracingReSTIRDI;
  }

bool Application::IsReSTIRGIRenderMode() const
  {
    return m_RenderMode == RenderMode::ePathTracingReSTIRGI;
  }

bool Application::IsReSTIRPTRenderMode() const
  {
    return m_RenderMode == RenderMode::ePathTracingReSTIRPT;
  }
// ---------------------------------------------------------------------------------------------------------------------
// Application module API
//

std::shared_ptr<nvapp::IAppElement> CreateApplicationElement(const std::shared_ptr<nvutils::CameraManipulator>& cameraManip)
{
  return std::make_shared<Application>(cameraManip);
}

}  // namespace nvsamples













