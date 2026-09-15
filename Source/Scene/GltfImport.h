#pragma once

#include <filesystem>

#include <glm/glm.hpp>

#include "Framework/Vulkan/GpuResources.h"
#include "Framework/Vulkan/UploadContext.h"
#include "SceneGpuResources.h"
#include <tiny_gltf.h>

namespace rtpt
{

// Loads a .gltf or .glb file from disk. Throws on a parse failure or an unsupported extension.
tinygltf::Model LoadGltfResources(const std::filesystem::path& filename);

// Uploads the model's first binary buffer and appends one scene mesh per supported triangle primitive.
// materialOffset maps the model's material indices into the scene material array; fallbackMaterialIndex serves primitives without a valid material.
// When importInstance is true, the node hierarchy is also flattened into world-space instances.
void ImportGltfData(GltfSceneResource& sceneResource, const tinygltf::Model& model, rtpt::ResourceAllocator& resources, rtpt::UploadContext& uploads, bool importInstance = false, uint32_t materialOffset = 0, uint32_t fallbackMaterialIndex = 0);

// Creates the device-addressable scene arrays. The caller fills the addresses into sceneInfo before creating its uniform buffer.
void CreateGltfSceneDataBuffers(GltfSceneResource& sceneResource, rtpt::ResourceAllocator& resources, rtpt::UploadContext& uploads);

// Creates the uniform buffer holding sceneInfo and uploads its current contents.
void CreateGltfSceneInfoBuffer(GltfSceneResource& sceneResource, rtpt::ResourceAllocator& resources, rtpt::UploadContext& uploads);

}  // namespace rtpt
