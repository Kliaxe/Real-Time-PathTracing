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
    m_SceneRuntime      = std::make_unique<nvsamples::SceneRuntime>(nvsamples::SceneRuntime::CreateInfo{
        .app             = m_App,
        .allocator       = &m_Allocator,
        .stagingUploader = &m_StagingUploader,
        .samplerPool     = &m_SamplerPool,
    });
    m_PathTracer->Initialize();
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
        const char* renderModes[] = {"Rasterizer", "Path Tracing"};
        if(ImGui::Combo("Mode", &renderMode, renderModes, IM_ARRAYSIZE(renderModes)))
        {
          m_RenderMode = static_cast<RenderMode>(renderMode);
          invalidatePathTracingHistory = true;
        }

        if(m_RenderMode == RenderMode::eRasterizer)
        {
          ImGui::TextWrapped("Rasterizer mode uses the existing graphics pipeline path.");
        }
        else if(m_PathTracer == nullptr || !m_PathTracer->IsReady())
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

          int maxBounces = static_cast<int>(pathTracingSettings.maxBounces);
          if(ImGui::SliderInt("Max Bounces", &maxBounces, 0, static_cast<int>(bounceLimit)))
          {
            pathTracingSettings.maxBounces = static_cast<uint32_t>(maxBounces);
            invalidatePathTracingHistory   = true;
          }

          ImGui::SameLine();
          if(ImGui::Button("Reset Accumulation"))
          {
            invalidatePathTracingHistory = true;
          }

          ImGui::Text("Accumulated Frames: %u", m_PathTracer->GetAccumulatedFrameCount());
          ImGui::TextWrapped(
              "Accumulation averages path traced samples across frames and automatically resets when the camera, "
              "scene, HDRI, sky, viewport, or bounce budget changes.");
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
          if(ImGui::BeginCombo("Scene", m_SceneDefinitions[m_SelectedSceneIndex].label.c_str()))
          {
            for(size_t i = 0; i < m_SceneDefinitions.size(); ++i)
            {
              const bool selected = (m_SelectedSceneIndex == i);
              if(ImGui::Selectable(m_SceneDefinitions[i].label.c_str(), selected))
              {
                m_SelectedSceneIndex   = i;
                m_SceneReloadRequested = true;
              }
              if(selected)
              {
                ImGui::SetItemDefaultFocus();
              }
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
          if(ImGui::BeginCombo("HDRI", m_HdriAssets[m_SelectedHdriIndex].label.c_str()))
          {
            for(size_t i = 0; i < m_HdriAssets.size(); ++i)
            {
              const bool selected = (m_SelectedHdriIndex == i);
              if(ImGui::Selectable(m_HdriAssets[i].label.c_str(), selected))
              {
                m_SelectedHdriIndex    = i;
                m_HdriReloadRequested  = true;
              }
              if(selected)
              {
                ImGui::SetItemDefaultFocus();
              }
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
          sceneInfo.useSky            = useSky ? 1 : 0;
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
            ImGui::Text("Active HDRI: %s", m_HdriAssets[m_SelectedHdriIndex].label.c_str());
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
    if(m_RenderMode == RenderMode::ePathTracing && m_PathTracer != nullptr && m_PathTracer->IsReady())
    {
      PathTraceScene(cmd);
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

void Application::InvalidatePathTracingHistory()
  {
    if(m_PathTracer != nullptr && m_PathTracer->IsReady())
    {
      m_PathTracer->InvalidateAccumulation();
    }
  }
// ---------------------------------------------------------------------------------------------------------------------
// Application module API
//

std::shared_ptr<nvapp::IAppElement> CreateApplicationElement(const std::shared_ptr<nvutils::CameraManipulator>& cameraManip)
{
  return std::make_shared<Application>(cameraManip);
}

}  // namespace nvsamples







