#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "Framework/Vulkan/GpuResources.h"
#include "Framework/Vulkan/UploadContext.h"
#include "GltfImport.h"
#include "SceneTypes.h"

namespace rtpt
{

// SceneUploader
// The Vulkan-facing compilation step that takes a resolved scene definition and produces GPU-ready buffers and images in caller-owned state.
// It performs glTF parsing, texture and material packing, and light sampling table construction, but owns none of the results, so SceneRuntime controls their lifetime.

class SceneUploader
{
public:

  // UploadInput
  // What to build: the chosen scene, an optional environment map, and where to look for their files.

  struct UploadInput
  {
    // Scene whose models are loaded.
    const SceneDefinition& sceneDefinition;

    // Content-relative HDRI path; empty loads no environment map.
    std::optional<std::filesystem::path> selectedHdriRelativePath;

    // Directories searched in order for relative asset paths.
    std::span<const std::filesystem::path> contentRoots;
  };

  // UploadState
  // Caller-owned containers the upload appends to. They are expected to be empty on entry.

  struct UploadState
  {
    // Receives meshes, instances, materials, glTF buffers, and light sampling tables.
    GltfSceneResource& sceneResource;

    // Receives every uploaded texture; material texture indices refer to positions here.
    std::vector<SceneTexture>& textures;

    // Receives the CPU material attributes, parallel to sceneResource.materials.
    std::vector<MaterialAttributes>& materialAttributes;
  };

  SceneUploader(rtpt::ResourceAllocator& resources, rtpt::UploadContext& uploads);

  // Returns the texture index of the environment map, or -1 when none was loaded.
  int Upload(const UploadInput& input, UploadState& state) const;

private:

  // Creates the buffers, images, and samplers the scene is made of.
  rtpt::ResourceAllocator* m_Resources = nullptr;

  // Copies CPU data into those resources.
  rtpt::UploadContext* m_Uploads = nullptr;
};

}  // namespace rtpt
