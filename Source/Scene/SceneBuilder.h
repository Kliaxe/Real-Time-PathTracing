#pragma once

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

// Builds one runtime scene from a high-level SceneDefinition.
// The builder owns no GPU resources; it writes into caller-owned containers so
// existing lifetime/destruction in RtFoundation remains unchanged.
class SceneBuilder
{
public:
  struct BuildInput
  {
    const SceneDefinition&                  sceneDefinition;
    std::optional<std::filesystem::path>    selectedHdriRelativePath;
  };

  struct BuildState
  {
    GltfSceneResource&                sceneResource;
    std::vector<nvvk::Image>&         textures;
    std::vector<MaterialAttributes>&  materialAttributes;
  };

  SceneBuilder(nvapp::Application* app, nvvk::ResourceAllocator* allocator, nvvk::StagingUploader* stagingUploader,
               nvvk::SamplerPool* samplerPool);

  // Appends scene geometry/materials/textures into BuildState and returns the
  // selected environment texture index in the texture array (-1 if none).
  int BuildScene(VkCommandBuffer cmd, const BuildInput& input, BuildState& state) const;

private:
  nvapp::Application*   m_App            = nullptr;
  nvvk::ResourceAllocator* m_Allocator   = nullptr;
  nvvk::StagingUploader*   m_StagingUploader = nullptr;
  nvvk::SamplerPool*       m_SamplerPool = nullptr;
};

}  // namespace nvsamples
