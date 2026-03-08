#include "SceneRuntime.h"

// Role:
// Centralizes scene rebuild/destroy, descriptor updates, scene-info buffer updates,
// and now the acceleration structures needed by ray tracing.

#include <algorithm>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <nvapp/application.hpp>
#include <nvutils/camera_manipulator.hpp>
#include <nvutils/logger.hpp>
#include <nvvk/acceleration_structures.hpp>
#include <nvvk/barriers.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/descriptors.hpp>

#include "Shaders/ShaderIo.h"

namespace nvsamples
{

namespace
{

// These budgets only affect how many BLAS the builder tries to batch together.
// They do not change the final BLAS contents, only the temporary build strategy.
constexpr VkDeviceSize kBlasScratchBudget = 128ull * 1024ull * 1024ull;
constexpr VkDeviceSize kBlasBuildBudget   = 512ull * 1024ull * 1024ull;
constexpr VkDeviceSize kTlasInstanceAlignment = 16;

VkDeviceSize QueryAccelerationStructureScratchAlignment(VkPhysicalDevice physicalDevice)
{
  VkPhysicalDeviceAccelerationStructurePropertiesKHR accelProps{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
  };
  VkPhysicalDeviceProperties2 props{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &accelProps,
  };
  vkGetPhysicalDeviceProperties2(physicalDevice, &props);
  return accelProps.minAccelerationStructureScratchOffsetAlignment;
}

// Convert one imported mesh into the triangle description Vulkan expects for a
// bottom-level acceleration structure (BLAS). The important contract here is
// that the imported glTF buffer already has a valid device address, so the BLAS
// build can read positions and indices directly from the same geometry buffers
// the raster path already uses.
nvvk::AccelerationStructureGeometryInfo CreateBottomLevelGeometry(const shaderio::GltfMesh& gltfMesh)
{
  nvvk::AccelerationStructureGeometryInfo result{};

  const shaderio::TriangleMesh triMesh      = gltfMesh.triMesh;
  const uint32_t              triangleCount = static_cast<uint32_t>(triMesh.indices.count / 3U);

  VkAccelerationStructureGeometryTrianglesDataKHR triangles{
      .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
      .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
      .vertexData   = {.deviceAddress = VkDeviceAddress(gltfMesh.gltfBuffer) + triMesh.positions.offset},
      .vertexStride = triMesh.positions.byteStride,
      .maxVertex    = triMesh.positions.count - 1,
      .indexType    = VkIndexType(gltfMesh.indexType),
      .indexData    = {.deviceAddress = VkDeviceAddress(gltfMesh.gltfBuffer) + triMesh.indices.offset},
  };

  result.geometry = VkAccelerationStructureGeometryKHR{
      .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
      .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
      .geometry     = {.triangles = triangles},
      .flags        = VK_GEOMETRY_OPAQUE_BIT_KHR,
  };

  result.rangeInfo = VkAccelerationStructureBuildRangeInfoKHR{.primitiveCount = triangleCount};

  return result;
}

}  // namespace

SceneRuntime::SceneRuntime(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
    , m_StagingUploader(createInfo.stagingUploader)
    , m_SceneUploader(createInfo.app, createInfo.allocator, createInfo.stagingUploader, createInfo.samplerPool)
{
}

void SceneRuntime::Destroy()
{
  // Destroy TLAS before BLAS because the top-level structure references the
  // bottom-level ones. The queue is idle before Destroy() is called, so this is
  // purely about clear ownership order.
  DestroyTopLevelAccelerationStructure();
  DestroyBottomLevelAccelerationStructures();
  DestroySceneResources();
  DestroyTextures();
}

void SceneRuntime::RebuildScene(VkQueue queue, const SceneUploader::UploadInput& input, bool resetCamera,
                                nvutils::CameraManipulator* cameraManip)
{
  vkQueueWaitIdle(queue);
  Destroy();

  // First pass: upload the scene buffers and textures.
  // This is still pure scene-data preparation. No ray tracing structures exist yet.
  VkCommandBuffer cmd = m_App->createTempCmdBuffer();

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

  // Second pass: build BLAS from uploaded mesh buffers.
  BuildBottomLevelAccelerationStructures();

  // Third pass: build the TLAS from scene instances.
  // This is where mesh-level acceleration structures become a world-space scene.
  BuildTopLevelAccelerationStructure();

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
  const uint32_t         setCount  = std::max(1u, static_cast<uint32_t>(descPack.getSets().size()));
  nvvk::Image*           allImages = const_cast<nvvk::Image*>(m_Textures.data());

  // Some descriptor packs, like the path tracer's, allocate one descriptor set
  // per frame-in-flight. Populate every set so each frame can bind its own set
  // without needing a texture re-upload right before rendering.
  for(uint32_t setIndex = 0; setIndex < setCount; ++setIndex)
  {
    VkWriteDescriptorSet allTextures = descPack.makeWrite(shaderio::BindingPoints::eTextures, setIndex, 0, textureCount);
    write.append(allTextures, allImages);
  }

  vkUpdateDescriptorSets(device, write.size(), write.data(), 0, nullptr);
}

void SceneRuntime::UpdateSceneBuffer(VkCommandBuffer cmd, const glm::mat4& viewMatrix, const glm::mat4& projMatrix,
                                     const glm::vec3& cameraPosition, const VkExtent2D& viewportSize)
{
  m_SceneResource.sceneInfo.viewProjMatrix = projMatrix * viewMatrix;

  // Historical note: the shared shader struct still calls this field
  // projInvMatrix, but both the raster sky/background path and the path tracer
  // reconstruct world-space camera rays from the inverse view-projection.
  m_SceneResource.sceneInfo.projInvMatrix  = glm::inverse(m_SceneResource.sceneInfo.viewProjMatrix);
  m_SceneResource.sceneInfo.viewInvMatrix  = glm::inverse(viewMatrix);
  m_SceneResource.sceneInfo.cameraPosition = cameraPosition;
  m_SceneResource.sceneInfo.viewportSize   = glm::vec2(static_cast<float>(viewportSize.width), static_cast<float>(viewportSize.height));
  m_SceneResource.sceneInfo.instances      = (shaderio::GltfInstance*)m_SceneResource.bInstances.address;
  m_SceneResource.sceneInfo.meshes         = (shaderio::GltfMesh*)m_SceneResource.bMeshes.address;
  m_SceneResource.sceneInfo.materials      = (shaderio::GltfMetallicRoughness*)m_SceneResource.bMaterials.address;

  const VkPipelineStageFlags2 shaderReadStages =
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;

  // Use an explicit barrier here instead of the convenience helper. The helper
  // infers acceleration-structure read access for ray tracing stages, but this
  // buffer is ordinary scene data read as shader data, not a BLAS/TLAS handle.
  const VkBufferMemoryBarrier2 beforeUpdateBarrier{
      .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask  = shaderReadStages,
      .srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
      .dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .buffer        = m_SceneResource.bSceneInfo.buffer,
      .offset        = 0,
      .size          = sizeof(shaderio::GltfSceneInfo),
  };
  const VkDependencyInfo beforeUpdateDependency{
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1,
      .pBufferMemoryBarriers    = &beforeUpdateBarrier,
  };
  vkCmdPipelineBarrier2(cmd, &beforeUpdateDependency);

  vkCmdUpdateBuffer(cmd, m_SceneResource.bSceneInfo.buffer, 0, sizeof(shaderio::GltfSceneInfo), &m_SceneResource.sceneInfo);

  const VkBufferMemoryBarrier2 afterUpdateBarrier{
      .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask  = shaderReadStages,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
      .buffer        = m_SceneResource.bSceneInfo.buffer,
      .offset        = 0,
      .size          = sizeof(shaderio::GltfSceneInfo),
  };
  const VkDependencyInfo afterUpdateDependency{
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1,
      .pBufferMemoryBarriers    = &afterUpdateBarrier,
  };
  vkCmdPipelineBarrier2(cmd, &afterUpdateDependency);
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

const std::vector<nvvk::AccelerationStructure>& SceneRuntime::GetBottomLevelAccelerationStructures() const
{
  return m_BottomLevelAS;
}

const nvvk::AccelerationStructure& SceneRuntime::GetTopLevelAccelerationStructure() const
{
  return m_TopLevelAS;
}

void SceneRuntime::BuildBottomLevelAccelerationStructures()
{
  if(m_SceneResource.meshes.empty())
  {
    return;
  }

  // Build data is the CPU-side description of "how to build each BLAS".
  // Each imported mesh becomes one BLAS build entry.
  std::vector<nvvk::AccelerationStructureBuildData> blasBuildData;
  blasBuildData.reserve(m_SceneResource.meshes.size());

  for(const shaderio::GltfMesh& mesh : m_SceneResource.meshes)
  {
    nvvk::AccelerationStructureBuildData buildData{VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR};
    buildData.addGeometry(CreateBottomLevelGeometry(mesh));
    buildData.finalizeGeometry(m_App->getDevice(), VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
    blasBuildData.push_back(buildData);
  }

  m_BottomLevelAS.resize(blasBuildData.size());

  // The builder is an execution helper: it allocates the VkAccelerationStructureKHR
  // objects and records vkCmdBuildAccelerationStructuresKHR commands for us.
  nvvk::AccelerationStructureBuilder blasBuilder;
  blasBuilder.init(m_Allocator);

  std::span<nvvk::AccelerationStructureBuildData> buildDataSpan(blasBuildData);
  std::span<nvvk::AccelerationStructure>          blasSpan(m_BottomLevelAS);

  const VkDeviceSize scratchSize = blasBuilder.getScratchSize(kBlasScratchBudget, buildDataSpan);

  // Scratch memory is temporary GPU workspace used only while the BLAS build runs.
  // It is not part of the final BLAS and can be destroyed after the build completes.
  nvvk::Buffer scratchBuffer;
  NVVK_CHECK(m_Allocator->createBuffer(scratchBuffer, scratchSize,
                                       VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT
                                           | VK_BUFFER_USAGE_2_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                                       VMA_MEMORY_USAGE_AUTO, {}, blasBuilder.getScratchAlignment()));

  VkCommandBuffer cmd = m_App->createTempCmdBuffer();

  // The geometry data was uploaded in a previous transfer submission.
  // This barrier tells Vulkan that those transfer writes must be visible before
  // acceleration structure build reads start on the same queue.
  nvvk::accelerationStructureBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT,
                                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT);

  VkResult buildResult = VK_INCOMPLETE;
  while(buildResult == VK_INCOMPLETE)
  {
    buildResult = blasBuilder.cmdCreateBlas(cmd, buildDataSpan, blasSpan, scratchBuffer.address, scratchBuffer.bufferSize,
                                            kBlasBuildBudget);
  }

  NVVK_CHECK(buildResult);
  m_App->submitAndWaitTempCmdBuffer(cmd);

  m_Allocator->destroyBuffer(scratchBuffer);
  blasBuilder.destroyNonCompactedBlas();
  blasBuilder.deinit();
}

void SceneRuntime::BuildTopLevelAccelerationStructure()
{
  if(m_SceneResource.instances.empty() || m_BottomLevelAS.empty())
  {
    return;
  }

  // TLAS instances are the scene-level link between world transforms and BLAS handles.
  // One TLAS instance says: "place this BLAS at this transform with this visibility mask".
  m_TlasInstances.clear();
  m_TlasInstances.reserve(m_SceneResource.instances.size());

  for(size_t instanceIndex = 0; instanceIndex < m_SceneResource.instances.size(); ++instanceIndex)
  {
    const shaderio::GltfInstance& sceneInstance = m_SceneResource.instances[instanceIndex];
    if(sceneInstance.meshIndex >= m_BottomLevelAS.size())
    {
      LOGW("Skipping TLAS instance %zu because mesh index %u has no BLAS.\n", instanceIndex, sceneInstance.meshIndex);
      continue;
    }

    VkAccelerationStructureInstanceKHR tlasInstance{};
    tlasInstance.transform = nvvk::toTransformMatrixKHR(sceneInstance.transform);
    tlasInstance.instanceCustomIndex = static_cast<uint32_t>(instanceIndex);
    tlasInstance.accelerationStructureReference = m_BottomLevelAS[sceneInstance.meshIndex].address;
    tlasInstance.instanceShaderBindingTableRecordOffset = 0;
    tlasInstance.mask = 0xFF;
    tlasInstance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    m_TlasInstances.push_back(tlasInstance);
  }

  if(m_TlasInstances.empty())
  {
    return;
  }

  // The TLAS build reads an array of VkAccelerationStructureInstanceKHR from a
  // regular GPU buffer. This buffer is separate from the final TLAS itself.
  NVVK_CHECK(m_Allocator->createBuffer(
      m_TlasInstancesBuffer, std::span<const VkAccelerationStructureInstanceKHR>(m_TlasInstances).size_bytes(),
      VK_BUFFER_USAGE_2_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT,
      VMA_MEMORY_USAGE_AUTO, {}, kTlasInstanceAlignment));

  nvvk::AccelerationStructureBuildData tlasBuildData{VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR};
  const nvvk::AccelerationStructureGeometryInfo instanceGeometry =
      tlasBuildData.makeInstanceGeometry(m_TlasInstances.size(), m_TlasInstancesBuffer.address);
  tlasBuildData.addGeometry(instanceGeometry);
  const VkAccelerationStructureBuildSizesInfoKHR sizeInfo =
      tlasBuildData.finalizeGeometry(m_App->getDevice(), VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);

  const VkDeviceSize scratchAlignment = QueryAccelerationStructureScratchAlignment(m_Allocator->getPhysicalDevice());

  nvvk::Buffer scratchBuffer;
  NVVK_CHECK(m_Allocator->createBuffer(scratchBuffer, sizeInfo.buildScratchSize,
                                       VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT
                                           | VK_BUFFER_USAGE_2_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                                       VMA_MEMORY_USAGE_AUTO, {}, scratchAlignment));

  NVVK_CHECK(m_StagingUploader->appendBuffer(m_TlasInstancesBuffer, 0, std::span<const VkAccelerationStructureInstanceKHR>(m_TlasInstances)));
  NVVK_CHECK(m_Allocator->createAcceleration(m_TopLevelAS, tlasBuildData.makeCreateInfo()));

  VkCommandBuffer cmd = m_App->createTempCmdBuffer();
  m_StagingUploader->cmdUploadAppended(cmd);

  // The TLAS build reads the uploaded instance buffer, so we need the same kind
  // of transfer-write -> acceleration-structure-read dependency as for BLAS.
  nvvk::accelerationStructureBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT,
                                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT);

  tlasBuildData.cmdBuildAccelerationStructure(cmd, m_TopLevelAS.accel, scratchBuffer.address);
  m_App->submitAndWaitTempCmdBuffer(cmd);

  m_Allocator->destroyBuffer(scratchBuffer);
}

void SceneRuntime::DestroyTopLevelAccelerationStructure()
{
  m_Allocator->destroyAcceleration(m_TopLevelAS);
  m_Allocator->destroyBuffer(m_TlasInstancesBuffer);
  m_TlasInstances.clear();
  m_TopLevelAS = {};
  m_TlasInstancesBuffer = {};
}

void SceneRuntime::DestroyBottomLevelAccelerationStructures()
{
  for(nvvk::AccelerationStructure& blas : m_BottomLevelAS)
  {
    m_Allocator->destroyAcceleration(blas);
  }
  m_BottomLevelAS.clear();
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




