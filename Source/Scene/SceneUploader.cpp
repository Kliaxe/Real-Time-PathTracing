#include "SceneUploader.h"

// Role:
// Performs glTF parsing, texture/material packing, and import upload for runtime scenes.

#include <algorithm>
#include <span>
#include <vector>

#include <glm/glm.hpp>
#include <nvapp/application.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/default_structs.hpp>
#include <nvvk/sampler_pool.hpp>
#include <tinygltf/tiny_gltf.h>

#include "Common/PathUtils.hpp"
#include "Common/Utils.hpp"
#include "nvutils/file_operations.hpp"
#include "nvutils/logger.hpp"

namespace nvsamples
{

namespace
{

struct MaterialRange
{
  uint32_t offset = 0;
  uint32_t count  = 0;
};

float Clamp01(float value)
{
  return std::clamp(value, 0.0f, 1.0f);
}

float ReadValueAsFloat(const tinygltf::Value& value, float fallback)
{
  if(value.IsNumber())
  {
    return static_cast<float>(value.GetNumberAsDouble());
  }
  if(value.IsInt())
  {
    return static_cast<float>(value.Get<int>());
  }
  return fallback;
}

const tinygltf::Value* FindObjectMember(const tinygltf::Value::Object& object, const char* key)
{
  const auto it = object.find(key);
  if(it == object.end())
  {
    return nullptr;
  }
  return &it->second;
}

float ReadObjectNumber(const tinygltf::Value::Object& object, const char* key, float fallback)
{
  const tinygltf::Value* value = FindObjectMember(object, key);
  if(value == nullptr)
  {
    return fallback;
  }
  return ReadValueAsFloat(*value, fallback);
}

glm::vec3 ReadObjectVec3(const tinygltf::Value::Object& object, const char* key, const glm::vec3& fallback)
{
  const tinygltf::Value* value = FindObjectMember(object, key);
  if(value == nullptr || !value->IsArray())
  {
    return fallback;
  }

  const tinygltf::Value::Array& arr = value->Get<tinygltf::Value::Array>();
  if(arr.size() < 3)
  {
    return fallback;
  }

  return glm::vec3(ReadValueAsFloat(arr[0], fallback.x), ReadValueAsFloat(arr[1], fallback.y), ReadValueAsFloat(arr[2], fallback.z));
}

const tinygltf::Value::Object* FindMaterialExtensionObject(const tinygltf::Material& material, const char* extensionName)
{
  const auto extIt = material.extensions.find(extensionName);
  if(extIt == material.extensions.end() || !extIt->second.IsObject())
  {
    return nullptr;
  }

  return &extIt->second.Get<tinygltf::Value::Object>();
}

MaterialAttributes ParseMaterialAttributes(const tinygltf::Material& src)
{
  MaterialAttributes dst{};

  if(src.pbrMetallicRoughness.baseColorFactor.size() == 4)
  {
    dst.albedo = glm::vec3(static_cast<float>(src.pbrMetallicRoughness.baseColorFactor[0]),
                           static_cast<float>(src.pbrMetallicRoughness.baseColorFactor[1]),
                           static_cast<float>(src.pbrMetallicRoughness.baseColorFactor[2]));
  }
  if(src.emissiveFactor.size() == 3)
  {
    dst.emission = glm::vec3(static_cast<float>(src.emissiveFactor[0]), static_cast<float>(src.emissiveFactor[1]),
                             static_cast<float>(src.emissiveFactor[2]));
  }
  dst.metallic  = static_cast<float>(src.pbrMetallicRoughness.metallicFactor);
  dst.roughness = static_cast<float>(src.pbrMetallicRoughness.roughnessFactor);

  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_transmission"))
  {
    dst.transmission = ReadObjectNumber(*ext, "transmissionFactor", dst.transmission);
  }
  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_ior"))
  {
    dst.refraction = ReadObjectNumber(*ext, "ior", dst.refraction);
  }
  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_clearcoat"))
  {
    dst.clearcoat          = ReadObjectNumber(*ext, "clearcoatFactor", dst.clearcoat);
    dst.clearcoatRoughness = ReadObjectNumber(*ext, "clearcoatRoughnessFactor", dst.clearcoatRoughness);
  }
  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_specular"))
  {
    dst.specular = ReadObjectNumber(*ext, "specularFactor", dst.specular);
    const glm::vec3 specColor = ReadObjectVec3(*ext, "specularColorFactor", glm::vec3(1.0f));
    dst.specularTint          = (specColor.x + specColor.y + specColor.z) / 3.0f;
  }
  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_anisotropy"))
  {
    dst.anisotropy = ReadObjectNumber(*ext, "anisotropyStrength", dst.anisotropy);
  }
  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_sheen"))
  {
    dst.sheenRoughness = ReadObjectNumber(*ext, "sheenRoughnessFactor", dst.sheenRoughness);
    const glm::vec3 sheenColor = ReadObjectVec3(*ext, "sheenColorFactor", glm::vec3(0.0f));
    dst.sheenTint              = (sheenColor.x + sheenColor.y + sheenColor.z) / 3.0f;
  }
  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_volume"))
  {
    dst.subsurface = ReadObjectNumber(*ext, "thicknessFactor", dst.subsurface);
  }

  dst.specular           = Clamp01(dst.specular);
  dst.specularTint       = Clamp01(dst.specularTint);
  dst.metallic           = Clamp01(dst.metallic);
  dst.roughness          = Clamp01(dst.roughness);
  dst.subsurface         = Clamp01(dst.subsurface);
  dst.anisotropy         = Clamp01(dst.anisotropy);
  dst.sheenRoughness     = Clamp01(dst.sheenRoughness);
  dst.sheenTint          = Clamp01(dst.sheenTint);
  dst.clearcoat          = Clamp01(dst.clearcoat);
  dst.clearcoatRoughness = Clamp01(dst.clearcoatRoughness);
  dst.transmission       = Clamp01(dst.transmission);
  return dst;
}

shaderio::GltfMetallicRoughness ToGpuMaterial(const MaterialAttributes& materialAttributes, int baseColorTextureIndex, float alphaCutoff,
                                              int alphaMode)
{
  shaderio::GltfMetallicRoughness dst{};
  dst.baseColorFactor       = glm::vec4(materialAttributes.albedo, 1.0f);
  dst.metallicFactor        = materialAttributes.metallic;
  dst.roughnessFactor       = materialAttributes.roughness;
  dst.baseColorTextureIndex = baseColorTextureIndex;
  dst.alphaCutoff           = alphaCutoff;
  dst.alphaMode             = alphaMode;
  return dst;
}

}  // namespace

SceneUploader::SceneUploader(nvapp::Application* app, nvvk::ResourceAllocator* allocator, nvvk::StagingUploader* stagingUploader,
                             nvvk::SamplerPool* samplerPool)
    : m_App(app)
    , m_Allocator(allocator)
    , m_StagingUploader(stagingUploader)
    , m_SamplerPool(samplerPool)
{
}

int SceneUploader::Upload(VkCommandBuffer cmd, const UploadInput& input, UploadState& state) const
{
  auto appendRgbaTexture = [&](const std::span<const unsigned char> rgbaPixels, uint32_t width, uint32_t height) -> int {
    if(width == 0 || height == 0 || rgbaPixels.empty())
    {
      return -1;
    }

    VkImageCreateInfo imageInfo = DEFAULT_VkImageCreateInfo;
    imageInfo.format            = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.usage             = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.extent            = {width, height, 1};

    nvvk::Image texture;
    NVVK_CHECK(m_Allocator->createImage(texture, imageInfo, DEFAULT_VkImageViewCreateInfo));
    NVVK_CHECK(m_StagingUploader->appendImage(texture, rgbaPixels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    m_SamplerPool->acquireSampler(texture.descriptor.sampler);
    state.textures.emplace_back(texture);
    return static_cast<int>(state.textures.size()) - 1;
  };

  auto loadBaseColorImageTexture = [&](const tinygltf::Image& image, const std::filesystem::path& modelPath) -> int {
    if(image.width > 0 && image.height > 0 && !image.image.empty())
    {
      const int srcChannels = std::clamp(image.component, 1, 4);
      if(image.bits != 8 || srcChannels <= 0)
      {
        LOGW("Skipping unsupported glTF image format (%d bits, %d channels)\n", image.bits, image.component);
        return -1;
      }

      const size_t pixelCount    = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
      const size_t requiredBytes = pixelCount * static_cast<size_t>(srcChannels);
      if(image.image.size() < requiredBytes)
      {
        LOGW("Skipping invalid glTF image payload (expected at least %zu bytes, got %zu)\n", requiredBytes, image.image.size());
        return -1;
      }

      std::vector<unsigned char> rgba(pixelCount * 4, 255);
      for(size_t i = 0; i < pixelCount; ++i)
      {
        const size_t src = i * static_cast<size_t>(srcChannels);
        const size_t dst = i * 4;
        rgba[dst + 0]    = image.image[src + 0];
        if(srcChannels >= 2)
          rgba[dst + 1] = image.image[src + 1];
        if(srcChannels >= 3)
          rgba[dst + 2] = image.image[src + 2];
        if(srcChannels >= 4)
          rgba[dst + 3] = image.image[src + 3];
      }

      return appendRgbaTexture(rgba, static_cast<uint32_t>(image.width), static_cast<uint32_t>(image.height));
    }

    if(!image.uri.empty())
    {
      std::error_code ec;
      const std::filesystem::path uriPath     = std::filesystem::path(image.uri);
      const std::filesystem::path texturePath = std::filesystem::weakly_canonical(modelPath.parent_path() / uriPath, ec);
      if(!ec && std::filesystem::exists(texturePath, ec))
      {
        nvvk::Image texture = nvsamples::LoadAndCreateImage(cmd, *m_StagingUploader, m_App->getDevice(), texturePath);
        m_SamplerPool->acquireSampler(texture.descriptor.sampler);
        state.textures.emplace_back(texture);
        return static_cast<int>(state.textures.size()) - 1;
      }
    }

    return -1;
  };

  auto loadBaseColorTextureIndices = [&](const tinygltf::Model& model, const std::filesystem::path& modelPath) -> std::vector<int> {
    std::vector<int> textureMap(model.textures.size(), -1);
    std::vector<int> sourceMap(model.images.size(), -1);
    for(size_t i = 0; i < model.textures.size(); ++i)
    {
      const tinygltf::Texture& texture = model.textures[i];
      if(texture.source < 0 || texture.source >= static_cast<int>(model.images.size()))
      {
        continue;
      }
      const int sourceIndex = texture.source;
      if(sourceMap[sourceIndex] < 0)
      {
        sourceMap[sourceIndex] = loadBaseColorImageTexture(model.images[sourceIndex], modelPath);
      }
      textureMap[i] = sourceMap[sourceIndex];
    }
    return textureMap;
  };

  auto addModelMaterials = [&](const tinygltf::Model& model, const std::vector<int>& textureMap) -> MaterialRange {
    const uint32_t materialOffset   = static_cast<uint32_t>(state.sceneResource.materials.size());
    const uint32_t attributesOffset = static_cast<uint32_t>(state.materialAttributes.size());

    if(model.materials.empty())
    {
      MaterialAttributes defaultMaterial{};
      state.materialAttributes.push_back(defaultMaterial);
      state.sceneResource.materials.push_back(ToGpuMaterial(defaultMaterial, -1, 0.5f, shaderio::GltfAlphaMode::eOpaque));
      return {.offset = materialOffset, .count = 1};
    }

    for(const tinygltf::Material& src : model.materials)
    {
      const MaterialAttributes parsedMaterial = ParseMaterialAttributes(src);
      state.materialAttributes.push_back(parsedMaterial);

      const int baseColorTex = src.pbrMetallicRoughness.baseColorTexture.index;
      int       baseColorTextureIndex = -1;
      if(baseColorTex >= 0 && baseColorTex < static_cast<int>(textureMap.size()))
      {
        baseColorTextureIndex = textureMap[baseColorTex];
      }

      int alphaMode = shaderio::GltfAlphaMode::eOpaque;
      if(src.alphaMode == "MASK")
        alphaMode = shaderio::GltfAlphaMode::eMask;
      else if(src.alphaMode == "BLEND")
        alphaMode = shaderio::GltfAlphaMode::eBlend;

      state.sceneResource.materials.push_back(
          ToGpuMaterial(parsedMaterial, baseColorTextureIndex, static_cast<float>(src.alphaCutoff), alphaMode));
    }
    return {.offset = materialOffset, .count = static_cast<uint32_t>(state.materialAttributes.size() - attributesOffset)};
  };

  auto applyMaterialOverrides = [&](const MaterialRange& range, const std::vector<SceneMaterialOverride>& overrides) {
    if(range.count == 0 || overrides.empty())
      return;

    for(const SceneMaterialOverride& overrideDef : overrides)
    {
      uint32_t begin = range.offset;
      uint32_t end   = range.offset + range.count;
      if(overrideDef.materialSlot >= 0)
      {
        const uint32_t slot = static_cast<uint32_t>(overrideDef.materialSlot);
        if(slot >= range.count)
          continue;
        begin = range.offset + slot;
        end   = begin + 1;
      }

      for(uint32_t i = begin; i < end; ++i)
      {
        state.materialAttributes[i] = overrideDef.attributes;
        shaderio::GltfMetallicRoughness& gpuMaterial = state.sceneResource.materials[i];
        gpuMaterial.baseColorFactor                    = glm::vec4(overrideDef.attributes.albedo, gpuMaterial.baseColorFactor.w);
        gpuMaterial.metallicFactor                     = overrideDef.attributes.metallic;
        gpuMaterial.roughnessFactor                    = overrideDef.attributes.roughness;
      }
    }
  };

  int environmentTextureIndex = -1;
  if(input.selectedHdriRelativePath.has_value())
  {
    const std::filesystem::path hdriPath = nvutils::findFile(*input.selectedHdriRelativePath, nvsamples::GetContentDirs());
    nvvk::Image texture                  = nvsamples::LoadAndCreateImage(cmd, *m_StagingUploader, m_App->getDevice(), hdriPath);
    m_SamplerPool->acquireSampler(texture.descriptor.sampler);
    state.textures.emplace_back(texture);
    environmentTextureIndex = static_cast<int>(state.textures.size()) - 1;
  }

  const std::vector<std::filesystem::path> contentDirs = nvsamples::GetContentDirs();
  for(const SceneModelEntry& modelEntry : input.sceneDefinition.models)
  {
    const std::filesystem::path modelPath = nvutils::findFile(modelEntry.assetPath, contentDirs);
    const tinygltf::Model       model     = nvsamples::LoadGltfResources(modelPath);

    const std::vector<int> textureMap     = loadBaseColorTextureIndices(model, modelPath);
    const MaterialRange    materialRange  = addModelMaterials(model, textureMap);
    const uint32_t         meshStartIndex = static_cast<uint32_t>(state.sceneResource.meshes.size());
    const uint32_t         instanceStart  = static_cast<uint32_t>(state.sceneResource.instances.size());

    nvsamples::ImportGltfData(state.sceneResource, model, *m_StagingUploader, modelEntry.importNodeInstances, materialRange.offset,
                              materialRange.offset);

    uint32_t importedInstanceCount = static_cast<uint32_t>(state.sceneResource.instances.size()) - instanceStart;
    if(importedInstanceCount == 0)
    {
      const uint32_t meshCount = static_cast<uint32_t>(state.sceneResource.meshes.size()) - meshStartIndex;
      for(uint32_t meshLocal = 0; meshLocal < meshCount; ++meshLocal)
      {
        const uint32_t meshIndex = meshStartIndex + meshLocal;
        uint32_t       materialIndex = materialRange.offset;
        if(meshIndex < state.sceneResource.meshMaterialIndices.size())
          materialIndex = state.sceneResource.meshMaterialIndices[meshIndex];
        state.sceneResource.instances.push_back(
            {.transform = glm::mat4(1.0f), .materialIndex = materialIndex, .meshIndex = meshIndex});
      }
      importedInstanceCount = static_cast<uint32_t>(state.sceneResource.instances.size()) - instanceStart;
    }

    for(uint32_t i = 0; i < importedInstanceCount; ++i)
    {
      shaderio::GltfInstance& instance = state.sceneResource.instances[instanceStart + i];
      instance.transform                = modelEntry.transform * instance.transform;
    }

    applyMaterialOverrides(materialRange, modelEntry.materialOverrides);
  }

  return environmentTextureIndex;
}

}  // namespace nvsamples
