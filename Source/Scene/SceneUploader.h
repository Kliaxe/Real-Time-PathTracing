#pragma once

// Role:
// Vulkan-facing upload/compiler step from scene definition to GPU resources.

#include <filesystem>
#include <optional>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "Common/GltfUtils.hpp"
#include "SceneTypes.h"
#include "nvvk/resources.hpp"
#include "nvvk/sampler_pool.hpp"
#include "nvvk/staging.hpp"

namespace nvapp
{
class Application;
}

namespace nvsamples
{

// SceneUploader is the Vulkan-facing compilation step that takes a resolved
// scene definition and produces GPU-ready buffers/images in caller-owned state.
class SceneUploader
{
public:
  struct UploadInput
  {
    const SceneDefinition&                  sceneDefinition;
    std::optional<std::filesystem::path>    selectedHdriRelativePath;
  };

  struct UploadState
  {
    GltfSceneResource&                sceneResource;
    std::vector<nvvk::Image>&         textures;
    std::vector<MaterialAttributes>&  materialAttributes;
  };

  SceneUploader(nvapp::Application* app, nvvk::ResourceAllocator* allocator, nvvk::StagingUploader* stagingUploader,
                nvvk::SamplerPool* samplerPool);

  int Upload(VkCommandBuffer cmd, const UploadInput& input, UploadState& state) const;

private:
  nvapp::Application*      m_App             = nullptr;
  nvvk::ResourceAllocator* m_Allocator       = nullptr;
  nvvk::StagingUploader*   m_StagingUploader = nullptr;
  nvvk::SamplerPool*       m_SamplerPool     = nullptr;
};

}  // namespace nvsamples
