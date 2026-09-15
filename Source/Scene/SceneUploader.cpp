#include "SceneUploader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include <fmt/format.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <tiny_gltf.h>

#include "Assets/ImageData.h"
#include "Framework/Platform/Log.h"
#include "Framework/Vulkan/Diagnostics.h"
#include "LightSampling.h"

namespace rtpt
{

namespace
{

// MaterialRange
// The contiguous block of scene materials one model contributed, so per-model material slots can be mapped to scene material indices.

struct MaterialRange
{
  // Scene material index of the model's first material.
  uint32_t offset = 0;

  // Number of materials the model contributed, including the default one a material-less model receives.
  uint32_t count  = 0;
};

// MaterialTextureIndices
// One index per texture slot of a material, or -1 when the material has no such texture.
// ReadGltfTextureIndices fills it with glTF texture indices, and addModelMaterials resolves those to positions in the uploaded texture list before the GPU material is packed.

struct MaterialTextureIndices
{
  // pbrMetallicRoughness.baseColorTexture, sampled as sRGB.
  int baseColor          = -1;

  // pbrMetallicRoughness.metallicRoughnessTexture, sampled as linear data.
  int metallicRoughness  = -1;

  // emissiveTexture, sampled as sRGB.
  int emissive           = -1;

  // normalTexture, sampled as linear data.
  int normal             = -1;

  // KHR_materials_specular specularTexture, sampled as linear data.
  int specular           = -1;

  // KHR_materials_specular specularColorTexture, sampled as sRGB.
  int specularColor      = -1;

  // KHR_materials_transmission transmissionTexture, sampled as linear data.
  int transmission       = -1;

  // KHR_materials_volume thicknessTexture, sampled as linear data.
  int thickness          = -1;

  // KHR_materials_clearcoat clearcoatTexture, sampled as linear data.
  int clearcoat          = -1;

  // KHR_materials_clearcoat clearcoatRoughnessTexture, sampled as linear data.
  int clearcoatRoughness = -1;

  // KHR_materials_sheen sheenColorTexture, sampled as sRGB.
  int sheenColor         = -1;

  // KHR_materials_sheen sheenRoughnessTexture, sampled as linear data.
  int sheenRoughness     = -1;
};

// MaterialTextureSlot
// One texture slot of MaterialTextureIndices paired with the color space its texels are stored in.
// kMaterialTextureSlots is the only place that decides which slots hold colors and which hold data, so the formats an image is uploaded in and the texture map a material reads cannot disagree.

struct MaterialTextureSlot
{
  // The slot's field in MaterialTextureIndices.
  int MaterialTextureIndices::*field = nullptr;

  // Whether the slot holds color, sampled through an sRGB format. Data slots such as roughness, normals, or thickness are sampled as UNORM.
  bool sRgb = false;
};

// Every material texture slot the importer reads, with the color space documented on its MaterialTextureIndices field.
constexpr std::array<MaterialTextureSlot, 12> kMaterialTextureSlots { {
    { .field = &MaterialTextureIndices::baseColor,          .sRgb = true  },
    { .field = &MaterialTextureIndices::metallicRoughness,  .sRgb = false },
    { .field = &MaterialTextureIndices::emissive,           .sRgb = true  },
    { .field = &MaterialTextureIndices::normal,             .sRgb = false },
    { .field = &MaterialTextureIndices::specular,           .sRgb = false },
    { .field = &MaterialTextureIndices::specularColor,      .sRgb = true  },
    { .field = &MaterialTextureIndices::transmission,       .sRgb = false },
    { .field = &MaterialTextureIndices::thickness,          .sRgb = false },
    { .field = &MaterialTextureIndices::clearcoat,          .sRgb = false },
    { .field = &MaterialTextureIndices::clearcoatRoughness, .sRgb = false },
    { .field = &MaterialTextureIndices::sheenColor,         .sRgb = true  },
    { .field = &MaterialTextureIndices::sheenRoughness,     .sRgb = false },
} };

// ImageColorSpaces
// How the material slots of one model sample a glTF image. A glTF image carries no color space of its own, so the slots that reference it decide which uploads it needs, and an image no slot references is not uploaded at all.

struct ImageColorSpaces
{
  // A color slot samples the image, so it needs an R8G8B8A8_SRGB upload.
  bool sRgb = false;

  // A data slot samples the image, so it needs an R8G8B8A8_UNORM upload.
  bool linear = false;

  // The emissive slot samples the image, so its mean luminance is measured for emissive light selection. Emissive is a color slot, so this implies sRgb.
  bool emissive = false;
};

// ModelTextureMaps
// Scene texture indices for the glTF textures of one model, one map per color space, because one glTF texture can be uploaded both as color and as data.

struct ModelTextureMaps
{
  // glTF texture index to the scene index of its sRGB upload, or -1 when no color slot samples it.
  std::vector<int> srgb;

  // glTF texture index to the scene index of its UNORM upload, or -1 when no data slot samples it.
  std::vector<int> linear;
};

float Clamp01(float value)
{
  return std::clamp(value, 0.0f, 1.0f);
}

// Extension JSON
// tinygltf keeps material extensions as generic JSON values rather than typed fields, so these helpers read them defensively.
// Every reader takes a fallback that is returned when the key is missing or holds the wrong type, which keeps malformed assets importable.

// JSON numbers can arrive as either reals or integers.
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

// Reads the first three elements of a JSON array; shorter arrays fall back entirely rather than mixing in fallback components.
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

// Extension textures are glTF textureInfo objects; only their "index" member is read, so texCoord sets and transforms are ignored.
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

// Returns the named extension's JSON object, or nullptr when the material does not use that extension.
const tinygltf::Value::Object* FindMaterialExtensionObject(const tinygltf::Material& material, const char* extensionName)
{
  const auto extIt = material.extensions.find(extensionName);

  if(extIt == material.extensions.end() || !extIt->second.IsObject())
  {
    return nullptr;
  }

  return &extIt->second.Get<tinygltf::Value::Object>();
}

// Texture slots
// A material's texture references are read twice, once to decide which uploads each image needs and once to resolve the material's scene texture indices, so both go through these helpers.

// Reads the glTF texture index of every slot in kMaterialTextureSlots, core and extension slots alike, with -1 for slots the material leaves empty.
// The result holds glTF texture indices, not scene texture indices.
MaterialTextureIndices ReadGltfTextureIndices(const tinygltf::Material& src)
{
  MaterialTextureIndices indices {};

  indices.baseColor         = src.pbrMetallicRoughness.baseColorTexture.index;
  indices.metallicRoughness = src.pbrMetallicRoughness.metallicRoughnessTexture.index;
  indices.emissive          = src.emissiveTexture.index;
  indices.normal            = src.normalTexture.index;

  // Extension slots
  // A material without the extension, or an extension object without the texture key, keeps the -1 default.

  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_transmission"))
  {
    indices.transmission = ReadObjectTextureIndex(*ext, "transmissionTexture", -1);
  }

  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_volume"))
  {
    indices.thickness = ReadObjectTextureIndex(*ext, "thicknessTexture", -1);
  }

  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_clearcoat"))
  {
    indices.clearcoat          = ReadObjectTextureIndex(*ext, "clearcoatTexture", -1);
    indices.clearcoatRoughness = ReadObjectTextureIndex(*ext, "clearcoatRoughnessTexture", -1);
  }

  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_specular"))
  {
    indices.specular      = ReadObjectTextureIndex(*ext, "specularTexture", -1);
    indices.specularColor = ReadObjectTextureIndex(*ext, "specularColorTexture", -1);
  }

  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_sheen"))
  {
    indices.sheenRoughness = ReadObjectTextureIndex(*ext, "sheenRoughnessTexture", -1);
    indices.sheenColor     = ReadObjectTextureIndex(*ext, "sheenColorTexture", -1);
  }

  return indices;
}

// Records, for every image of the model, the color spaces that the material slots referencing it sample it in.
// Out-of-range texture and image references are ignored, as they are when materials resolve them.
std::vector<ImageColorSpaces> CollectImageColorSpaces(const tinygltf::Model& model)
{
  std::vector<ImageColorSpaces> imageColorSpaces(model.images.size());

  for(const tinygltf::Material& material : model.materials)
  {
    const MaterialTextureIndices gltfTextureIndices = ReadGltfTextureIndices(material);

    for(const MaterialTextureSlot& slot : kMaterialTextureSlots)
    {
      const int textureIndex = gltfTextureIndices.*slot.field;

      if(textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
      {
        continue;
      }

      const int imageIndex = model.textures[textureIndex].source;

      if(imageIndex < 0 || imageIndex >= static_cast<int>(model.images.size()))
      {
        continue;
      }

      ImageColorSpaces& colorSpaces = imageColorSpaces[imageIndex];

      colorSpaces.sRgb     = colorSpaces.sRgb || slot.sRgb;
      colorSpaces.linear   = colorSpaces.linear || !slot.sRgb;
      colorSpaces.emissive = colorSpaces.emissive || slot.field == &MaterialTextureIndices::emissive;
    }
  }

  return imageColorSpaces;
}

// Translates one glTF material, core factors plus the supported KHR_materials extensions, into the renderer's CPU material attributes.
// Textures are not handled here; ReadGltfTextureIndices reads them separately.
MaterialAttributes ParseMaterialAttributes(const tinygltf::Material& src)
{
  // Core factors
  // baseColorFactor splits into the RGB albedo and the alpha factor, which alpha masking multiplies with the base color texture's alpha.

  MaterialAttributes dst {};

  if(src.pbrMetallicRoughness.baseColorFactor.size() == 4)
  {
    dst.albedo = glm::vec3(static_cast<float>(src.pbrMetallicRoughness.baseColorFactor[0]), static_cast<float>(src.pbrMetallicRoughness.baseColorFactor[1]), static_cast<float>(src.pbrMetallicRoughness.baseColorFactor[2]));
    dst.alpha  = static_cast<float>(src.pbrMetallicRoughness.baseColorFactor[3]);
  }

  if(src.emissiveFactor.size() == 3)
  {
    dst.emission = glm::vec3(static_cast<float>(src.emissiveFactor[0]), static_cast<float>(src.emissiveFactor[1]), static_cast<float>(src.emissiveFactor[2]));
  }

  dst.doubleSided = src.doubleSided;

  // Emissive strength scales the emission factor so values above 1 survive; a negative strength is treated as 0.
  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_emissive_strength"))
  {
    const float emissiveStrength = std::max(ReadObjectNumber(*ext, "emissiveStrength", 1.0f), 0.0f);

    dst.emission *= emissiveStrength;
  }

  dst.metallic  = static_cast<float>(src.pbrMetallicRoughness.metallicFactor);
  dst.roughness = static_cast<float>(src.pbrMetallicRoughness.roughnessFactor);

  // Extensions
  // Each extension overrides only the attributes it defines. A missing key keeps the current value, which is the MaterialAttributes default.

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

  // The material stores a scalar specular tint, so the RGB specularColorFactor is collapsed to its mean.
  if(const tinygltf::Value::Object* ext = FindMaterialExtensionObject(src, "KHR_materials_specular"))
  {
    dst.specular = ReadObjectNumber(*ext, "specularFactor", dst.specular);

    const glm::vec3 specColor = ReadObjectVec3(*ext, "specularColorFactor", glm::vec3(1.0f));

    dst.specularTint = (specColor.x + specColor.y + specColor.z) / 3.0f;
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

  // Range clamping
  // Asset values are untrusted, so every attribute is forced into the range MaterialAttributes documents before it reaches the shaders.
  // Emission and the index of refraction are left unclamped.

  dst.alpha               = Clamp01(dst.alpha);
  dst.specular            = Clamp01(dst.specular);
  dst.specularTint        = Clamp01(dst.specularTint);
  dst.metallic            = Clamp01(dst.metallic);
  dst.roughness           = Clamp01(dst.roughness);
  dst.subsurface          = Clamp01(dst.subsurface);
  dst.anisotropy          = Clamp01(dst.anisotropy);

  dst.attenuationColor    = glm::clamp(dst.attenuationColor, glm::vec3(0.0f), glm::vec3(1.0f));
  dst.attenuationDistance = std::max(dst.attenuationDistance, 0.0f);
  dst.volumeThickness     = std::max(dst.volumeThickness, 0.0f);

  dst.sheenColor          = glm::clamp(dst.sheenColor, glm::vec3(0.0f), glm::vec3(1.0f));
  dst.sheenRoughness      = Clamp01(dst.sheenRoughness);
  dst.clearcoat           = Clamp01(dst.clearcoat);
  dst.clearcoatRoughness  = Clamp01(dst.clearcoatRoughness);
  dst.transmission        = Clamp01(dst.transmission);

  return dst;
}

// Packs CPU material attributes and resolved texture indices into the layout shaders read from the material buffer.
shaderio::GltfMetallicRoughness ToGpuMaterial(const MaterialAttributes& materialAttributes, int baseColorTextureIndex, int metallicRoughnessTextureIndex, const MaterialTextureIndices& textureIndices, float alphaCutoff, int alphaMode)
{
  // Factors
  // Copied one to one from the CPU attributes, which ParseMaterialAttributes or an override has already range-checked.

  shaderio::GltfMetallicRoughness dst {};

  dst.baseColorFactor                = glm::vec4(materialAttributes.albedo, materialAttributes.alpha);
  dst.emissionFactor                 = materialAttributes.emission;
  dst.doubleSided                    = materialAttributes.doubleSided ? 1 : 0;
  dst.metallicFactor                 = materialAttributes.metallic;
  dst.roughnessFactor                = materialAttributes.roughness;
  dst.specularFactor                 = materialAttributes.specular;
  dst.specularTint                   = materialAttributes.specularTint;
  dst.subsurfaceFactor               = materialAttributes.subsurface;
  dst.anisotropy                     = materialAttributes.anisotropy;

  dst.attenuationColor               = materialAttributes.attenuationColor;
  dst.transmissionFactor             = materialAttributes.transmission;
  dst.attenuationDistance            = materialAttributes.attenuationDistance;
  dst.volumeThickness                = materialAttributes.volumeThickness;
  dst.refractionIndex                = materialAttributes.refraction;
  dst.clearcoatFactor                = materialAttributes.clearcoat;
  dst.clearcoatRoughness             = materialAttributes.clearcoatRoughness;
  dst.sheenColorFactor               = materialAttributes.sheenColor;
  dst.sheenRoughnessFactor           = materialAttributes.sheenRoughness;

  // Texture indices
  // Positions in the scene texture list; -1 tells the shaders to use the factor alone.

  dst.baseColorTextureIndex          = baseColorTextureIndex;
  dst.metallicRoughnessTextureIndex  = metallicRoughnessTextureIndex;
  dst.emissiveTextureIndex           = textureIndices.emissive;
  dst.normalTextureIndex             = textureIndices.normal;
  dst.specularTextureIndex           = textureIndices.specular;
  dst.specularColorTextureIndex      = textureIndices.specularColor;

  dst.transmissionTextureIndex       = textureIndices.transmission;
  dst.thicknessTextureIndex          = textureIndices.thickness;
  dst.clearcoatTextureIndex          = textureIndices.clearcoat;
  dst.clearcoatRoughnessTextureIndex = textureIndices.clearcoatRoughness;
  dst.sheenColorTextureIndex         = textureIndices.sheenColor;
  dst.sheenRoughnessTextureIndex     = textureIndices.sheenRoughness;

  // Alpha
  // Shaders discard masked hits whose base color alpha falls below the cutoff; the cutoff is ignored in other modes.

  dst.alphaCutoff = alphaCutoff;
  dst.alphaMode   = alphaMode;

  return dst;
}

// Geometry readers
// Emissive triangle extraction reads mesh data back from the CPU copy of the glTF buffer, using the same offsets and strides the GPU sees.

// Reads one element of a vertex stream. An absent stream (zero count or the 0xFFFFFFFF offset marker) or an out-of-range index yields the fallback.
template <typename T>
T ReadStridedValue(const unsigned char* base, const shaderio::BufferView& view, uint32_t index, const T& fallback)
{
  if(view.count == 0 || index >= view.count || view.offset == std::numeric_limits<uint32_t>::max())
  {
    return fallback;
  }

  // memcpy rather than a cast, because the buffer bytes are not guaranteed to be aligned for T.
  T value {};

  std::memcpy(&value, base + view.offset + static_cast<size_t>(index) * view.byteStride, sizeof(T));

  return value;
}

// Reads the three vertex indices of one triangle. The index stride doubles as the index width, so a 2-byte stride means 16-bit indices.
glm::uvec3 ReadTriangleIndices(const unsigned char* base, const shaderio::GltfMesh& mesh, uint32_t primitiveIndex)
{
  const size_t indexOffset = mesh.triMesh.indices.offset + static_cast<size_t>(primitiveIndex) * 3ull * mesh.triMesh.indices.byteStride;

  if(mesh.triMesh.indices.byteStride == sizeof(uint16_t))
  {
    glm::u16vec3 indices {};

    std::memcpy(&indices, base + indexOffset, sizeof(indices));

    return glm::uvec3(indices);
  }

  glm::uvec3 indices {};

  std::memcpy(&indices, base + indexOffset, sizeof(indices));

  return indices;
}

glm::vec3 TransformPosition(const glm::mat4& transform, const glm::vec3& position)
{
  return glm::vec3(transform * glm::vec4(position, 1.0f));
}

// Whether any importable primitive (an indexed triangle list) lacks a valid material, so the model needs a default material to point it at.
// A model without any materials always needs one.
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

// Decodes a glTF image to RGBA8 on the CPU, so it can be uploaded in every color space it needs from one decode.
// An image tinygltf already decoded is expanded from its channel count. When tinygltf left the pixels empty, the image's URI is loaded relative to the model file instead.
// Images that fail both ways, or use unsupported formats, return nothing, so materials simply go without that texture.
std::optional<rtpt::ImageRgba8> DecodeGltfImage(const tinygltf::Image& image, const std::filesystem::path& modelPath)
{
  if(image.width > 0 && image.height > 0 && !image.image.empty())
  {
    // Only 8-bit images with one to four channels fit the RGBA8 upload path. The channel count is checked as given, so an unsupported count warns rather than being clamped into range.
    if(image.bits != 8 || image.component < 1 || image.component > 4)
    {
      rtpt::Log(rtpt::LogLevel::Warning, fmt::format("Skipping unsupported glTF image format ({} bits, {} channels)", image.bits, image.component));
      return std::nullopt;
    }

    const size_t sourceChannels = static_cast<size_t>(image.component);
    const size_t pixelCount     = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    const size_t requiredBytes  = pixelCount * sourceChannels;

    // A short payload would make the expansion below read past the end of the image data.
    if(image.image.size() < requiredBytes)
    {
      rtpt::Log(rtpt::LogLevel::Warning, fmt::format("Skipping invalid glTF image payload (expected at least {} bytes, got {})", requiredBytes, image.image.size()));
      return std::nullopt;
    }

    // Channel expansion
    // One channel is luminance and two are luminance plus alpha, so both copy the luminance into R, G, and B; leaving G and B at 255 would tint the texture. Three and four channels copy straight across.
    // Alpha is the last source channel of two- and four-channel images. The others keep the initial 255, so they end up opaque.

    rtpt::ImageRgba8 decoded;

    decoded.width  = static_cast<uint32_t>(image.width);
    decoded.height = static_cast<uint32_t>(image.height);

    decoded.pixels.assign(pixelCount * 4, 255);

    for(size_t i = 0; i < pixelCount; ++i)
    {
      const unsigned char* source      = image.image.data() + i * sourceChannels;
      uint8_t*             destination = decoded.pixels.data() + i * 4;

      if(sourceChannels <= 2)
      {
        destination[0] = source[0];
        destination[1] = source[0];
        destination[2] = source[0];
      }
      else
      {
        destination[0] = source[0];
        destination[1] = source[1];
        destination[2] = source[2];
      }

      if(sourceChannels == 2 || sourceChannels == 4)
      {
        destination[3] = source[sourceChannels - 1];
      }
    }

    return decoded;
  }

  if(!image.uri.empty())
  {
    std::error_code ec;

    const std::filesystem::path uriPath     = std::filesystem::path(image.uri);
    const std::filesystem::path texturePath = std::filesystem::weakly_canonical(modelPath.parent_path() / uriPath, ec);

    if(!ec && std::filesystem::exists(texturePath, ec))
    {
      return rtpt::LoadImageRgba8(texturePath);
    }
  }

  return std::nullopt;
}

// Builds the HDRI importance sampling tables: a discrete PDF and CDF over texels in row-major order.
// The tables stay empty, with zero dimensions, when the image cannot be decoded or is completely black, and shaders then fall back to cosine sampling.
void BuildEnvironmentSamplingData(const std::filesystem::path& hdriPath, GltfSceneResource& sceneResource)
{
  sceneResource.environmentCdf.clear();
  sceneResource.environmentPdf.clear();

  sceneResource.environmentWidth  = 0;
  sceneResource.environmentHeight = 0;

  const std::optional<rtpt::ImageRgba32f> image = rtpt::LoadImageRgba32f(hdriPath);

  if(!image.has_value() || image->width == 0 || image->height == 0)
  {
    return;
  }

  // Texel weights
  // Each texel is weighted by its luminance times sin(theta) at the row center. In a lat-long map, rows near the poles cover less solid angle, and Hdri.hlsli divides the same sin(theta) back out when converting to a solid-angle PDF.
  // The sin(theta) floor keeps pole rows from getting exactly zero weight.

  const uint32_t width      = image->width;
  const uint32_t height     = image->height;
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
      const double    weight = static_cast<double>(std::max(ComputeLuminance(radiance), 0.0f) * sinTheta);

      weights[index] = weight;
      totalWeight   += weight;
    }
  }

  // A black environment has nothing to importance sample.
  if(totalWeight <= 0.0)
  {
    return;
  }

  // Tables
  // Accumulation runs in double precision to limit drift across millions of texels. The CDF is still clamped to 1 so rounding cannot push the last entries past the end of the range.

  sceneResource.environmentWidth  = width;
  sceneResource.environmentHeight = height;

  sceneResource.environmentCdf.resize(texelCount);
  sceneResource.environmentPdf.resize(texelCount);

  double cumulative = 0.0;

  for(size_t i = 0; i < texelCount; ++i)
  {
    const double probability = weights[i] / totalWeight;

    cumulative += probability;

    sceneResource.environmentPdf[i] = static_cast<float>(probability);
    sceneResource.environmentCdf[i] = static_cast<float>(std::min(cumulative, 1.0));
  }
}

// Appends one world-space light record for every non-degenerate triangle of the model's instances whose material emits.
// Instances must already carry their final transforms and material overrides, because both are baked into the records.
void AppendEmissiveTrianglesFromModel(const tinygltf::Model& model, const GltfSceneResource& sceneResource, uint32_t instanceStart, uint32_t instanceCount, std::vector<shaderio::EmissiveTriangleLight>& outTriangles)
{
  if(model.buffers.empty())
  {
    return;
  }

  // Mesh offsets address the model's first buffer, the same one ImportGltfData uploads.
  const unsigned char* base = model.buffers[0].data.data();

  for(uint32_t instanceOffset = 0; instanceOffset < instanceCount; ++instanceOffset)
  {
    // Instances
    // Emission is decided per instance from its material's emission factor; instances with invalid indices or no emission contribute nothing.

    const uint32_t sceneInstanceIndex = instanceStart + instanceOffset;

    const shaderio::GltfInstance& instance = sceneResource.instances[sceneInstanceIndex];

    if(instance.materialIndex >= sceneResource.materials.size() || instance.meshIndex >= sceneResource.meshes.size())
    {
      continue;
    }

    const shaderio::GltfMetallicRoughness& material = sceneResource.materials[instance.materialIndex];

    if(ComputeLuminance(glm::vec3(material.emissionFactor)) <= 0.0f)
    {
      continue;
    }

    const shaderio::GltfMesh& mesh = sceneResource.meshes[instance.meshIndex];
    const uint32_t primitiveCount  = mesh.triMesh.indices.count / 3u;

    for(uint32_t primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
    {
      // Triangle geometry
      // Positions are moved to world space so the area and normal match what is rendered. Texture coordinates are kept so shaders can evaluate the emissive texture at the sampled point.
      // Degenerate triangles are skipped: they cannot be hit, and normalizing their zero-length cross product would produce NaNs.

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

      // Light record
      // The instance and primitive indices let shaders recognize a hit on this same triangle and skip sampling a surface as its own light.

      shaderio::EmissiveTriangleLight light {};

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

SceneUploader::SceneUploader(rtpt::ResourceAllocator& resources, rtpt::UploadContext& uploads)
    : m_Resources(&resources)
    , m_Uploads(&uploads)
{
}

int SceneUploader::Upload(const UploadInput& input, UploadState& state) const
{
  // Texture upload
  // Every scene texture is a single-mip 2D image with a linear-filtered sampler, uploaded straight into SHADER_READ_ONLY_OPTIMAL for the raster, ray tracing, and compute stages.
  // The return value is the texture's index in state.textures, or -1 when there are no pixels to upload.

  const auto appendTexture = [&](std::span<const std::byte> pixels, uint32_t width, uint32_t height, VkFormat format) -> int {
    if(width == 0 || height == 0 || pixels.empty())
    {
      return -1;
    }

    const VkImageCreateInfo imageInfo {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // Filled once by a transfer, then only sampled.
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    const VkImageViewCreateInfo viewInfo {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
    };

    // Unset fields stay zero, which leaves the address mode at REPEAT and the mip mode at NEAREST; glTF sampler settings are not applied.
    const VkSamplerCreateInfo samplerInfo {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
    };

    SceneTexture texture;

    rtpt::CheckVk(m_Resources->CreateImage(texture.image, imageInfo, &viewInfo), "ResourceAllocator::CreateImage(scene texture)");

    // Until the texture is moved into state.textures nothing else owns it, so a failed sampler creation or upload must release it before rethrowing.
    try
    {
      rtpt::CheckVk(m_Resources->CreateSampler(texture.sampler, samplerInfo), "ResourceAllocator::CreateSampler(scene texture)");

      m_Uploads->UploadImage({ .image = texture.image.image, .extent = imageInfo.extent, .subresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 }, .before = { .layout = VK_IMAGE_LAYOUT_UNDEFINED }, .after = { .access = { .stages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT }, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } }, pixels);

      texture.image.descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    catch(...)
    {
      texture.sampler.Reset();
      texture.image.Reset();
      throw;
    }

    state.textures.emplace_back(std::move(texture));

    return static_cast<int>(state.textures.size()) - 1;
  };

  // 8-bit textures pick the sRGB format for color data so sampling decodes to linear, and UNORM for data such as normals and roughness.
  const auto appendRgbaTexture = [&](std::span<const unsigned char> rgbaPixels, uint32_t width, uint32_t height, bool sRgb) -> int {
    return appendTexture(std::as_bytes(rgbaPixels), width, height, sRgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
  };

  // Resolves a scene asset path: an existing absolute path is used as is, otherwise each content root is tried in order. Throws when nothing matches.
  const auto resolveFile = [&](const std::filesystem::path& requested) {
    std::error_code error;

    if(requested.is_absolute() && std::filesystem::is_regular_file(requested, error) && !error)
    {
      return std::filesystem::weakly_canonical(requested);
    }

    for(const std::filesystem::path& root : input.contentRoots)
    {
      const std::filesystem::path candidate = root / requested;

      error.clear();

      if(std::filesystem::is_regular_file(candidate, error) && !error)
      {
        return std::filesystem::weakly_canonical(candidate);
      }
    }

    throw std::runtime_error("scene asset was not found: " + requested.string());
  };

  // Emissive texture means
  // Mean linear luminance per scene texture, indexed like state.textures, for weighting emissive triangles once every model is uploaded. Only textures an emissive slot samples are measured, while their pixels are still on the CPU.
  // The vector only reaches the last measured texture, and the entries in between hold 1, which BuildEmissiveTriangleCdf treats the same as no entry; no emissive slot points at them.

  std::vector<float> textureMeanLuminance;

  // Model textures
  // An image is uploaded once per color space its material slots sample it in: an image read as color gets an SRGB upload and one read as data gets a UNORM upload. Uploading every image in both formats would roughly double texture memory, since most images are only ever read one way, and images no slot references are not uploaded at all.
  // The returned maps take a glTF texture index to a scene texture index per color space. Textures sharing a source image share its uploads.

  auto uploadModelTextures = [&](const tinygltf::Model& model, const std::filesystem::path& modelPath) -> ModelTextureMaps {
    const std::vector<ImageColorSpaces> imageColorSpaces = CollectImageColorSpaces(model);

    std::vector<int> srgbImageTextures(model.images.size(), -1);
    std::vector<int> linearImageTextures(model.images.size(), -1);

    const size_t firstTextureIndex = state.textures.size();
    size_t       uploadedBytes     = 0;

    for(size_t imageIndex = 0; imageIndex < model.images.size(); ++imageIndex)
    {
      const ImageColorSpaces& colorSpaces = imageColorSpaces[imageIndex];

      if(!colorSpaces.sRgb && !colorSpaces.linear)
      {
        continue;
      }

      const std::optional<rtpt::ImageRgba8> decoded = DecodeGltfImage(model.images[imageIndex], modelPath);

      if(!decoded)
      {
        continue;
      }

      if(colorSpaces.sRgb)
      {
        srgbImageTextures[imageIndex] = appendRgbaTexture(decoded->pixels, decoded->width, decoded->height, true);
        uploadedBytes                += decoded->pixels.size();
      }

      if(colorSpaces.linear)
      {
        linearImageTextures[imageIndex] = appendRgbaTexture(decoded->pixels, decoded->width, decoded->height, false);
        uploadedBytes                  += decoded->pixels.size();
      }

      // Light selection needs the mean after every model is uploaded, and this is the last point the pixels are on the CPU.
      if(colorSpaces.emissive && srgbImageTextures[imageIndex] >= 0)
      {
        textureMeanLuminance.resize(state.textures.size(), 1.0f);

        textureMeanLuminance[srgbImageTextures[imageIndex]] = ComputeMeanLinearLuminanceSrgb(decoded->pixels);
      }
    }

    // Logged per model so the texture memory the per-color-space uploads save is visible.
    if(!model.images.empty())
    {
      rtpt::Log(rtpt::LogLevel::Info, fmt::format("Uploaded {} textures ({:.1f} MiB) for {} glTF images of {}", state.textures.size() - firstTextureIndex, static_cast<double>(uploadedBytes) / (1024.0 * 1024.0), model.images.size(), modelPath.filename().string()));
    }

    ModelTextureMaps textureMaps;

    textureMaps.srgb.assign(model.textures.size(), -1);
    textureMaps.linear.assign(model.textures.size(), -1);

    for(size_t textureIndex = 0; textureIndex < model.textures.size(); ++textureIndex)
    {
      const int imageIndex = model.textures[textureIndex].source;

      if(imageIndex < 0 || imageIndex >= static_cast<int>(model.images.size()))
      {
        continue;
      }

      textureMaps.srgb[textureIndex]   = srgbImageTextures[imageIndex];
      textureMaps.linear[textureIndex] = linearImageTextures[imageIndex];
    }

    return textureMaps;
  };

  // Looks up a glTF texture index in a texture map; a missing or out-of-range index means no texture.
  auto getTextureIndex = [](const std::vector<int>& textureMap, int gltfTextureIndex) -> int {
    if(gltfTextureIndex < 0 || gltfTextureIndex >= static_cast<int>(textureMap.size()))
    {
      return -1;
    }

    return textureMap[gltfTextureIndex];
  };

  // Model materials
  // Appends one CPU and one GPU material per glTF material, keeping state.materialAttributes parallel to sceneResource.materials.
  // A model without materials gets a single default material, so its range is never empty.

  auto addModelMaterials = [&](const tinygltf::Model& model, const ModelTextureMaps& textureMaps) -> MaterialRange {
    const uint32_t materialOffset   = static_cast<uint32_t>(state.sceneResource.materials.size());
    const uint32_t attributesOffset = static_cast<uint32_t>(state.materialAttributes.size());

    if(model.materials.empty())
    {
      MaterialAttributes defaultMaterial {};

      state.materialAttributes.push_back(defaultMaterial);
      state.sceneResource.materials.push_back(ToGpuMaterial(defaultMaterial, -1, -1, MaterialTextureIndices {}, 0.5f, shaderio::GltfAlphaMode::eOpaque));

      return { .offset = materialOffset, .count = 1 };
    }

    for(const tinygltf::Material& src : model.materials)
    {
      const MaterialAttributes parsedMaterial = ParseMaterialAttributes(src);

      state.materialAttributes.push_back(parsedMaterial);

      // Texture color spaces
      // Each slot reads the texture map of the color space kMaterialTextureSlots gives it, the same table uploadModelTextures used to decide which formats each image needed.

      const MaterialTextureIndices gltfTextureIndices = ReadGltfTextureIndices(src);

      MaterialTextureIndices textureIndices {};

      for(const MaterialTextureSlot& slot : kMaterialTextureSlots)
      {
        textureIndices.*slot.field = getTextureIndex(slot.sRgb ? textureMaps.srgb : textureMaps.linear, gltfTextureIndices.*slot.field);
      }

      // Alpha mode
      // glTF names the mode as a string; anything other than MASK or BLEND, including the default OPAQUE, is treated as opaque.

      int alphaMode = shaderio::GltfAlphaMode::eOpaque;

      if(src.alphaMode == "MASK")
        alphaMode = shaderio::GltfAlphaMode::eMask;
      else if(src.alphaMode == "BLEND")
        alphaMode = shaderio::GltfAlphaMode::eBlend;

      state.sceneResource.materials.push_back(ToGpuMaterial(parsedMaterial, textureIndices.baseColor, textureIndices.metallicRoughness, textureIndices, static_cast<float>(src.alphaCutoff), alphaMode));
    }

    return { .offset = materialOffset, .count = static_cast<uint32_t>(state.materialAttributes.size() - attributesOffset) };
  };

  // Appends an untextured opaque default material and returns its scene index; it serves primitives whose authored material is missing or invalid.
  auto addDefaultMaterial = [&]() -> uint32_t {
    const uint32_t materialIndex = static_cast<uint32_t>(state.sceneResource.materials.size());

    MaterialAttributes defaultMaterial {};

    state.materialAttributes.push_back(defaultMaterial);
    state.sceneResource.materials.push_back(ToGpuMaterial(defaultMaterial, -1, -1, MaterialTextureIndices {}, 0.5f, shaderio::GltfAlphaMode::eOpaque));

    return materialIndex;
  };

  // Material overrides
  // Scene definitions restyle stock models by replacing material attributes. An override with a slot replaces one material of the range; one without a slot replaces all of them and the model's fallback material too, so primitives without an authored material follow the override.
  // Only attribute factors change: texture indices, alpha mode, alpha cutoff, and the base color alpha stay as imported. Overrides run in order, so later ones win.

  auto applyMaterialOverrides = [&](const MaterialRange& range, const std::vector<SceneMaterialOverride>& overrides, std::optional<uint32_t> fallbackMaterialIndex = std::nullopt) {
    if(range.count == 0 || overrides.empty())
      return;

    for(const SceneMaterialOverride& overrideDef : overrides)
    {
      uint32_t begin = range.offset;
      uint32_t end   = range.offset + range.count;

      // A slot past the end of the model's materials is ignored.
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
        // The imported alpha factor survives on the CPU copy, as it does in the GPU material below.
        const float importedAlpha = state.materialAttributes[i].alpha;

        state.materialAttributes[i]       = overrideDef.attributes;
        state.materialAttributes[i].alpha = importedAlpha;

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

      // The fallback material sits outside the range, so a whole-model override updates it separately.
      if(overrideDef.materialSlot < 0 && fallbackMaterialIndex.has_value())
      {
        const uint32_t fallbackIndex = *fallbackMaterialIndex;
        const float    importedAlpha = state.materialAttributes[fallbackIndex].alpha;

        state.materialAttributes[fallbackIndex]       = overrideDef.attributes;
        state.materialAttributes[fallbackIndex].alpha = importedAlpha;

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

  // Environment map
  // The HDRI is uploaded as a float texture for shading and also turned into the importance sampling tables. A selected HDRI that cannot be found or decoded fails the whole upload.

  int environmentTextureIndex = -1;

  if(input.selectedHdriRelativePath.has_value())
  {
    const std::filesystem::path hdriPath = resolveFile(*input.selectedHdriRelativePath);

    const std::optional<rtpt::ImageRgba32f> image = rtpt::LoadImageRgba32f(hdriPath);

    if(!image)
    {
      throw std::runtime_error("failed to decode environment image: " + hdriPath.string());
    }

    environmentTextureIndex = appendTexture(std::as_bytes(std::span(image->pixels)), image->width, image->height, VK_FORMAT_R32G32B32A32_SFLOAT);

    BuildEnvironmentSamplingData(hdriPath, state.sceneResource);
  }

  for(const SceneModelEntry& modelEntry : input.sceneDefinition.models)
  {
    // Model textures and materials
    // A glTF texture carries no color space of its own, so images are uploaded in the color spaces the model's material slots sample them in, and each slot then reads the map of its own color space.

    const std::filesystem::path modelPath = resolveFile(modelEntry.assetPath);
    const tinygltf::Model       model     = rtpt::LoadGltfResources(modelPath);

    const ModelTextureMaps textureMaps   = uploadModelTextures(model, modelPath);
    const MaterialRange    materialRange = addModelMaterials(model, textureMaps);

    // Fallback material
    // A material-less model already received a default material, which doubles as its fallback. A model whose materials exist but some primitive lacks a valid one gets a dedicated default appended after its range.
    // When no fallback is needed the index stays empty and the range's first material is passed as a placeholder that no primitive uses.

    const bool              needsFallbackMaterial = ModelNeedsFallbackMaterial(model);
    std::optional<uint32_t> fallbackMaterialIndex;

    if(model.materials.empty())
    {
      fallbackMaterialIndex = materialRange.offset;
    }
    else if(needsFallbackMaterial)
    {
      fallbackMaterialIndex = addDefaultMaterial();
    }

    // Geometry import
    // The mesh and instance counts before the import mark where this model's entries begin.

    const uint32_t meshStartIndex = static_cast<uint32_t>(state.sceneResource.meshes.size());
    const uint32_t instanceStart  =static_cast<uint32_t>(state.sceneResource.instances.size());

    rtpt::ImportGltfData(state.sceneResource, model, *m_Resources, *m_Uploads, modelEntry.importNodeInstances, materialRange.offset, fallbackMaterialIndex.value_or(materialRange.offset));

    // Identity instances
    // When node instancing is off, or the node graph produced no instances, each imported mesh is placed once with an identity transform and its authored material.

    uint32_t importedInstanceCount = static_cast<uint32_t>(state.sceneResource.instances.size()) - instanceStart;

    if(importedInstanceCount == 0)
    {
      const uint32_t meshCount = static_cast<uint32_t>(state.sceneResource.meshes.size()) - meshStartIndex;

      for(uint32_t meshLocal = 0; meshLocal < meshCount; ++meshLocal)
      {
        const uint32_t meshIndex     = meshStartIndex + meshLocal;
        uint32_t       materialIndex =fallbackMaterialIndex.value_or(materialRange.offset);

        if(meshIndex < state.sceneResource.meshMaterialIndices.size())
          materialIndex = state.sceneResource.meshMaterialIndices[meshIndex];

        state.sceneResource.instances.push_back({ .transform = glm::mat4(1.0f), .materialIndex = materialIndex, .meshIndex = meshIndex });
      }

      importedInstanceCount = static_cast<uint32_t>(state.sceneResource.instances.size()) - instanceStart;
    }

    // Placement
    // The scene entry's transform is applied outside every node transform, placing the whole model in the scene.

    for(uint32_t i = 0; i < importedInstanceCount; ++i)
    {
      shaderio::GltfInstance& instance = state.sceneResource.instances[instanceStart + i];
      instance.transform               = modelEntry.transform * instance.transform;
    }

    // Overrides and lights
    // Overrides run before emissive extraction, so an override that adds or removes emission changes which triangles become lights.

    applyMaterialOverrides(materialRange, modelEntry.materialOverrides, fallbackMaterialIndex);
    AppendEmissiveTrianglesFromModel(model, state.sceneResource, instanceStart, importedInstanceCount, state.sceneResource.emissiveTriangles);
  }

  // Light sampling
  // The emissive CDF is built once over the lights of every model, so light selection is scene-wide. Emissive textures scale the weights by the means measured during texture upload.

  BuildEmissiveTriangleCdf(state.sceneResource.emissiveTriangles, state.sceneResource.emissiveTriangleCdf, state.sceneResource.materials, textureMeanLuminance);

  return environmentTextureIndex;
}

}  // namespace rtpt
