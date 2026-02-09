#include "SceneCatalog.h"

#include <array>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

namespace nvsamples
{

namespace
{

// Small helper to keep scene entries concise and readable.
SceneModelEntry ModelEntry(std::string name, const char* assetPath, const glm::mat4& transform = glm::mat4(1.0f))
{
  SceneModelEntry entry{};
  entry.name      = std::move(name);
  entry.assetPath = assetPath;
  entry.transform = transform;
  return entry;
}

// Material presets used by the old demo scenes.
MaterialAttributes DielectricPreset()
{
  MaterialAttributes mat{};
  mat.albedo    = glm::vec3(1.0f, 0.73f, 0.05f) * 0.8f;
  mat.roughness = 0.05f;
  return mat;
}

MaterialAttributes MetallicPreset()
{
  MaterialAttributes mat = DielectricPreset();
  mat.metallic           = 1.0f;
  return mat;
}

MaterialAttributes GlassPreset()
{
  MaterialAttributes mat{};
  mat.albedo       = glm::vec3(1.0f, 0.15f, 0.0f) * 0.8f;
  mat.roughness    = 0.05f;
  mat.transmission = 1.0f;
  return mat;
}

MaterialAttributes ClearcoatPreset()
{
  MaterialAttributes mat{};
  mat.albedo             = glm::vec3(0.0f, 0.4f, 1.0f) * 0.8f;
  mat.metallic           = 1.0f;
  mat.roughness          = 0.8f;
  mat.clearcoat          = 1.0f;
  mat.clearcoatRoughness = 0.01f;
  return mat;
}

SceneDefinition SingleModelScene(const char* label, const char* assetPath)
{
  SceneDefinition scene{};
  scene.label = label;
  scene.models.push_back(ModelEntry(label, assetPath));
  return scene;
}

SceneDefinition BunnyVariantScene(const char* label, const MaterialAttributes& bunnyMaterial)
{
  SceneDefinition scene{};
  scene.label = label;

  SceneModelEntry bunny = ModelEntry("Bunny", "Models/Bunny.glb");
  bunny.materialOverrides.push_back({.materialSlot = 0, .attributes = bunnyMaterial});
  scene.models.push_back(bunny);
  scene.models.push_back(ModelEntry("Floor", "Models/Floor.glb"));
  return scene;
}

SceneDefinition DragonVariantScene(const char* label, const MaterialAttributes& dragonMaterial)
{
  SceneDefinition scene{};
  scene.label = label;

  SceneModelEntry dragon = ModelEntry("Dragon", "Models/Dragon.glb");
  dragon.materialOverrides.push_back({.materialSlot = 0, .attributes = dragonMaterial});
  scene.models.push_back(dragon);
  scene.models.push_back(ModelEntry("Floor", "Models/Floor.glb"));
  return scene;
}

}  // namespace

std::vector<SceneDefinition> CreateSceneCatalog()
{
  // Central scene registry.
  // Add new scenes here and they automatically appear in ImGui.
  std::vector<SceneDefinition> scenes;
  scenes.reserve(13);

  scenes.push_back(SingleModelScene("Area Light", "Models/AreaLight/AreaLight.gltf"));
  scenes.push_back(SingleModelScene("Fireplace", "Models/Fireplace/Fireplace.gltf"));
  scenes.push_back(SingleModelScene("Mill", "Models/Mill/Mill.gltf"));
  scenes.push_back(SingleModelScene("Sponza", "Models/Sponza/Sponza.gltf"));
  scenes.push_back(SingleModelScene("Sponza Reduced", "Models/Sponza/SponzaReduced.gltf"));

  scenes.push_back(BunnyVariantScene("Bunny Dielectric", DielectricPreset()));
  scenes.push_back(BunnyVariantScene("Bunny Metallic", MetallicPreset()));
  scenes.push_back(BunnyVariantScene("Bunny Glass", GlassPreset()));
  scenes.push_back(BunnyVariantScene("Bunny Clearcoat", ClearcoatPreset()));

  scenes.push_back(DragonVariantScene("Dragon Dielectric", DielectricPreset()));
  scenes.push_back(DragonVariantScene("Dragon Metallic", MetallicPreset()));
  scenes.push_back(DragonVariantScene("Dragon Glass", GlassPreset()));
  scenes.push_back(DragonVariantScene("Dragon Clearcoat", ClearcoatPreset()));

  return scenes;
}

size_t FindDefaultSceneIndex(const std::vector<SceneDefinition>& scenes)
{
  // Prefer a stable "material test" scene on startup when present.
  static constexpr std::array<const char*, 2> kPreferredDefaults = {"Bunny Dielectric", "Sponza"};

  for(const char* preferred : kPreferredDefaults)
  {
    for(size_t i = 0; i < scenes.size(); ++i)
    {
      if(scenes[i].label == preferred)
      {
        return i;
      }
    }
  }

  return 0;
}

}  // namespace nvsamples
