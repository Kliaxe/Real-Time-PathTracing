#pragma once

// Role:
// Owns GPU scene lifetime and frame updates; boundary between scene data and rendering.

#include <filesystem>
#include <optional>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan_core.h>

#include "Common/GltfUtils.hpp"
#include "SceneUploader.h"

namespace nvapp
{
class Application;
}

namespace nvutils
{
class CameraManipulator;
}

namespace nvvk
{
class DescriptorPack;
class ResourceAllocator;
class SamplerPool;
class StagingUploader;
struct Image;
}  // namespace nvvk

namespace nvsamples
{

// SceneRuntime owns the current GPU scene resources and defines the boundary
// between CPU scene choices and GPU upload/lifetime/update operations.
class SceneRuntime
{
public:
  struct CreateInfo
  {
    nvapp::Application*      app             = nullptr;
    nvvk::ResourceAllocator* allocator       = nullptr;
    nvvk::StagingUploader*   stagingUploader = nullptr;
    nvvk::SamplerPool*       samplerPool     = nullptr;
  };

  explicit SceneRuntime(const CreateInfo& createInfo);

  void Destroy();
  void RebuildScene(VkQueue queue, const SceneUploader::UploadInput& input, bool resetCamera, nvutils::CameraManipulator* cameraManip);

  bool IsReady() const;

  void UpdateTextureDescriptors(VkDevice device, nvvk::DescriptorPack& descPack, uint32_t maxTextureDescriptors) const;
  void UpdateSceneBuffer(VkCommandBuffer cmd, const glm::mat4& viewMatrix, const glm::mat4& projMatrix, const glm::vec3& cameraPosition,
                         const VkExtent2D& viewportSize);

  nvsamples::GltfSceneResource&       GetSceneResource();
  const nvsamples::GltfSceneResource& GetSceneResource() const;
  shaderio::GltfSceneInfo&            GetSceneInfo();
  const shaderio::GltfSceneInfo&      GetSceneInfo() const;
  const std::vector<nvvk::AccelerationStructure>& GetBottomLevelAccelerationStructures() const;
  const nvvk::AccelerationStructure& GetTopLevelAccelerationStructure() const;

private:
  void BuildBottomLevelAccelerationStructures();
  void BuildTopLevelAccelerationStructure();
  void DestroyTopLevelAccelerationStructure();
  void DestroyBottomLevelAccelerationStructures();
  void DestroySceneResources();
  void DestroyTextures();

  nvapp::Application*      m_App             = nullptr;
  nvvk::ResourceAllocator* m_Allocator       = nullptr;
  nvvk::StagingUploader*   m_StagingUploader = nullptr;

  SceneUploader m_SceneUploader;

  nvsamples::GltfSceneResource               m_SceneResource;
  std::vector<nvvk::Image>                   m_Textures;
  std::vector<nvsamples::MaterialAttributes> m_MaterialAttributes;
  std::vector<nvvk::AccelerationStructure>   m_BottomLevelAS;
  std::vector<VkAccelerationStructureInstanceKHR> m_TlasInstances;
  nvvk::Buffer                                    m_TlasInstancesBuffer;
  nvvk::AccelerationStructure                     m_TopLevelAS;
};

}  // namespace nvsamples
