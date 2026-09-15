#pragma once

#include "Common/IoGltf.h"
#include "Framework/Vulkan/GpuResources.h"

#include <cstdint>
#include <vector>

namespace rtpt
{

// SceneTexture
// A sampled scene image paired with the sampler it is bound with, so both are released together.

struct SceneTexture
{
  // Uploaded texel data; left in SHADER_READ_ONLY_OPTIMAL after upload.
  rtpt::Image image;

  // Sampler written alongside the image into the texture descriptor arrays.
  rtpt::Sampler sampler;

  [[nodiscard]] VkDescriptorImageInfo Descriptor() const noexcept
  {
    return { .sampler = sampler.sampler, .imageView = image.descriptor.imageView, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
  }
};

// GltfSceneResource
// CPU scene arrays and their device-addressable GPU representation move as one bundle, so a scene replacement retires every borrowed address together.

struct GltfSceneResource
{
  // One entry per imported triangle primitive, across every model in the scene.
  std::vector<shaderio::GltfMesh> meshes;

  // Placed meshes with world transforms, indexed by the TLAS instance custom index.
  std::vector<shaderio::GltfInstance> instances;

  // GPU materials for every model, with scene material overrides already applied.
  std::vector<shaderio::GltfMetallicRoughness> materials;

  // World-space emissive triangles used for explicit light sampling.
  std::vector<shaderio::EmissiveTriangleLight> emissiveTriangles;

  // Normalized CDF over emissiveTriangles, weighted by area times emitted luminance.
  std::vector<float> emissiveTriangleCdf;

  // Normalized CDF over HDRI texels in row-major order.
  std::vector<float> environmentCdf;

  // Discrete probability of each HDRI texel, matching environmentCdf.
  std::vector<float> environmentPdf;

  // HDRI width in texels for the importance tables, or 0 when no table was built.
  uint32_t environmentWidth = 0;

  // HDRI height in texels for the importance tables, or 0 when no table was built.
  uint32_t environmentHeight = 0;

  // CPU copy of the scene uniform, rewritten every frame by SceneRuntime.
  shaderio::GltfSceneInfo sceneInfo {};

  // One uploaded glTF binary buffer per imported model; meshes point into these by device address.
  std::vector<rtpt::Buffer> bGltfDatas;

  // Device copy of meshes.
  rtpt::Buffer bMeshes;

  // Device copy of instances.
  rtpt::Buffer bInstances;

  // Device copy of materials.
  rtpt::Buffer bMaterials;

  // Device copy of emissiveTriangles.
  rtpt::Buffer bEmissiveTriangles;

  // Device copy of emissiveTriangleCdf.
  rtpt::Buffer bEmissiveTriangleCdf;

  // Device copy of environmentCdf.
  rtpt::Buffer bEnvironmentCdf;

  // Device copy of environmentPdf.
  rtpt::Buffer bEnvironmentPdf;

  // Uniform buffer holding sceneInfo; its existence marks the scene as ready.
  rtpt::Buffer bSceneInfo;

  // For each mesh, the index into bGltfDatas of the buffer its streams live in.
  std::vector<uint32_t> meshToBufferIndex;

  // For each mesh, the scene material index its primitive was authored with.
  std::vector<uint32_t> meshMaterialIndices;
};

}  // namespace rtpt
