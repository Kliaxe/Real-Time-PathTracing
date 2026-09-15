#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace rtpt
{

// MaterialAttributes
// Canonical CPU-side material attributes for scene authoring.
// Some values are not consumed by shaders yet, but they are still parsed and carried so scene data is forward-compatible with future shading work.

struct MaterialAttributes
{
  // Emitted radiance, already scaled by KHR_materials_emissive_strength when imported.
  glm::vec3 emission = glm::vec3(0.0f, 0.0f, 0.0f);

  // Base color, linear RGB.
  glm::vec3 albedo = glm::vec3(1.0f, 1.0f, 1.0f);

  // Base color alpha factor in [0, 1], from baseColorFactor[3]. Shaders multiply it with the base color texture's alpha before the alpha mask test.
  float alpha = 1.0f;

  // Whether back faces shade like front faces.
  bool doubleSided = false;

  // Dielectric specular strength in [0, 1].
  float specular = 1.0f;

  // Dielectric specular tint in [0, 1]; imported as the mean of specularColorFactor.
  float specularTint = 0.0f;

  // Metalness in [0, 1].
  float metallic = 0.0f;

  // Perceptual roughness in [0, 1].
  float roughness = 1.0f;

  // Diffuse-to-subsurface blend in [0, 1]. No glTF extension feeds it.
  float subsurface = 0.0f;

  // Anisotropic roughness stretch in [0, 1].
  float anisotropy = 0.0f;

  // Beer-Lambert color reached at attenuationDistance.
  glm::vec3 attenuationColor = glm::vec3(1.0f, 1.0f, 1.0f);

  // Distance at which absorption reaches attenuationColor. The default is effectively infinite, meaning no absorption.
  float attenuationDistance = 1.0e30f;

  // Minimum thickness used for volumetric absorption.
  float volumeThickness = 0.0f;

  // Sheen reflectance, RGB in [0, 1].
  glm::vec3 sheenColor = glm::vec3(0.0f, 0.0f, 0.0f);

  // Sheen roughness in [0, 1].
  float sheenRoughness = 0.0f;

  // Clearcoat layer strength in [0, 1].
  float clearcoat = 0.0f;

  // Clearcoat layer roughness in [0, 1].
  float clearcoatRoughness = 0.0f;

  // Index of refraction.
  float refraction = 1.5f;

  // 0 is opaque, 1 is a fully transmissive dielectric.
  float transmission = 0.0f;
};

// SceneMaterialOverride
// Per-material-slot override for one model entry in a scene, letting built-in scenes restyle stock models without editing the asset.

struct SceneMaterialOverride
{
  // Material index within the model; -1 means apply to all materials in that model.
  int materialSlot = -1;

  // Attributes that replace the imported ones. Texture bindings and alpha settings of the target, including its alpha factor, are kept.
  MaterialAttributes attributes {};
};

// SceneModelEntry
// One model placed in a scene. A scene can be composed from multiple model entries.

struct SceneModelEntry
{
  // Display name for the entry.
  std::string name;

  // Content-relative path of the .gltf or .glb file.
  std::filesystem::path assetPath;

  // Applied on top of every instance the model produces.
  glm::mat4 transform = glm::mat4(1.0f);

  // Whether to keep the glTF scene graph instances from the file or create a single identity instance per mesh.
  bool importNodeInstances = true;

  // Applied after the model's materials are imported, in order.
  std::vector<SceneMaterialOverride> materialOverrides;
};

// SceneDefinition
// Scene-level definition authored once and consumed by the runtime UI and build code.

struct SceneDefinition
{
  // Name shown in the scene selector and used to find the default scene.
  std::string label;

  // Models composed into the scene, uploaded in this order.
  std::vector<SceneModelEntry> models;
};

}  // namespace rtpt
