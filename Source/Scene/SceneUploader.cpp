#include "SceneUploader.h"

// Role:
// Performs glTF parsing, texture/material packing, and import upload for runtime scenes.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
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

struct MaterialTextureIndices
{
  int baseColor          = -1;
  int metallicRoughness  = -1;
  int emissive           = -1;
  int normal             = -1;
  int specular           = -1;
  int specularColor      = -1;
  int transmission       = -1;
  int thickness          = -1;
  int clearcoat          = -1;
  int clearcoatRoughness = -1;
  int sheenColor         = -1;
  int sheenRoughness     = -1;
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

int ReadObjectTextureIndex(const tinygltf::Value::Object& object, const char* key, int fallback)
{
  const tinygltf::Value* value = FindObjectMember(object, key);
  if(value == nullptr || !value->IsObject())
  {
    return fallback;
  }

  const tinygltf::Value::Object& textureObject = value->Get<tinygltf::Value::Object>();
  return static_cast<int>(ReadObjectNumber(textureObject, "index", static_cast<float>(fallback)));
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
  dst.doubleSided = src.doubleSided;
  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_emissive_strength"))
  {
    const float emissiveStrength = std::max(ReadObjectNumber(*ext, "emissiveStrength", 1.0f), 0.0f);
    dst.emission *= emissiveStrength;
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
    dst.sheenColor     = ReadObjectVec3(*ext, "sheenColorFactor", dst.sheenColor);
  }
  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_volume"))
  {
    dst.volumeThickness     = ReadObjectNumber(*ext, "thicknessFactor", dst.volumeThickness);
    dst.attenuationDistance = ReadObjectNumber(*ext, "attenuationDistance", dst.attenuationDistance);
    dst.attenuationColor    = ReadObjectVec3(*ext, "attenuationColor", dst.attenuationColor);
  }

  dst.specular           = Clamp01(dst.specular);
  dst.specularTint       = Clamp01(dst.specularTint);
  dst.metallic           = Clamp01(dst.metallic);
  dst.roughness          = Clamp01(dst.roughness);
  dst.subsurface         = Clamp01(dst.subsurface);
  dst.anisotropy         = Clamp01(dst.anisotropy);
  dst.attenuationColor   = glm::clamp(dst.attenuationColor, glm::vec3(0.0f), glm::vec3(1.0f));
  dst.attenuationDistance = std::max(dst.attenuationDistance, 0.0f);
  dst.volumeThickness    = std::max(dst.volumeThickness, 0.0f);
  dst.sheenColor         = glm::clamp(dst.sheenColor, glm::vec3(0.0f), glm::vec3(1.0f));
  dst.sheenRoughness     = Clamp01(dst.sheenRoughness);
  dst.clearcoat          = Clamp01(dst.clearcoat);
  dst.clearcoatRoughness = Clamp01(dst.clearcoatRoughness);
  dst.transmission       = Clamp01(dst.transmission);
  return dst;
}

shaderio::GltfMetallicRoughness ToGpuMaterial(const MaterialAttributes& materialAttributes, int baseColorTextureIndex,
                                              int metallicRoughnessTextureIndex, const MaterialTextureIndices& textureIndices,
                                              float alphaCutoff, int alphaMode)
{
  shaderio::GltfMetallicRoughness dst{};
  dst.baseColorFactor               = glm::vec4(materialAttributes.albedo, 1.0f);
  dst.emissionFactor                = materialAttributes.emission;
  dst.doubleSided                   = materialAttributes.doubleSided ? 1 : 0;
  dst.metallicFactor                = materialAttributes.metallic;
  dst.roughnessFactor               = materialAttributes.roughness;
  dst.specularFactor                = materialAttributes.specular;
  dst.specularTint                  = materialAttributes.specularTint;
  dst.subsurfaceFactor              = materialAttributes.subsurface;
  dst.anisotropy                    = materialAttributes.anisotropy;
  dst.attenuationColor              = materialAttributes.attenuationColor;
  dst.transmissionFactor            = materialAttributes.transmission;
  dst.attenuationDistance           = materialAttributes.attenuationDistance;
  dst.volumeThickness               = materialAttributes.volumeThickness;
  dst.refractionIndex               = materialAttributes.refraction;
  dst.clearcoatFactor               = materialAttributes.clearcoat;
  dst.clearcoatRoughness            = materialAttributes.clearcoatRoughness;
  dst.sheenColorFactor              = materialAttributes.sheenColor;
  dst.sheenRoughnessFactor          = materialAttributes.sheenRoughness;
  dst.baseColorTextureIndex         = baseColorTextureIndex;
  dst.metallicRoughnessTextureIndex = metallicRoughnessTextureIndex;
  dst.emissiveTextureIndex          = textureIndices.emissive;
  dst.normalTextureIndex            = textureIndices.normal;
  dst.specularTextureIndex          = textureIndices.specular;
  dst.specularColorTextureIndex     = textureIndices.specularColor;
  dst.transmissionTextureIndex      = textureIndices.transmission;
  dst.thicknessTextureIndex         = textureIndices.thickness;
  dst.clearcoatTextureIndex         = textureIndices.clearcoat;
  dst.clearcoatRoughnessTextureIndex = textureIndices.clearcoatRoughness;
  dst.sheenColorTextureIndex        = textureIndices.sheenColor;
  dst.sheenRoughnessTextureIndex    = textureIndices.sheenRoughness;
  dst.alphaCutoff                   = alphaCutoff;
  dst.alphaMode                     = alphaMode;
  return dst;
}

float Luminance(const glm::vec3& color)
{
  return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

template <typename T>
T ReadStridedValue(const unsigned char* base, const shaderio::BufferView& view, uint32_t index, const T& fallback)
{
  if(view.count == 0 || index >= view.count || view.offset == std::numeric_limits<uint32_t>::max())
  {
    return fallback;
  }

  T value{};
  std::memcpy(&value, base + view.offset + static_cast<size_t>(index) * view.byteStride, sizeof(T));
  return value;
}

glm::uvec3 ReadTriangleIndices(const unsigned char* base, const shaderio::GltfMesh& mesh, uint32_t primitiveIndex)
{
  const size_t indexOffset = mesh.triMesh.indices.offset + static_cast<size_t>(primitiveIndex) * 3ull * mesh.triMesh.indices.byteStride;
  if(mesh.triMesh.indices.byteStride == sizeof(uint16_t))
  {
    glm::u16vec3 indices{};
    std::memcpy(&indices, base + indexOffset, sizeof(indices));
    return glm::uvec3(indices);
  }

  glm::uvec3 indices{};
  std::memcpy(&indices, base + indexOffset, sizeof(indices));
  return indices;
}

glm::vec3 TransformPosition(const glm::mat4& transform, const glm::vec3& position)
{
  return glm::vec3(transform * glm::vec4(position, 1.0f));
}

bool ModelNeedsFallbackMaterial(const tinygltf::Model& model)
{
  if(model.materials.empty())
  {
    return true;
  }

  for(const tinygltf::Mesh& mesh : model.meshes)
  {
    for(const tinygltf::Primitive& primitive : mesh.primitives)
    {
      if(primitive.mode != TINYGLTF_MODE_TRIANGLES || primitive.indices < 0)
      {
        continue;
      }

      if(primitive.material < 0 || primitive.material >= static_cast<int>(model.materials.size()))
      {
        return true;
      }
    }
  }

  return false;
}

void BuildEnvironmentSamplingData(const std::filesystem::path& hdriPath, GltfSceneResource& sceneResource)
{
  sceneResource.environmentCdf.clear();
  sceneResource.environmentPdf.clear();
  sceneResource.environmentWidth  = 0;
  sceneResource.environmentHeight = 0;

  const std::optional<ImageDataFloat4> image = LoadImageFloat4(hdriPath);
  if(!image.has_value() || image->width == 0 || image->height == 0)
  {
    return;
  }

  const uint32_t width  = image->width;
  const uint32_t height = image->height;
  const size_t   texelCount = static_cast<size_t>(width) * static_cast<size_t>(height);

  std::vector<double> weights(texelCount, 0.0);
  double              totalWeight = 0.0;

  for(uint32_t y = 0; y < height; ++y)
  {
    const float rowV     = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
    const float sinTheta = std::max(std::sin(glm::pi<float>() * rowV), 1.0e-6f);
    for(uint32_t x = 0; x < width; ++x)
    {
      const size_t index = static_cast<size_t>(y) * width + x;
      const glm::vec3 radiance(image->pixels[index * 4 + 0], image->pixels[index * 4 + 1], image->pixels[index * 4 + 2]);
      const double    weight = static_cast<double>(std::max(Luminance(radiance), 0.0f) * sinTheta);
      weights[index]         = weight;
      totalWeight += weight;
    }
  }

  if(totalWeight <= 0.0)
  {
    return;
  }

  sceneResource.environmentWidth  = width;
  sceneResource.environmentHeight = height;
  sceneResource.environmentCdf.resize(texelCount);
  sceneResource.environmentPdf.resize(texelCount);

  double cumulative = 0.0;
  for(size_t i = 0; i < texelCount; ++i)
  {
    const double probability            = weights[i] / totalWeight;
    cumulative                         += probability;
    sceneResource.environmentPdf[i]     = static_cast<float>(probability);
    sceneResource.environmentCdf[i]     = static_cast<float>(std::min(cumulative, 1.0));
  }
}

void BuildEmissiveTriangleSamplingData(GltfSceneResource& sceneResource)
{
  sceneResource.emissiveTriangleCdf.clear();
  if(sceneResource.emissiveTriangles.empty())
  {
    return;
  }

  std::vector<double> weights(sceneResource.emissiveTriangles.size(), 0.0);
  double              totalWeight = 0.0;
  for(size_t i = 0; i < sceneResource.emissiveTriangles.size(); ++i)
  {
    const shaderio::EmissiveTriangleLight& light = sceneResource.emissiveTriangles[i];
    if(light.materialIndex >= sceneResource.materials.size())
    {
      continue;
    }

    const shaderio::GltfMetallicRoughness& material = sceneResource.materials[light.materialIndex];
    const double weight = static_cast<double>(light.area) * static_cast<double>(std::max(Luminance(glm::vec3(material.emissionFactor)), 0.0f));
    weights[i]          = weight;
    totalWeight += weight;
  }

  if(totalWeight <= 0.0)
  {
    sceneResource.emissiveTriangles.clear();
    return;
  }

  sceneResource.emissiveTriangleCdf.resize(sceneResource.emissiveTriangles.size());
  double cumulative = 0.0;
  for(size_t i = 0; i < sceneResource.emissiveTriangles.size(); ++i)
  {
    cumulative += weights[i] / totalWeight;
    sceneResource.emissiveTriangleCdf[i] = static_cast<float>(std::min(cumulative, 1.0));
  }
}

void AppendEmissiveTrianglesFromModel(const tinygltf::Model& model,
                                      const GltfSceneResource& sceneResource,
                                      uint32_t instanceStart,
                                      uint32_t instanceCount,
                                      std::vector<shaderio::EmissiveTriangleLight>& outTriangles)
{
  if(model.buffers.empty())
  {
    return;
  }

  const unsigned char* base = model.buffers[0].data.data();
  for(uint32_t instanceOffset = 0; instanceOffset < instanceCount; ++instanceOffset)
  {
    const uint32_t sceneInstanceIndex = instanceStart + instanceOffset;
    const shaderio::GltfInstance& instance = sceneResource.instances[sceneInstanceIndex];
    if(instance.materialIndex >= sceneResource.materials.size() || instance.meshIndex >= sceneResource.meshes.size())
    {
      continue;
    }

    const shaderio::GltfMetallicRoughness& material = sceneResource.materials[instance.materialIndex];
    if(Luminance(glm::vec3(material.emissionFactor)) <= 0.0f)
    {
      continue;
    }

    const shaderio::GltfMesh& mesh = sceneResource.meshes[instance.meshIndex];
    const uint32_t primitiveCount  = mesh.triMesh.indices.count / 3u;
    for(uint32_t primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
    {
      const glm::uvec3 indices = ReadTriangleIndices(base, mesh, primitiveIndex);

      const glm::vec3 position0 = ReadStridedValue(base, mesh.triMesh.positions, indices.x, glm::vec3(0.0f));
      const glm::vec3 position1 = ReadStridedValue(base, mesh.triMesh.positions, indices.y, glm::vec3(0.0f));
      const glm::vec3 position2 = ReadStridedValue(base, mesh.triMesh.positions, indices.z, glm::vec3(0.0f));
      const glm::vec2 texCoord0 = ReadStridedValue(base, mesh.triMesh.texCoords, indices.x, glm::vec2(0.0f));
      const glm::vec2 texCoord1 = ReadStridedValue(base, mesh.triMesh.texCoords, indices.y, glm::vec2(0.0f));
      const glm::vec2 texCoord2 = ReadStridedValue(base, mesh.triMesh.texCoords, indices.z, glm::vec2(0.0f));

      const glm::vec3 worldPosition0 = TransformPosition(instance.transform, position0);
      const glm::vec3 worldPosition1 = TransformPosition(instance.transform, position1);
      const glm::vec3 worldPosition2 = TransformPosition(instance.transform, position2);
      const glm::vec3 crossProduct   = glm::cross(worldPosition1 - worldPosition0, worldPosition2 - worldPosition0);
      const float     area           = 0.5f * glm::length(crossProduct);
      if(area <= 1.0e-6f)
      {
        continue;
      }

      shaderio::EmissiveTriangleLight light{};
      light.position0       = worldPosition0;
      light.position1       = worldPosition1;
      light.position2       = worldPosition2;
      light.area            = area;
      light.materialIndex   = instance.materialIndex;
      light.instanceIndex   = sceneInstanceIndex;
      light.primitiveIndex  = primitiveIndex;
      light.geometricNormal = glm::normalize(crossProduct);
      light.texCoord0       = texCoord0;
      light.texCoord1       = texCoord1;
      light.texCoord2       = texCoord2;
      outTriangles.push_back(light);
    }
  }
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
  auto appendRgbaTexture = [&](const std::span<const unsigned char> rgbaPixels, uint32_t width, uint32_t height, bool sRgb) -> int {
    if(width == 0 || height == 0 || rgbaPixels.empty())
    {
      return -1;
    }

    VkImageCreateInfo imageInfo = DEFAULT_VkImageCreateInfo;
    imageInfo.format            = sRgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.usage             = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.extent            = {width, height, 1};

    nvvk::Image texture;
    NVVK_CHECK(m_Allocator->createImage(texture, imageInfo, DEFAULT_VkImageViewCreateInfo));
    NVVK_CHECK(m_StagingUploader->appendImage(texture, rgbaPixels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    m_SamplerPool->acquireSampler(texture.descriptor.sampler);
    state.textures.emplace_back(texture);
    return static_cast<int>(state.textures.size()) - 1;
  };

  auto loadImageTexture = [&](const tinygltf::Image& image, const std::filesystem::path& modelPath, bool sRgb) -> int {
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

      return appendRgbaTexture(rgba, static_cast<uint32_t>(image.width), static_cast<uint32_t>(image.height), sRgb);
    }

    if(!image.uri.empty())
    {
      std::error_code ec;
      const std::filesystem::path uriPath     = std::filesystem::path(image.uri);
      const std::filesystem::path texturePath = std::filesystem::weakly_canonical(modelPath.parent_path() / uriPath, ec);
      if(!ec && std::filesystem::exists(texturePath, ec))
      {
        nvvk::Image texture = nvsamples::LoadAndCreateImage(cmd, *m_StagingUploader, m_App->getDevice(), texturePath, sRgb);
        m_SamplerPool->acquireSampler(texture.descriptor.sampler);
        state.textures.emplace_back(texture);
        return static_cast<int>(state.textures.size()) - 1;
      }
    }

    return -1;
  };

  auto loadTextureIndices = [&](const tinygltf::Model& model, const std::filesystem::path& modelPath, bool sRgb) -> std::vector<int> {
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
        sourceMap[sourceIndex] = loadImageTexture(model.images[sourceIndex], modelPath, sRgb);
      }
      textureMap[i] = sourceMap[sourceIndex];
    }
    return textureMap;
  };

  auto getTextureIndex = [](const std::vector<int>& textureMap, int gltfTextureIndex) -> int {
    if(gltfTextureIndex < 0 || gltfTextureIndex >= static_cast<int>(textureMap.size()))
    {
      return -1;
    }

    return textureMap[gltfTextureIndex];
  };

  auto addModelMaterials = [&](const tinygltf::Model& model, const std::vector<int>& srgbTextureMap,
                               const std::vector<int>& linearTextureMap) -> MaterialRange {
    const uint32_t materialOffset   = static_cast<uint32_t>(state.sceneResource.materials.size());
    const uint32_t attributesOffset = static_cast<uint32_t>(state.materialAttributes.size());

    if(model.materials.empty())
    {
      MaterialAttributes defaultMaterial{};
      state.materialAttributes.push_back(defaultMaterial);
      state.sceneResource.materials.push_back(ToGpuMaterial(defaultMaterial, -1, -1, MaterialTextureIndices{}, 0.5f,
                                                            shaderio::GltfAlphaMode::eOpaque));
      return {.offset = materialOffset, .count = 1};
    }

    for(const tinygltf::Material& src : model.materials)
    {
      const MaterialAttributes parsedMaterial = ParseMaterialAttributes(src);
      state.materialAttributes.push_back(parsedMaterial);

      MaterialTextureIndices textureIndices{};
      textureIndices.baseColor         = getTextureIndex(srgbTextureMap, src.pbrMetallicRoughness.baseColorTexture.index);
      textureIndices.metallicRoughness = getTextureIndex(linearTextureMap, src.pbrMetallicRoughness.metallicRoughnessTexture.index);
      textureIndices.emissive          = getTextureIndex(srgbTextureMap, src.emissiveTexture.index);
      textureIndices.normal            = getTextureIndex(linearTextureMap, src.normalTexture.index);

      if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_transmission"))
      {
        textureIndices.transmission = getTextureIndex(linearTextureMap, ReadObjectTextureIndex(*ext, "transmissionTexture", -1));
      }
      if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_volume"))
      {
        textureIndices.thickness = getTextureIndex(linearTextureMap, ReadObjectTextureIndex(*ext, "thicknessTexture", -1));
      }
      if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_clearcoat"))
      {
        textureIndices.clearcoat =
            getTextureIndex(linearTextureMap, ReadObjectTextureIndex(*ext, "clearcoatTexture", -1));
        textureIndices.clearcoatRoughness =
            getTextureIndex(linearTextureMap, ReadObjectTextureIndex(*ext, "clearcoatRoughnessTexture", -1));
      }
      if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_specular"))
      {
        textureIndices.specular =
            getTextureIndex(linearTextureMap, ReadObjectTextureIndex(*ext, "specularTexture", -1));
        textureIndices.specularColor =
            getTextureIndex(srgbTextureMap, ReadObjectTextureIndex(*ext, "specularColorTexture", -1));
      }
      if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_sheen"))
      {
        textureIndices.sheenRoughness =
            getTextureIndex(linearTextureMap, ReadObjectTextureIndex(*ext, "sheenRoughnessTexture", -1));
        textureIndices.sheenColor =
            getTextureIndex(srgbTextureMap, ReadObjectTextureIndex(*ext, "sheenColorTexture", -1));
      }

      int alphaMode = shaderio::GltfAlphaMode::eOpaque;
      if(src.alphaMode == "MASK")
        alphaMode = shaderio::GltfAlphaMode::eMask;
      else if(src.alphaMode == "BLEND")
        alphaMode = shaderio::GltfAlphaMode::eBlend;

      state.sceneResource.materials.push_back(ToGpuMaterial(parsedMaterial, textureIndices.baseColor,
                                                            textureIndices.metallicRoughness, textureIndices,
                                                            static_cast<float>(src.alphaCutoff), alphaMode));
    }
    return {.offset = materialOffset, .count = static_cast<uint32_t>(state.materialAttributes.size() - attributesOffset)};
  };

  auto addDefaultMaterial = [&]() -> uint32_t {
    const uint32_t materialIndex = static_cast<uint32_t>(state.sceneResource.materials.size());
    MaterialAttributes defaultMaterial{};
    state.materialAttributes.push_back(defaultMaterial);
    state.sceneResource.materials.push_back(ToGpuMaterial(defaultMaterial, -1, -1, MaterialTextureIndices{}, 0.5f,
                                                          shaderio::GltfAlphaMode::eOpaque));
    return materialIndex;
  };

  auto applyMaterialOverrides = [&](const MaterialRange& range, const std::vector<SceneMaterialOverride>& overrides,
                                    std::optional<uint32_t> fallbackMaterialIndex = std::nullopt) {
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
        gpuMaterial.emissionFactor                     = overrideDef.attributes.emission;
        gpuMaterial.doubleSided                        = overrideDef.attributes.doubleSided ? 1 : 0;
        gpuMaterial.metallicFactor                     = overrideDef.attributes.metallic;
        gpuMaterial.roughnessFactor                    = overrideDef.attributes.roughness;
        gpuMaterial.specularFactor                     = overrideDef.attributes.specular;
        gpuMaterial.specularTint                       = overrideDef.attributes.specularTint;
        gpuMaterial.subsurfaceFactor                   = overrideDef.attributes.subsurface;
        gpuMaterial.anisotropy                         = overrideDef.attributes.anisotropy;
        gpuMaterial.attenuationColor                   = overrideDef.attributes.attenuationColor;
        gpuMaterial.transmissionFactor                 = overrideDef.attributes.transmission;
        gpuMaterial.attenuationDistance                = overrideDef.attributes.attenuationDistance;
        gpuMaterial.volumeThickness                    = overrideDef.attributes.volumeThickness;
        gpuMaterial.refractionIndex                    = overrideDef.attributes.refraction;
        gpuMaterial.clearcoatFactor                    = overrideDef.attributes.clearcoat;
        gpuMaterial.clearcoatRoughness                 = overrideDef.attributes.clearcoatRoughness;
        gpuMaterial.sheenColorFactor                   = overrideDef.attributes.sheenColor;
        gpuMaterial.sheenRoughnessFactor               = overrideDef.attributes.sheenRoughness;
      }

      if(overrideDef.materialSlot < 0 && fallbackMaterialIndex.has_value())
      {
        const uint32_t fallbackIndex = *fallbackMaterialIndex;
        state.materialAttributes[fallbackIndex] = overrideDef.attributes;
        shaderio::GltfMetallicRoughness& gpuMaterial = state.sceneResource.materials[fallbackIndex];
        gpuMaterial.baseColorFactor                    = glm::vec4(overrideDef.attributes.albedo, gpuMaterial.baseColorFactor.w);
        gpuMaterial.emissionFactor                     = overrideDef.attributes.emission;
        gpuMaterial.doubleSided                        = overrideDef.attributes.doubleSided ? 1 : 0;
        gpuMaterial.metallicFactor                     = overrideDef.attributes.metallic;
        gpuMaterial.roughnessFactor                    = overrideDef.attributes.roughness;
        gpuMaterial.specularFactor                     = overrideDef.attributes.specular;
        gpuMaterial.specularTint                       = overrideDef.attributes.specularTint;
        gpuMaterial.subsurfaceFactor                   = overrideDef.attributes.subsurface;
        gpuMaterial.anisotropy                         = overrideDef.attributes.anisotropy;
        gpuMaterial.attenuationColor                   = overrideDef.attributes.attenuationColor;
        gpuMaterial.transmissionFactor                 = overrideDef.attributes.transmission;
        gpuMaterial.attenuationDistance                = overrideDef.attributes.attenuationDistance;
        gpuMaterial.volumeThickness                    = overrideDef.attributes.volumeThickness;
        gpuMaterial.refractionIndex                    = overrideDef.attributes.refraction;
        gpuMaterial.clearcoatFactor                    = overrideDef.attributes.clearcoat;
        gpuMaterial.clearcoatRoughness                 = overrideDef.attributes.clearcoatRoughness;
        gpuMaterial.sheenColorFactor                   = overrideDef.attributes.sheenColor;
        gpuMaterial.sheenRoughnessFactor               = overrideDef.attributes.sheenRoughness;
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
    BuildEnvironmentSamplingData(hdriPath, state.sceneResource);
  }

  const std::vector<std::filesystem::path> contentDirs = nvsamples::GetContentDirs();
  for(const SceneModelEntry& modelEntry : input.sceneDefinition.models)
  {
    const std::filesystem::path modelPath = nvutils::findFile(modelEntry.assetPath, contentDirs);
    const tinygltf::Model       model     = nvsamples::LoadGltfResources(modelPath);

    const std::vector<int> srgbTextureMap   = loadTextureIndices(model, modelPath, true);
    const std::vector<int> linearTextureMap = loadTextureIndices(model, modelPath, false);
    const MaterialRange    materialRange    = addModelMaterials(model, srgbTextureMap, linearTextureMap);
    const bool             needsFallbackMaterial = ModelNeedsFallbackMaterial(model);
    std::optional<uint32_t> fallbackMaterialIndex;
    if(model.materials.empty())
    {
      fallbackMaterialIndex = materialRange.offset;
    }
    else if(needsFallbackMaterial)
    {
      fallbackMaterialIndex = addDefaultMaterial();
    }

    const uint32_t         meshStartIndex = static_cast<uint32_t>(state.sceneResource.meshes.size());
    const uint32_t         instanceStart  = static_cast<uint32_t>(state.sceneResource.instances.size());

    nvsamples::ImportGltfData(state.sceneResource, model, *m_StagingUploader, modelEntry.importNodeInstances, materialRange.offset,
                              fallbackMaterialIndex.value_or(materialRange.offset));

    uint32_t importedInstanceCount = static_cast<uint32_t>(state.sceneResource.instances.size()) - instanceStart;
    if(importedInstanceCount == 0)
    {
      const uint32_t meshCount = static_cast<uint32_t>(state.sceneResource.meshes.size()) - meshStartIndex;
      for(uint32_t meshLocal = 0; meshLocal < meshCount; ++meshLocal)
      {
        const uint32_t meshIndex = meshStartIndex + meshLocal;
        uint32_t       materialIndex = fallbackMaterialIndex.value_or(materialRange.offset);
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

    applyMaterialOverrides(materialRange, modelEntry.materialOverrides, fallbackMaterialIndex);
    AppendEmissiveTrianglesFromModel(model, state.sceneResource, instanceStart, importedInstanceCount, state.sceneResource.emissiveTriangles);
  }

  BuildEmissiveTriangleSamplingData(state.sceneResource);
  return environmentTextureIndex;
}

}  // namespace nvsamples

