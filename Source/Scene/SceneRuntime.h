#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan_core.h>

#include "Camera/CameraController.h"
#include "Framework/Vulkan/AccelerationStructures.h"
#include "Framework/Vulkan/Descriptors.h"
#include "Framework/Vulkan/GpuExecution.h"
#include "Framework/Vulkan/GpuResources.h"
#include "Framework/Vulkan/UploadContext.h"
#include "Framework/Vulkan/VulkanDevice.h"
#include "SceneGpuResources.h"
#include "SceneUploader.h"

namespace rtpt
{

// SceneRuntime
// Owns the current GPU scene resources and its ray-tracing acceleration structures, and handles per-frame scene uniform updates.
// It defines the boundary between CPU scene choices and GPU upload, lifetime, and update operations.

class SceneRuntime
{
public:

  // CreateInfo
  // Vulkan services the runtime borrows. All four are required; the constructor throws if any is null.

  struct CreateInfo
  {
    // Logical device, used for descriptor writes and acceleration structure sizing.
    rtpt::VulkanDevice* device = nullptr;

    // Allocator for every scene buffer and image.
    rtpt::ResourceAllocator* resources = nullptr;

    // Staging path for scene data.
    rtpt::UploadContext* uploads = nullptr;

    // Queue submission and deferred resource retirement.
    rtpt::GpuExecution* execution = nullptr;
  };

  explicit SceneRuntime(const CreateInfo& createInfo);

  void Destroy();

  void RebuildScene(const SceneUploader::UploadInput& input, VkExtent2D viewport, bool resetCamera, rtpt::CameraController* camera);

  bool IsReady() const;

  void UpdateTextureDescriptors(rtpt::DescriptorPack& descPack, uint32_t maxTextureDescriptors) const;

  void UpdateSceneBuffer(VkCommandBuffer cmd, const glm::mat4& viewMatrix, const glm::mat4& projMatrix, const glm::vec3& cameraPosition, const VkExtent2D& viewportSize);

  void InvalidateFrameHistory();

  rtpt::GltfSceneResource& GetSceneResource();

  const rtpt::GltfSceneResource& GetSceneResource() const;

  shaderio::GltfSceneInfo& GetSceneInfo();

  const shaderio::GltfSceneInfo& GetSceneInfo() const;

  const std::vector<rtpt::AccelerationStructure>& GetBottomLevelAccelerationStructures() const;

  const rtpt::AccelerationStructure& GetTopLevelAccelerationStructure() const;

private:

  void BuildBottomLevelAccelerationStructures();

  void BuildTopLevelAccelerationStructure();

  void DestroyTopLevelAccelerationStructure();

  void DestroyBottomLevelAccelerationStructures();

  void DestroySceneResources();

  void DestroyTextures();

  // Logical device, used for descriptor writes and acceleration structure sizing.
  rtpt::VulkanDevice* m_Device = nullptr;

  // Allocator for every scene buffer and image.
  rtpt::ResourceAllocator* m_Resources = nullptr;

  // Staging path for scene data.
  rtpt::UploadContext* m_Uploads = nullptr;

  // Drained before a rebuild so no in-flight frame still references the old scene.
  rtpt::GpuExecution* m_Execution = nullptr;

  // Turns a scene definition into the resources below.
  SceneUploader m_SceneUploader;

  // Geometry, materials, light tables, and the scene uniform for the current scene.
  rtpt::GltfSceneResource m_SceneResource;

  // Every scene texture; material texture indices and the environment index refer to positions here.
  std::vector<SceneTexture> m_Textures;

  // CPU material attributes, parallel to m_SceneResource.materials.
  std::vector<rtpt::MaterialAttributes> m_MaterialAttributes;

  // One BLAS per scene mesh, indexed by mesh index.
  std::vector<rtpt::AccelerationStructure> m_BottomLevelAS;

  // TLAS instance records, kept alive until the TLAS is destroyed.
  std::vector<VkAccelerationStructureInstanceKHR> m_TlasInstances;

  // Device copy of m_TlasInstances, read by the TLAS build.
  rtpt::Buffer m_TlasInstancesBuffer;

  // Top-level acceleration structure over every instance with a BLAS.
  rtpt::AccelerationStructure m_TopLevelAS;

  // False until the first UpdateSceneBuffer after a reset. While false, the previous-frame values are seeded from the current frame so reprojection sees no motion.
  bool m_HasFrameHistory = false;

  // View matrix of the previous frame.
  glm::mat4 m_PreviousViewMatrix { 1.0f };

  // View-projection matrix of the previous frame.
  glm::mat4 m_PreviousViewProjMatrix { 1.0f };

  // View-projection matrix of frame N-2.
  glm::mat4 m_PreviousPreviousViewProjMatrix { 1.0f };

  // Camera position of the previous frame.
  glm::vec3 m_PreviousCameraPosition { 0.0f };

  // Camera position of frame N-2.
  glm::vec3 m_PreviousPreviousCameraPosition { 0.0f };
};

}  // namespace rtpt
