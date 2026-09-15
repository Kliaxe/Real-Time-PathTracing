#include "SceneRuntime.h"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <vector>

#include <fmt/format.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "Framework/Platform/Log.h"
#include "Framework/Vulkan/Diagnostics.h"
#include "GltfImport.h"
#include "Shaders/ShaderIo.h"

namespace rtpt
{

namespace
{

// BottomLevelGeometry
// One mesh's BLAS geometry description paired with its build range, as AccelerationStructureBuild::AddGeometry takes them.

struct BottomLevelGeometry
{
  // Triangle geometry pointing into the uploaded glTF blob.
  VkAccelerationStructureGeometryKHR geometry {};

  // Primitive count for the build.
  VkAccelerationStructureBuildRangeInfoKHR range {};
};

// Describes a mesh to the BLAS build straight from its streams in the glTF blob, so no geometry is copied.
BottomLevelGeometry CreateBottomLevelGeometry(const shaderio::GltfMesh& gltfMesh)
{
  const shaderio::TriangleMesh triMesh = gltfMesh.triMesh;

  const VkAccelerationStructureGeometryTrianglesDataKHR triangles {
      .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
      .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
      .vertexData   = { .deviceAddress = VkDeviceAddress(gltfMesh.gltfBuffer) + triMesh.positions.offset },
      .vertexStride = triMesh.positions.byteStride,
      .maxVertex    = triMesh.positions.count - 1,
      .indexType    = VkIndexType(gltfMesh.indexType),
      .indexData    = { .deviceAddress = VkDeviceAddress(gltfMesh.gltfBuffer) + triMesh.indices.offset },
  };

  return {
      .geometry = {
          .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
          .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
          .geometry     = { .triangles = triangles },
      },
      .range = { .primitiveCount = static_cast<uint32_t>(triMesh.indices.count / 3U) },
  };
}

bool UsesAlphaMask(const shaderio::GltfMetallicRoughness& material)
{
  return material.alphaMode == shaderio::GltfAlphaMode::eMask;
}

// Checked accessors for the constructor's initializer list, which builds the SceneUploader from references before the body can validate anything.
rtpt::ResourceAllocator& RequireResources(const SceneRuntime::CreateInfo& createInfo)
{
  if(createInfo.resources == nullptr)
  {
    throw std::invalid_argument("SceneRuntime requires a resource allocator");
  }

  return *createInfo.resources;
}

rtpt::UploadContext& RequireUploads(const SceneRuntime::CreateInfo& createInfo)
{
  if(createInfo.uploads == nullptr)
  {
    throw std::invalid_argument("SceneRuntime requires an upload context");
  }

  return *createInfo.uploads;
}

}  // namespace

SceneRuntime::SceneRuntime(const CreateInfo& createInfo)
    : m_Device(createInfo.device)
    , m_Resources(createInfo.resources)
    , m_Uploads(createInfo.uploads)
    , m_Execution(createInfo.execution)
    , m_SceneUploader(RequireResources(createInfo), RequireUploads(createInfo))
{
  if(m_Device == nullptr || m_Resources == nullptr || m_Uploads == nullptr || m_Execution == nullptr)
  {
    throw std::invalid_argument("SceneRuntime requires initialized Vulkan services");
  }
}

void SceneRuntime::Destroy()
{
  // The TLAS references BLAS addresses and the BLASes reference scene buffers, so teardown runs from the top down.
  DestroyTopLevelAccelerationStructure();
  DestroyBottomLevelAccelerationStructures();
  DestroySceneResources();
  DestroyTextures();

  m_Execution->CollectRetiredResources();
}

void SceneRuntime::RebuildScene(const SceneUploader::UploadInput& input, VkExtent2D viewport, bool resetCamera, rtpt::CameraController* camera)
{
  // Teardown
  // Queued GPU work may still reference the old scene, so it is drained before anything is destroyed. Camera history from the old scene is meaningless for the new one.

  m_Execution->Drain();
  Destroy();
  InvalidateFrameHistory();

  // Upload
  // The uploader fills this runtime's containers; the device arrays must exist before their addresses can be written into the scene uniform.

  SceneUploader::UploadState uploadState {
      .sceneResource      = m_SceneResource,
      .textures           = m_Textures,
      .materialAttributes = m_MaterialAttributes,
  };

  const int environmentTextureIndex = m_SceneUploader.Upload(input, uploadState);

  CreateGltfSceneDataBuffers(m_SceneResource, *m_Resources, *m_Uploads);

  // Scene uniform
  // Initial values for the scene uniform: the HDRI switches on when one was loaded, the procedural sky starts off, and one default point light is set. Camera matrices are filled per frame by UpdateSceneBuffer.

  shaderio::GltfSceneInfo& sceneInfo = m_SceneResource.sceneInfo;

  sceneInfo.useSky                      = 0;
  sceneInfo.useHdrEnv                   = environmentTextureIndex >= 0 ? 1 : 0;
  sceneInfo.environmentTextureIndex     = environmentTextureIndex;
  sceneInfo.instances                   = reinterpret_cast<shaderio::GltfInstance*>(m_SceneResource.bInstances.address);
  sceneInfo.meshes                      = reinterpret_cast<shaderio::GltfMesh*>(m_SceneResource.bMeshes.address);
  sceneInfo.materials                   = reinterpret_cast<shaderio::GltfMetallicRoughness*>(m_SceneResource.bMaterials.address);
  sceneInfo.backgroundColor             = { 0.85F, 0.85F, 0.85F };
  sceneInfo.numLights                   = 1;
  sceneInfo.viewportSize                = { static_cast<float>(viewport.width), static_cast<float>(viewport.height) };
  sceneInfo.punctualLights[0].color     = glm::vec3(1.0F);
  sceneInfo.punctualLights[0].intensity = 4.0F;
  sceneInfo.punctualLights[0].position  = glm::vec3(1.0F);
  sceneInfo.punctualLights[0].direction = glm::vec3(1.0F);
  sceneInfo.punctualLights[0].type      = shaderio::GltfLightType::ePoint;
  sceneInfo.punctualLights[0].coneAngle = 0.9F;
  sceneInfo.emissiveTriangles           = reinterpret_cast<shaderio::EmissiveTriangleLight*>(m_SceneResource.bEmissiveTriangles.address);
  sceneInfo.emissiveTriangleCdf         = reinterpret_cast<float*>(m_SceneResource.bEmissiveTriangleCdf.address);
  sceneInfo.environmentCdf              = reinterpret_cast<float*>(m_SceneResource.bEnvironmentCdf.address);
  sceneInfo.environmentPdf              = reinterpret_cast<float*>(m_SceneResource.bEnvironmentPdf.address);
  sceneInfo.emissiveTriangleCount       = static_cast<uint32_t>(m_SceneResource.emissiveTriangles.size());
  sceneInfo.environmentWidth            = m_SceneResource.environmentWidth;
  sceneInfo.environmentHeight           = m_SceneResource.environmentHeight;

  CreateGltfSceneInfoBuffer(m_SceneResource, *m_Resources, *m_Uploads);

  // Acceleration structures
  // The TLAS references BLAS addresses, so the bottom level is built first.

  BuildBottomLevelAccelerationStructures();
  BuildTopLevelAccelerationStructure();

  // Camera
  // Optionally moves the camera to a fixed start pose that frames the origin, with clip planes suited to the built-in scenes.

  if(resetCamera && camera != nullptr)
  {
    rtpt::CameraState state = camera->State();
    state.clipPlanes        = { 0.01F, 100.0F };

    camera->SetState(state);
    camera->SetLookAt({ 0.0F, 0.5F, 5.0F }, { 0.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F });
  }
}

bool SceneRuntime::IsReady() const
{
  return static_cast<bool>(m_SceneResource.bSceneInfo);
}

void SceneRuntime::UpdateTextureDescriptors(rtpt::DescriptorPack& descPack, uint32_t maxTextureDescriptors) const
{
  // Capacity
  // The descriptor arrays have a fixed size. Textures beyond it are left out with a warning.

  const uint32_t textureCount = std::min(static_cast<uint32_t>(m_Textures.size()), maxTextureDescriptors);

  if(textureCount == 0)
  {
    return;
  }

  if(textureCount < m_Textures.size())
  {
    rtpt::Log(rtpt::LogLevel::Warning, fmt::format("Texture count ({}) exceeds descriptor capacity ({}); extra textures are ignored", m_Textures.size(), maxTextureDescriptors));
  }

  // Image infos
  // Each texture is published three ways: as a combined image sampler, and as the separate image and sampler that HLSL-style bindings declare.

  std::vector<VkDescriptorImageInfo> combined(textureCount);
  std::vector<VkDescriptorImageInfo> sampled(textureCount);
  std::vector<VkDescriptorImageInfo> samplers(textureCount);

  for(uint32_t textureIndex = 0; textureIndex < textureCount; ++textureIndex)
  {
    combined[textureIndex] = m_Textures[textureIndex].Descriptor();
    sampled[textureIndex]  = {
        .imageView   = combined[textureIndex].imageView,
        .imageLayout = combined[textureIndex].imageLayout,
    };
    samplers[textureIndex] = { .sampler = combined[textureIndex].sampler };
  }

  // Writes
  // The same arrays go into every set of the pack.

  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(descPack.Sets().size() * 3U);

  for(uint32_t setIndex = 0; setIndex < descPack.Sets().size(); ++setIndex)
  {
    VkWriteDescriptorSet combinedWrite = descPack.MakeWrite(shaderio::BindingPoints::eTextures, setIndex, 0, textureCount);
    combinedWrite.pImageInfo           = combined.data();
    writes.push_back(combinedWrite);

    VkWriteDescriptorSet sampledWrite = descPack.MakeWrite(shaderio::BindingPoints::eHlslTextures, setIndex, 0, textureCount);
    sampledWrite.pImageInfo           = sampled.data();
    writes.push_back(sampledWrite);

    VkWriteDescriptorSet samplerWrite = descPack.MakeWrite(shaderio::BindingPoints::eHlslTextureSamplers, setIndex, 0, textureCount);
    samplerWrite.pImageInfo           = samplers.data();
    writes.push_back(samplerWrite);
  }

  vkUpdateDescriptorSets(m_Device->Handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void SceneRuntime::UpdateSceneBuffer(VkCommandBuffer cmd, const glm::mat4& viewMatrix, const glm::mat4& projMatrix, const glm::vec3& cameraPosition, const VkExtent2D& viewportSize)
{
  const glm::mat4 currentViewProjMatrix = projMatrix * viewMatrix;

  // Without history, previous frames are seeded with the current one so temporal passes see a stationary camera instead of a jump from identity.
  if(!m_HasFrameHistory)
  {
    m_PreviousViewMatrix             = viewMatrix;
    m_PreviousViewProjMatrix         = currentViewProjMatrix;
    m_PreviousPreviousViewProjMatrix = currentViewProjMatrix;
    m_PreviousCameraPosition         = cameraPosition;
    m_PreviousPreviousCameraPosition = cameraPosition;
  }

  // Camera state

  shaderio::GltfSceneInfo& sceneInfo = m_SceneResource.sceneInfo;

  sceneInfo.viewProjMatrix         = currentViewProjMatrix;
  sceneInfo.viewMatrix             = viewMatrix;
  sceneInfo.prevViewProjMatrix     = m_PreviousViewProjMatrix;
  sceneInfo.prevViewMatrix         = m_PreviousViewMatrix;
  sceneInfo.prevPrevViewProjMatrix = m_PreviousPreviousViewProjMatrix;
  sceneInfo.viewProjInvMatrix      = glm::inverse(currentViewProjMatrix);
  sceneInfo.viewInvMatrix          = glm::inverse(viewMatrix);
  sceneInfo.cameraPosition         = cameraPosition;
  sceneInfo.prevCameraPosition     = m_PreviousCameraPosition;
  sceneInfo.prevPrevCameraPosition = m_PreviousPreviousCameraPosition;
  sceneInfo.viewportSize           = { static_cast<float>(viewportSize.width), static_cast<float>(viewportSize.height) };

  // Upload
  // The whole uniform is rewritten inside the frame's command buffer, so the update lands in order with the passes that read it. Barriers bracket the transfer.

  constexpr VkPipelineStageFlags2 shaderReadStages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

  const VkBufferMemoryBarrier2 beforeUpdate {
      .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask  = shaderReadStages,
      .srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
      .dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .buffer        = m_SceneResource.bSceneInfo.buffer,
      .size          = sizeof(shaderio::GltfSceneInfo),
  };

  const VkDependencyInfo beforeDependency {
      .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1,
      .pBufferMemoryBarriers    = &beforeUpdate,
  };

  vkCmdPipelineBarrier2(cmd, &beforeDependency);

  vkCmdUpdateBuffer(cmd, m_SceneResource.bSceneInfo.buffer, 0, sizeof(shaderio::GltfSceneInfo), &sceneInfo);

  const VkBufferMemoryBarrier2 afterUpdate {
      .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask  = shaderReadStages,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
      .buffer        = m_SceneResource.bSceneInfo.buffer,
      .size          = sizeof(shaderio::GltfSceneInfo),
  };

  const VkDependencyInfo afterDependency {
      .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1,
      .pBufferMemoryBarriers    = &afterUpdate,
  };

  vkCmdPipelineBarrier2(cmd, &afterDependency);

  // History
  // Shift the history by one frame. N-2 must take the old previous values before those are overwritten with the current frame.

  m_PreviousViewMatrix             = viewMatrix;
  m_PreviousPreviousViewProjMatrix = m_PreviousViewProjMatrix;
  m_PreviousViewProjMatrix         = currentViewProjMatrix;
  m_PreviousPreviousCameraPosition = m_PreviousCameraPosition;
  m_PreviousCameraPosition         = cameraPosition;
  m_HasFrameHistory                = true;
}

void SceneRuntime::InvalidateFrameHistory()
{
  // The identity values are placeholders; the next UpdateSceneBuffer replaces them with the current frame before use.
  m_HasFrameHistory                = false;
  m_PreviousViewMatrix             = glm::mat4(1.0F);
  m_PreviousViewProjMatrix         = glm::mat4(1.0F);
  m_PreviousPreviousViewProjMatrix = glm::mat4(1.0F);
  m_PreviousCameraPosition         = glm::vec3(0.0F);
  m_PreviousPreviousCameraPosition = glm::vec3(0.0F);
}

GltfSceneResource& SceneRuntime::GetSceneResource()
{
  return m_SceneResource;
}

const GltfSceneResource& SceneRuntime::GetSceneResource() const
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

const std::vector<rtpt::AccelerationStructure>& SceneRuntime::GetBottomLevelAccelerationStructures() const
{
  return m_BottomLevelAS;
}

const rtpt::AccelerationStructure& SceneRuntime::GetTopLevelAccelerationStructure() const
{
  return m_TopLevelAS;
}

void SceneRuntime::BuildBottomLevelAccelerationStructures()
{
  if(m_SceneResource.meshes.empty())
  {
    return;
  }

  // Build descriptions
  // One BLAS per mesh, so m_BottomLevelAS lines up with mesh indices and instances can look theirs up directly.

  std::vector<rtpt::AccelerationStructureBuild> builds;
  builds.reserve(m_SceneResource.meshes.size());

  for(const shaderio::GltfMesh& mesh : m_SceneResource.meshes)
  {
    const BottomLevelGeometry geometry = CreateBottomLevelGeometry(mesh);

    rtpt::AccelerationStructureBuild build(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
    build.AddGeometry(geometry.geometry, geometry.range);

    rtpt::CheckVk(build.Finalize(m_Device->Handle()), "finalize bottom-level acceleration structure");

    builds.push_back(std::move(build));
  }

  // Build
  // All BLASes are built in one batch; the builder aligns scratch memory to the device's minimum scratch offset alignment.

  m_BottomLevelAS.resize(builds.size());

  rtpt::AccelerationStructureBuilder builder;
  builder.Initialize(*m_Resources, *m_Execution, m_Device->Support().accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment);

  rtpt::CheckVk(builder.Build(builds, m_BottomLevelAS), "build bottom-level acceleration structures");
}

void SceneRuntime::BuildTopLevelAccelerationStructure()
{
  if(m_SceneResource.instances.empty() || m_BottomLevelAS.empty())
  {
    return;
  }

  // Instance records
  // Exactly one TLAS instance per scene instance, in the same order, because shaders index the scene instance array with InstanceIndex(). The custom index records the scene instance index as well.
  // Both triangle faces are always hit. Only alpha-masked materials leave opacity to the shaders; everything else is forced opaque so any-hit shaders are skipped.

  m_TlasInstances.reserve(m_SceneResource.instances.size());

  for(size_t instanceIndex = 0; instanceIndex < m_SceneResource.instances.size(); ++instanceIndex)
  {
    const shaderio::GltfInstance& sceneInstance = m_SceneResource.instances[instanceIndex];

    // Every mesh gets a BLAS, so this only fires if the scene arrays and the acceleration structures fall out of sync; skipping the instance would silently shift every later InstanceIndex().
    if(sceneInstance.meshIndex >= m_BottomLevelAS.size())
    {
      throw std::logic_error(fmt::format("Scene instance {} references mesh {}, but only {} BLASes were built; the TLAS needs one instance per scene instance so InstanceIndex() matches the scene instance array", instanceIndex, sceneInstance.meshIndex, m_BottomLevelAS.size()));
    }

    VkGeometryInstanceFlagsKHR flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;

    if(sceneInstance.materialIndex < m_SceneResource.materials.size() && !UsesAlphaMask(m_SceneResource.materials[sceneInstance.materialIndex]))
    {
      flags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
    }

    m_TlasInstances.push_back({
        .transform                              = rtpt::ToTransformMatrix(glm::value_ptr(sceneInstance.transform)),
        .instanceCustomIndex                    = static_cast<uint32_t>(instanceIndex),
        .mask                                   = 0xFF,
        .instanceShaderBindingTableRecordOffset = 0,
        .flags                                  = flags,
        .accelerationStructureReference         = m_BottomLevelAS[sceneInstance.meshIndex].address,
    });
  }

  // Instance buffer
  // The build reads the instance records from device memory, aligned for VkAccelerationStructureInstanceKHR.

  const std::span instances(m_TlasInstances);

  rtpt::CheckVk(m_Resources->CreateBuffer(m_TlasInstancesBuffer, instances.size_bytes(), VK_BUFFER_USAGE_2_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, alignof(VkAccelerationStructureInstanceKHR)), "ResourceAllocator::CreateBuffer(TLAS instances)");

  m_Uploads->UploadBuffer(m_TlasInstancesBuffer.buffer, 0, std::as_bytes(instances), { .stages = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, .access = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR });

  // Build

  const VkAccelerationStructureGeometryInstancesDataKHR instanceData {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
      .data  = { .deviceAddress = m_TlasInstancesBuffer.address },
  };

  const VkAccelerationStructureGeometryKHR geometry {
      .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
      .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
      .geometry     = { .instances = instanceData },
  };

  const VkAccelerationStructureBuildRangeInfoKHR range {
      .primitiveCount = static_cast<uint32_t>(m_TlasInstances.size()),
  };

  rtpt::AccelerationStructureBuild build(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
  build.AddGeometry(geometry, range);

  rtpt::CheckVk(build.Finalize(m_Device->Handle()), "finalize top-level acceleration structure");

  rtpt::AccelerationStructureBuilder builder;
  builder.Initialize(*m_Resources, *m_Execution, m_Device->Support().accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment);

  rtpt::CheckVk(builder.Build(build, m_TopLevelAS), "build top-level acceleration structure");
}

void SceneRuntime::DestroyTopLevelAccelerationStructure()
{
  m_TopLevelAS.Reset();
  m_TlasInstancesBuffer.Reset();
  m_TlasInstances.clear();
}

void SceneRuntime::DestroyBottomLevelAccelerationStructures()
{
  m_BottomLevelAS.clear();
}

void SceneRuntime::DestroySceneResources()
{
  m_SceneResource = {};
  m_MaterialAttributes.clear();
}

void SceneRuntime::DestroyTextures()
{
  m_Textures.clear();
}

}  // namespace rtpt
