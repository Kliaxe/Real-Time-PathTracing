#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace nvsamples
{

// Canonical CPU-side material attributes for scene authoring.
// Some values are not currently consumed by shaders yet, but we still parse
// and carry them so scene data is forward-compatible with future shading work.
struct MaterialAttributes
{
  glm::vec3 emission           = glm::vec3(0.0f, 0.0f, 0.0f);
  glm::vec3 albedo             = glm::vec3(1.0f, 1.0f, 1.0f);
  float     specular           = 1.0f;
  float     specularTint       = 0.0f;
  float     metallic           = 0.0f;
  float     roughness          = 1.0f;
  float     subsurface         = 0.0f;
  float     anisotropy         = 0.0f;
  float     sheenRoughness     = 0.0f;
  float     sheenTint          = 0.0f;
  float     clearcoat          = 0.0f;
  float     clearcoatRoughness = 0.0f;
  float     refraction         = 1.5f;
  float     transmission       = 0.0f;
};

// Per-material-slot override for one model entry in a scene.
// materialSlot == -1 means "apply to all materials in that model".
struct SceneMaterialOverride
{
  int                materialSlot = -1;
  MaterialAttributes attributes{};
};

// A scene can be composed from multiple model entries.
// importNodeInstances controls whether we keep the glTF scene graph instances
// from that file or create a single fallback instance per mesh.
struct SceneModelEntry
{
  std::string                       name;
  std::filesystem::path             assetPath;
  glm::mat4                         transform = glm::mat4(1.0f);
  bool                              importNodeInstances = true;
  std::vector<SceneMaterialOverride> materialOverrides;
};

// Scene-level definition authored once and consumed by runtime UI/build code.
struct SceneDefinition
{
  std::string                label;
  std::vector<SceneModelEntry> models;
};

}  // namespace nvsamples

