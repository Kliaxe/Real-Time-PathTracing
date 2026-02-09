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

// RealTimePathTracing - Foundation
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

#include "Shaders/ShaderIo.h"

// Pre-compiled shaders (generated at build time).
#include "_autogen/sky_simple.slang.h"   // From nvpro_core2
#include "_autogen/tonemapper.slang.h"   // From nvpro_core2
#include "_autogen/Foundation.slang.h"   // From Source/Shaders/Foundation.slang

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
#include "Scene/SceneBuilder.h"

// ---------------------------------------------------------------------------------------------------------------------
// Foundation app element
//

class RtFoundation : public nvapp::IAppElement
{
  enum
  {
    eImgRendered,
    eImgTonemapped,
  };

public:
  RtFoundation()           = default;
  ~RtFoundation() override = default;

  void onAttach(nvapp::Application* app) override
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
    DiscoverAssets();
    m_SceneBuilder = std::make_unique<nvsamples::SceneBuilder>(m_App, &m_Allocator, &m_StagingUploader, &m_SamplerPool);
    CreateScene(true);
    CreateGraphicsDescriptorSetLayout();
    CreateGraphicsPipelineLayout();
    CompileAndCreateGraphicsShaders();
    UpdateTextures();

    // Init sky + tonemapper from precompiled shaders.
    m_SkySimple.init(&m_Allocator, std::span(sky_simple_slang));
    m_Tonemapper.init(&m_Allocator, std::span(tonemapper_slang));
  }

  void onDetach() override
  {
    NVVK_CHECK(vkQueueWaitIdle(m_App->getQueue(0).queue));

    VkDevice device = m_App->getDevice();

    m_DescPack.deinit();
    vkDestroyPipelineLayout(device, m_GraphicPipelineLayout, nullptr);
    vkDestroyShaderEXT(device, m_VertexShader, nullptr);
    vkDestroyShaderEXT(device, m_FragmentShader, nullptr);

    DestroySceneResources();
    DestroyTextures();

    m_GBuffers.deinit();
    m_StagingUploader.deinit();
    m_SkySimple.deinit();
    m_Tonemapper.deinit();
    m_SamplerPool.deinit();
    m_Allocator.deinit();
  }

  void onUIRender() override
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
                m_SelectedHdriIndex = i;
                m_HdriReloadRequested = true;
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
        bool useHdri = (m_SceneResource.sceneInfo.useHdrEnv != 0);
        if(ImGui::Checkbox("Use HDRI", &useHdri))
        {
          m_SceneResource.sceneInfo.useHdrEnv = useHdri ? 1 : 0;
        }

        bool useSky = (m_SceneResource.sceneInfo.useSky != 0);
        if(ImGui::Checkbox("Use Sky", &useSky))
        {
          m_SceneResource.sceneInfo.useSky = useSky ? 1 : 0;
        }

        if(m_SceneResource.sceneInfo.useHdrEnv != 0)
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
        else if(m_SceneResource.sceneInfo.useSky != 0)
        {
          nvgui::skySimpleParametersUI(m_SceneResource.sceneInfo.skySimpleParam);
        }
        else
        {
          PE::begin();
          PE::ColorEdit3("Background", (float*)&m_SceneResource.sceneInfo.backgroundColor);
          PE::end();

          // Light.
          PE::begin();
          if(m_SceneResource.sceneInfo.punctualLights[0].type == shaderio::GltfLightType::ePoint
             || m_SceneResource.sceneInfo.punctualLights[0].type == shaderio::GltfLightType::eSpot)
          {
            PE::DragFloat3("Light Position", glm::value_ptr(m_SceneResource.sceneInfo.punctualLights[0].position), 1.0f, -20.0f,
                           20.0f, "%.2f", ImGuiSliderFlags_None, "Position of the light");
          }
          if(m_SceneResource.sceneInfo.punctualLights[0].type == shaderio::GltfLightType::eDirectional
             || m_SceneResource.sceneInfo.punctualLights[0].type == shaderio::GltfLightType::eSpot)
          {
            PE::SliderFloat3("Light Direction", glm::value_ptr(m_SceneResource.sceneInfo.punctualLights[0].direction), -1.0f, 1.0f,
                             "%.2f", ImGuiSliderFlags_None, "Direction of the light");
          }

          PE::SliderFloat("Light Intensity", &m_SceneResource.sceneInfo.punctualLights[0].intensity, 0.0f, 1000.0f, "%.2f",
                          ImGuiSliderFlags_Logarithmic, "Intensity of the light");
          PE::ColorEdit3("Light Color", glm::value_ptr(m_SceneResource.sceneInfo.punctualLights[0].color), ImGuiColorEditFlags_NoInputs,
                         "Color of the light");
          PE::Combo("Light Type", (int*)&m_SceneResource.sceneInfo.punctualLights[0].type, "Point\0Spot\0Directional\0", 3,
                    "Type of the light (Point, Spot, Directional)");
          if(m_SceneResource.sceneInfo.punctualLights[0].type == shaderio::GltfLightType::eSpot)
          {
            PE::SliderAngle("Cone Angle", &m_SceneResource.sceneInfo.punctualLights[0].coneAngle, 0.f, 90.f, "%.2f",
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
      PE::SliderFloat2("Metallic/Roughness Override", glm::value_ptr(m_MetallicRoughnessOverride), -0.01f, 1.0f, "%.2f",
                       ImGuiSliderFlags_AlwaysClamp, "Override all material metallic and roughness");
      PE::end();
    }
    ImGui::End();
  }

  void onResize(VkCommandBuffer cmd, const VkExtent2D& size)
  {
    NVVK_CHECK(m_GBuffers.update(cmd, size));
  }

  void onRender(VkCommandBuffer cmd) override
  {
    NVVK_DBG_SCOPE(cmd);

    if(m_SceneReloadRequested || m_HdriReloadRequested)
    {
      RebuildSceneFromSelection();
      m_SceneReloadRequested = false;
      m_HdriReloadRequested  = false;
    }
    if(m_SceneResource.bSceneInfo.buffer == VK_NULL_HANDLE)
    {
      return;
    }

    UpdateSceneBuffer(cmd);
    RasterScene(cmd);
    PostProcess(cmd);
  }

  void onUIMenu() override
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

  void onLastHeadlessFrame() override
  {
    m_App->saveImageToFile(m_GBuffers.getColorImage(eImgTonemapped), m_GBuffers.getSize(),
                           nvutils::getExecutablePath().replace_extension(".jpg").string());
  }

  std::shared_ptr<nvutils::CameraManipulator> GetCameraManipulator() const
  {
    return m_CameraManip;
  }

private:
  void DiscoverAssets()
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

  void DestroySceneResources()
  {
    m_Allocator.destroyBuffer(m_SceneResource.bSceneInfo);
    m_Allocator.destroyBuffer(m_SceneResource.bMeshes);
    m_Allocator.destroyBuffer(m_SceneResource.bMaterials);
    m_Allocator.destroyBuffer(m_SceneResource.bInstances);
    for(auto& gltfData : m_SceneResource.bGltfDatas)
    {
      m_Allocator.destroyBuffer(gltfData);
    }
    m_SceneResource = {};
    m_MaterialAttributes.clear();
  }

  void DestroyTextures()
  {
    for(auto& texture : m_Textures)
    {
      m_Allocator.destroyImage(texture);
    }
    m_Textures.clear();
  }

  void RebuildSceneFromSelection()
  {
    vkQueueWaitIdle(m_App->getQueue(0).queue);
    DestroySceneResources();
    DestroyTextures();
    CreateScene(false);
    UpdateTextures();
  }

  void PostProcess(VkCommandBuffer cmd)
  {
    NVVK_DBG_SCOPE(cmd);

    m_Tonemapper.runCompute(cmd, m_GBuffers.getSize(), m_TonemapperData, m_GBuffers.getDescriptorImageInfo(eImgRendered),
                            m_GBuffers.getDescriptorImageInfo(eImgTonemapped));

    // Barrier: make sure the tonemapped image is ready for display.
    nvvk::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);
  }

  void CreateScene(bool resetCamera)
  {
    SCOPED_TIMER(__FUNCTION__);

    if(m_SceneDefinitions.empty())
    {
      LOGE("No scenes available\n");
      return;
    }
    if(m_SelectedSceneIndex >= m_SceneDefinitions.size())
    {
      m_SelectedSceneIndex = 0;
    }

    VkCommandBuffer cmd = m_App->createTempCmdBuffer();
    const nvsamples::SceneDefinition& selectedScene = m_SceneDefinitions[m_SelectedSceneIndex];
    std::optional<std::filesystem::path> selectedHdriRelativePath;
    if(!m_HdriAssets.empty())
    {
      if(m_SelectedHdriIndex >= m_HdriAssets.size())
      {
        m_SelectedHdriIndex = 0;
      }
      selectedHdriRelativePath = m_HdriAssets[m_SelectedHdriIndex].relativePath;
    }

    nvsamples::SceneBuilder::BuildInput input{
        .sceneDefinition          = selectedScene,
        .selectedHdriRelativePath = selectedHdriRelativePath,
    };
    nvsamples::SceneBuilder::BuildState state{
        .sceneResource      = m_SceneResource,
        .textures           = m_Textures,
        .materialAttributes = m_MaterialAttributes,
    };
    const int environmentTextureIndex = m_SceneBuilder->BuildScene(cmd, input, state);

    nvsamples::CreateGltfSceneInfoBuffer(m_SceneResource, m_StagingUploader);
    m_StagingUploader.cmdUploadAppended(cmd);

    // Scene info (GPU addresses).
    shaderio::GltfSceneInfo& sceneInfo = m_SceneResource.sceneInfo;
    sceneInfo.useSky                   = 0;
    sceneInfo.useHdrEnv                = (environmentTextureIndex >= 0) ? 1 : 0;
    sceneInfo.environmentTextureIndex  = environmentTextureIndex;
    sceneInfo.instances                = (shaderio::GltfInstance*)m_SceneResource.bInstances.address;
    sceneInfo.meshes                   = (shaderio::GltfMesh*)m_SceneResource.bMeshes.address;
    sceneInfo.materials                = (shaderio::GltfMetallicRoughness*)m_SceneResource.bMaterials.address;

    // Environment defaults.
    sceneInfo.backgroundColor             = {0.85f, 0.85f, 0.85f};
    sceneInfo.numLights                   = 1;
    sceneInfo.viewportSize                = glm::vec2(static_cast<float>(m_App->getViewportSize().width),
                                                      static_cast<float>(m_App->getViewportSize().height));
    sceneInfo.punctualLights[0].color     = glm::vec3(1.0f);
    sceneInfo.punctualLights[0].intensity = 4.0f;
    sceneInfo.punctualLights[0].position  = glm::vec3(1.0f, 1.0f, 1.0f);
    sceneInfo.punctualLights[0].direction = glm::vec3(1.0f, 1.0f, 1.0f);
    sceneInfo.punctualLights[0].type      = shaderio::GltfLightType::ePoint;
    sceneInfo.punctualLights[0].coneAngle = 0.9f;

    m_App->submitAndWaitTempCmdBuffer(cmd);

    if(resetCamera)
    {
      m_CameraManip->setClipPlanes({0.01F, 100.0F});
      m_CameraManip->setLookat({0.0F, 0.5F, 5.0}, {0.F, 0.F, 0.F}, {0.0F, 1.0F, 0.0F});
    }
  }

  void CreateGraphicsDescriptorSetLayout()
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

  void CreateGraphicsPipelineLayout()
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

  void UpdateTextures()
  {
    if(m_Textures.empty())
    {
      return;
    }

    const uint32_t textureCount = std::min(static_cast<uint32_t>(m_Textures.size()), kMaxTextureDescriptors);
    if(textureCount == 0)
    {
      return;
    }
    if(textureCount < m_Textures.size())
    {
      LOGW("Texture count (%zu) exceeds descriptor capacity (%u). Extra textures will be ignored.\n", m_Textures.size(),
           kMaxTextureDescriptors);
    }

    nvvk::WriteSetContainer write;
    VkWriteDescriptorSet    allTextures = m_DescPack.makeWrite(shaderio::BindingPoints::eTextures, 0, 0, textureCount);
    nvvk::Image*            allImages   = m_Textures.data();
    write.append(allTextures, allImages);
    vkUpdateDescriptorSets(m_App->getDevice(), write.size(), write.data(), 0, nullptr);
  }

  VkShaderModuleCreateInfo CompileSlangShader(const std::filesystem::path& filename, const std::span<const uint32_t>& spirvFallback)
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

  void CompileAndCreateGraphicsShaders()
  {
    SCOPED_TIMER(__FUNCTION__);

    VkShaderModuleCreateInfo shaderCode = CompileSlangShader("Foundation.slang", Foundation_slang);

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

  void UpdateSceneBuffer(VkCommandBuffer cmd)
  {
    NVVK_DBG_SCOPE(cmd);

    const glm::mat4& viewMatrix = m_CameraManip->getViewMatrix();
    const glm::mat4& projMatrix = m_CameraManip->getPerspectiveMatrix();

    m_SceneResource.sceneInfo.viewProjMatrix  = projMatrix * viewMatrix;
    m_SceneResource.sceneInfo.projInvMatrix   = glm::inverse(m_SceneResource.sceneInfo.viewProjMatrix);
    m_SceneResource.sceneInfo.viewInvMatrix   = glm::inverse(viewMatrix);
    m_SceneResource.sceneInfo.cameraPosition = m_CameraManip->getEye();
    m_SceneResource.sceneInfo.viewportSize   = glm::vec2(static_cast<float>(m_App->getViewportSize().width),
                                                         static_cast<float>(m_App->getViewportSize().height));
    m_SceneResource.sceneInfo.instances       = (shaderio::GltfInstance*)m_SceneResource.bInstances.address;
    m_SceneResource.sceneInfo.meshes          = (shaderio::GltfMesh*)m_SceneResource.bMeshes.address;
    m_SceneResource.sceneInfo.materials       = (shaderio::GltfMetallicRoughness*)m_SceneResource.bMaterials.address;

    nvvk::cmdBufferMemoryBarrier(cmd, {m_SceneResource.bSceneInfo.buffer, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT});
    vkCmdUpdateBuffer(cmd, m_SceneResource.bSceneInfo.buffer, 0, sizeof(shaderio::GltfSceneInfo), &m_SceneResource.sceneInfo);
    nvvk::cmdBufferMemoryBarrier(cmd, {m_SceneResource.bSceneInfo.buffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT});
  }

  void RasterScene(VkCommandBuffer cmd)
  {
    NVVK_DBG_SCOPE(cmd);

    shaderio::TutoPushConstant pushValues = {
        .sceneInfoAddress           = (shaderio::GltfSceneInfo*)m_SceneResource.bSceneInfo.address,
        .metallicRoughnessOverride  = m_MetallicRoughnessOverride,
    };
    const VkPushConstantsInfo pushInfo = {
        .sType      = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,
        .layout     = m_GraphicPipelineLayout,
        .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
        .offset     = 0,
        .size       = sizeof(shaderio::TutoPushConstant),
        .pValues    = &pushValues,
    };

    const bool useHdriBackground = (m_SceneResource.sceneInfo.useHdrEnv != 0) && (m_SceneResource.sceneInfo.environmentTextureIndex >= 0);
    const bool useProceduralSky  = (m_SceneResource.sceneInfo.useSky != 0) && !useHdriBackground;

    // Sky background (compute into HDR render target).
    if(useProceduralSky)
    {
      const glm::mat4& viewMatrix = m_CameraManip->getViewMatrix();
      const glm::mat4& projMatrix = m_CameraManip->getPerspectiveMatrix();
      m_SkySimple.runCompute(cmd, m_App->getViewportSize(), viewMatrix, projMatrix, m_SceneResource.sceneInfo.skySimpleParam,
                             m_GBuffers.getDescriptorImageInfo(eImgRendered));
    }

    VkRenderingAttachmentInfo colorAttachment = DEFAULT_VkRenderingAttachmentInfo;
    colorAttachment.loadOp = useProceduralSky ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.imageView  = m_GBuffers.getColorImageView(eImgRendered);
    colorAttachment.clearValue = {.color = {m_SceneResource.sceneInfo.backgroundColor.x, m_SceneResource.sceneInfo.backgroundColor.y,
                                            m_SceneResource.sceneInfo.backgroundColor.z, 1.0f}};

    VkRenderingAttachmentInfo depthAttachment = DEFAULT_VkRenderingAttachmentInfo;
    depthAttachment.imageView                 = m_GBuffers.getDepthImageView();
    depthAttachment.clearValue                = {.depthStencil = DEFAULT_VkClearDepthStencilValue};

    VkRenderingInfo renderingInfo      = DEFAULT_VkRenderingInfo;
    renderingInfo.renderArea           = DEFAULT_VkRect2D(m_GBuffers.getSize());
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments    = &colorAttachment;
    renderingInfo.pDepthAttachment     = &depthAttachment;

    // Transition to attachment layouts.
    nvvk::cmdImageMemoryBarrier(cmd, {m_GBuffers.getColorImage(eImgRendered), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    nvvk::cmdImageMemoryBarrier(cmd, {m_GBuffers.getDepthImage(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                      {VK_IMAGE_ASPECT_DEPTH_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}});

    // Bind descriptors (textures).
    const VkBindDescriptorSetsInfo bindDescriptorSetsInfo = {
        .sType              = VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO,
        .stageFlags         = VK_SHADER_STAGE_ALL_GRAPHICS,
        .layout             = m_GraphicPipelineLayout,
        .firstSet           = 0,
        .descriptorSetCount = 1,
        .pDescriptorSets    = m_DescPack.getSetPtr(),
    };
    vkCmdBindDescriptorSets2(cmd, &bindDescriptorSetsInfo);

    // Begin dynamic rendering.
    vkCmdBeginRendering(cmd, &renderingInfo);

    m_DynamicPipeline.rasterizationState.cullMode = VK_CULL_MODE_NONE;
    m_DynamicPipeline.cmdApplyAllStates(cmd);
    m_DynamicPipeline.cmdSetViewportAndScissor(cmd, m_App->getViewportSize());
    vkCmdSetDepthTestEnable(cmd, VK_TRUE);

    m_DynamicPipeline.cmdBindShaders(cmd, {.vertex = m_VertexShader, .fragment = m_FragmentShader});

    // No bound vertex buffers: the shader fetches from storage buffers.
    vkCmdSetVertexInputEXT(cmd, 0, nullptr, 0, nullptr);

    if(useHdriBackground)
    {
      vkCmdSetDepthTestEnable(cmd, VK_FALSE);
      vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
      pushValues.instanceIndex = -1;
      vkCmdPushConstants2(cmd, &pushInfo);
      vkCmdDraw(cmd, 3, 1, 0, 0);
      vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
      vkCmdSetDepthTestEnable(cmd, VK_TRUE);
    }

    for(size_t i = 0; i < m_SceneResource.instances.size(); ++i)
    {
      const uint32_t                  meshIndex = m_SceneResource.instances[i].meshIndex;
      const shaderio::GltfMesh&       gltfMesh  = m_SceneResource.meshes[meshIndex];
      const shaderio::TriangleMesh&   triMesh   = gltfMesh.triMesh;

      pushValues.normalMatrix  = glm::transpose(glm::inverse(glm::mat3(m_SceneResource.instances[i].transform)));
      pushValues.instanceIndex = static_cast<int>(i);
      vkCmdPushConstants2(cmd, &pushInfo);

      const uint32_t            bufferIndex = m_SceneResource.meshToBufferIndex[meshIndex];
      const nvvk::Buffer&       v           = m_SceneResource.bGltfDatas[bufferIndex];

      vkCmdBindIndexBuffer(cmd, v.buffer, triMesh.indices.offset, VkIndexType(gltfMesh.indexType));
      vkCmdDrawIndexed(cmd, triMesh.indices.count, 1, 0, 0, 0);
    }

    vkCmdEndRendering(cmd);

    // Transition back to GENERAL for compute/ImGui usage.
    nvvk::cmdImageMemoryBarrier(cmd, {m_GBuffers.getColorImage(eImgRendered), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL});
    nvvk::cmdImageMemoryBarrier(cmd, {m_GBuffers.getDepthImage(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                      {VK_IMAGE_ASPECT_DEPTH_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}});
  }

private:
  nvapp::Application* m_App = nullptr;  // Owning application
  static constexpr uint32_t kMaxTextureDescriptors = 4096;

  nvvk::ResourceAllocator m_Allocator;      // Vulkan allocator
  nvvk::StagingUploader   m_StagingUploader; // Upload helper
  nvvk::SamplerPool       m_SamplerPool;    // Sampler pool
  nvvk::GBuffer           m_GBuffers;       // Offscreen buffers
  nvslang::SlangCompiler  m_SlangCompiler;  // Hot reload compiler

  std::shared_ptr<nvutils::CameraManipulator> m_CameraManip = std::make_shared<nvutils::CameraManipulator>();

  nvvk::GraphicsPipelineState m_DynamicPipeline;  // Dynamic pipeline state
  nvvk::DescriptorPack        m_DescPack;         // Descriptor pack for textures
  VkPipelineLayout            m_GraphicPipelineLayout = VK_NULL_HANDLE;

  VkShaderEXT m_VertexShader   = VK_NULL_HANDLE;
  VkShaderEXT m_FragmentShader = VK_NULL_HANDLE;

  nvsamples::GltfSceneResource m_SceneResource;   // Scene resources
  std::vector<nvvk::Image>     m_Textures;        // Texture images
  std::vector<nvsamples::MaterialAttributes> m_MaterialAttributes;  // Canonical CPU-side material data
  std::vector<nvsamples::AssetEntry> m_ModelAssets;
  std::vector<nvsamples::AssetEntry> m_HdriAssets;
  std::vector<nvsamples::SceneDefinition> m_SceneDefinitions;
  size_t                       m_SelectedSceneIndex = 0;
  size_t                       m_SelectedHdriIndex  = 0;
  bool                         m_SceneReloadRequested = false;
  bool                         m_HdriReloadRequested  = false;

  nvshaders::SkySimple     m_SkySimple;       // Sky compute
  nvshaders::Tonemapper    m_Tonemapper;      // Tonemapper compute
  shaderio::TonemapperData m_TonemapperData;  // Tonemapper parameters

  glm::vec2 m_MetallicRoughnessOverride = {-0.01f, -0.01f};  // UI overrides
  std::unique_ptr<nvsamples::SceneAssetCatalog> m_SceneAssetCatalog;  // Asset discovery helper
  std::unique_ptr<nvsamples::SceneBuilder> m_SceneBuilder;   // Scene construction helper
};

// ---------------------------------------------------------------------------------------------------------------------
// Entry point
//

int main(int argc, char** argv)
{
  nvapp::ApplicationCreateInfo appInfo = {};

  // CLI parsing.
  nvutils::ParameterParser   cli(nvutils::getExecutablePath().stem().string());
  nvutils::ParameterRegistry reg;
  reg.add({"headless", "Run in headless mode"}, &appInfo.headless, true);
  cli.add(reg);
  cli.parse(argc, argv);

  // Vulkan context init.
  VkPhysicalDeviceShaderObjectFeaturesEXT shaderObjectFeatures = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
  };
  nvvk::ContextInitInfo vkSetup = {
      .instanceExtensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME},
      .deviceExtensions =
          {
              {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME},
              {VK_EXT_SHADER_OBJECT_EXTENSION_NAME, &shaderObjectFeatures},
          },
  };

  if(!appInfo.headless)
  {
    nvvk::addSurfaceExtensions(vkSetup.instanceExtensions, &vkSetup.deviceExtensions);
  }

  // Validation layer preset.
  nvvk::ValidationSettings validationSettings;
  validationSettings.setPreset(nvvk::ValidationSettings::LayerPresets::eStandard);
  vkSetup.instanceCreateInfoExt = validationSettings.buildPNextChain();

#if defined(USE_NSIGHT_AFTERMATH)
  auto& aftermath = AftermathCrashTracker::getInstance();
  aftermath.initialize();
  aftermath.addExtensions(vkSetup.deviceExtensions);
  nvvk::CheckError::getInstance().setCallbackFunction([&](VkResult result) { aftermath.errorCallback(result); });
#endif

  nvvk::Context vkContext;
  if(vkContext.init(vkSetup) != VK_SUCCESS)
  {
    LOGE("Error in Vulkan context creation\n");
    return 1;
  }

  // Application init.
  appInfo.name           = "RealTimePathTracing";
  appInfo.instance       = vkContext.getInstance();
  appInfo.device         = vkContext.getDevice();
  appInfo.physicalDevice = vkContext.getPhysicalDevice();
  appInfo.queues         = vkContext.getQueueInfos();

  nvapp::Application application;
  application.init(appInfo);

  // App elements.
  auto foundation   = std::make_shared<RtFoundation>();
  auto elemCamera   = std::make_shared<nvapp::ElementCamera>();
  auto windowTitle  = std::make_shared<nvapp::ElementDefaultWindowTitle>();
  auto windowMenu   = std::make_shared<nvapp::ElementDefaultMenu>();

  auto cameraManip = foundation->GetCameraManipulator();
  elemCamera->setCameraManipulator(cameraManip);

  application.addElement(windowMenu);
  application.addElement(windowTitle);
  application.addElement(elemCamera);
  application.addElement(foundation);

  application.run();

  application.deinit();
  vkContext.deinit();

  return 0;
}

