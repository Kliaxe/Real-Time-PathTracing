#include "SceneRuntime.h"

// Role:
// Centralizes scene rebuild/destroy, descriptor updates, and scene-info buffer updates.

#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>
#include <nvapp/application.hpp>
#include <nvutils/camera_manipulator.hpp>
#include <nvutils/logger.hpp>
#include <nvvk/barriers.hpp>
#include <nvvk/descriptors.hpp>

#include "Shaders/ShaderIo.h"

namespace nvsamples
{

SceneRuntime::SceneRuntime(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
    , m_StagingUploader(createInfo.stagingUploader)
    , m_SceneUploader(createInfo.app, createInfo.allocator, createInfo.stagingUploader, createInfo.samplerPool)
{
}

void SceneRuntime::Destroy()
{
  DestroySceneResources();
  DestroyTextures();
}

void SceneRuntime::RebuildScene(VkQueue queue, const SceneUploader::UploadInput& input, bool resetCamera,
                                nvutils::CameraManipulator* cameraManip)
{
  vkQueueWaitIdle(queue);
  Destroy();

  VkCommandBuffer cmd                  = m_App->createTempCmdBuffer();
  SceneUploader::UploadState uploadState{
      .sceneResource      = m_SceneResource,
      .textures           = m_Textures,
      .materialAttributes = m_MaterialAttributes,
  };
  const int environmentTextureIndex = m_SceneUploader.Upload(cmd, input, uploadState);

  nvsamples::CreateGltfSceneInfoBuffer(m_SceneResource, *m_StagingUploader);
  m_StagingUploader->cmdUploadAppended(cmd);

  shaderio::GltfSceneInfo& sceneInfo = m_SceneResource.sceneInfo;
  sceneInfo.useSky                   = 0;
  sceneInfo.useHdrEnv                = (environmentTextureIndex >= 0) ? 1 : 0;
  sceneInfo.environmentTextureIndex  = environmentTextureIndex;
  sceneInfo.instances                = (shaderio::GltfInstance*)m_SceneResource.bInstances.address;
  sceneInfo.meshes                   = (shaderio::GltfMesh*)m_SceneResource.bMeshes.address;
  sceneInfo.materials                = (shaderio::GltfMetallicRoughness*)m_SceneResource.bMaterials.address;
  sceneInfo.backgroundColor          = {0.85f, 0.85f, 0.85f};
  sceneInfo.numLights                = 1;
  sceneInfo.viewportSize             = glm::vec2(static_cast<float>(m_App->getViewportSize().width),
                                                 static_cast<float>(m_App->getViewportSize().height));
  sceneInfo.punctualLights[0].color     = glm::vec3(1.0f);
  sceneInfo.punctualLights[0].intensity = 4.0f;
  sceneInfo.punctualLights[0].position  = glm::vec3(1.0f, 1.0f, 1.0f);
  sceneInfo.punctualLights[0].direction = glm::vec3(1.0f, 1.0f, 1.0f);
  sceneInfo.punctualLights[0].type      = shaderio::GltfLightType::ePoint;
  sceneInfo.punctualLights[0].coneAngle = 0.9f;

  m_App->submitAndWaitTempCmdBuffer(cmd);

  if(resetCamera && cameraManip != nullptr)
  {
    cameraManip->setClipPlanes({0.01F, 100.0F});
    cameraManip->setLookat({0.0F, 0.5F, 5.0}, {0.F, 0.F, 0.F}, {0.0F, 1.0F, 0.0F});
  }
}

bool SceneRuntime::IsReady() const
{
  return m_SceneResource.bSceneInfo.buffer != VK_NULL_HANDLE;
}

void SceneRuntime::UpdateTextureDescriptors(VkDevice device, nvvk::DescriptorPack& descPack, uint32_t maxTextureDescriptors) const
{
  if(m_Textures.empty())
  {
    return;
  }

  const uint32_t textureCount = std::min(static_cast<uint32_t>(m_Textures.size()), maxTextureDescriptors);
  if(textureCount == 0)
  {
    return;
  }
  if(textureCount < m_Textures.size())
  {
    LOGW("Texture count (%zu) exceeds descriptor capacity (%u). Extra textures will be ignored.\n", m_Textures.size(),
         maxTextureDescriptors);
  }

  nvvk::WriteSetContainer write;
  VkWriteDescriptorSet    allTextures = descPack.makeWrite(shaderio::BindingPoints::eTextures, 0, 0, textureCount);
  nvvk::Image*            allImages   = const_cast<nvvk::Image*>(m_Textures.data());
  write.append(allTextures, allImages);
  vkUpdateDescriptorSets(device, write.size(), write.data(), 0, nullptr);
}

void SceneRuntime::UpdateSceneBuffer(VkCommandBuffer cmd, const glm::mat4& viewMatrix, const glm::mat4& projMatrix,
                                     const glm::vec3& cameraPosition, const VkExtent2D& viewportSize)
{
  m_SceneResource.sceneInfo.viewProjMatrix = projMatrix * viewMatrix;
  m_SceneResource.sceneInfo.projInvMatrix  = glm::inverse(m_SceneResource.sceneInfo.viewProjMatrix);
  m_SceneResource.sceneInfo.viewInvMatrix  = glm::inverse(viewMatrix);
  m_SceneResource.sceneInfo.cameraPosition = cameraPosition;
  m_SceneResource.sceneInfo.viewportSize   = glm::vec2(static_cast<float>(viewportSize.width), static_cast<float>(viewportSize.height));
  m_SceneResource.sceneInfo.instances      = (shaderio::GltfInstance*)m_SceneResource.bInstances.address;
  m_SceneResource.sceneInfo.meshes         = (shaderio::GltfMesh*)m_SceneResource.bMeshes.address;
  m_SceneResource.sceneInfo.materials      = (shaderio::GltfMetallicRoughness*)m_SceneResource.bMaterials.address;

  nvvk::cmdBufferMemoryBarrier(
      cmd, {m_SceneResource.bSceneInfo.buffer, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT});
  vkCmdUpdateBuffer(cmd, m_SceneResource.bSceneInfo.buffer, 0, sizeof(shaderio::GltfSceneInfo), &m_SceneResource.sceneInfo);
  nvvk::cmdBufferMemoryBarrier(
      cmd, {m_SceneResource.bSceneInfo.buffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT});
}

nvsamples::GltfSceneResource& SceneRuntime::GetSceneResource()
{
  return m_SceneResource;
}

const nvsamples::GltfSceneResource& SceneRuntime::GetSceneResource() const
{
  return m_SceneResource;
}

shaderio::GltfSceneInfo& SceneRuntime::GetSceneInfo()
{
  return m_SceneResource.sceneInfo;
}

const shaderio::GltfSceneInfo& SceneRuntime::GetSceneInfo() const
{
  return m_SceneResource.sceneInfo;
}

void SceneRuntime::DestroySceneResources()
{
  m_Allocator->destroyBuffer(m_SceneResource.bSceneInfo);
  m_Allocator->destroyBuffer(m_SceneResource.bMeshes);
  m_Allocator->destroyBuffer(m_SceneResource.bMaterials);
  m_Allocator->destroyBuffer(m_SceneResource.bInstances);
  for(auto& gltfData : m_SceneResource.bGltfDatas)
  {
    m_Allocator->destroyBuffer(gltfData);
  }
  m_SceneResource = {};
  m_MaterialAttributes.clear();
}

void SceneRuntime::DestroyTextures()
{
  for(auto& texture : m_Textures)
  {
    m_Allocator->destroyImage(texture);
  }
  m_Textures.clear();
}

}  // namespace nvsamples
