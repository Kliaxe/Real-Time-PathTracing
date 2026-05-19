#include "SceneCatalog.h"

#include <array>
#include <cmath>
#include <string>
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

glm::mat4 ComposeTransform(const glm::vec3& translation, const glm::vec3& rotationRadians, const glm::vec3& scale)
{
  glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation);
  transform           = glm::rotate(transform, rotationRadians.x, glm::vec3(1.0f, 0.0f, 0.0f));
  transform           = glm::rotate(transform, rotationRadians.y, glm::vec3(0.0f, 1.0f, 0.0f));
  transform           = glm::rotate(transform, rotationRadians.z, glm::vec3(0.0f, 0.0f, 1.0f));
  transform           = glm::scale(transform, scale);
  return transform;
}

MaterialAttributes DiffuseMaterial(const glm::vec3& albedo)
{
  MaterialAttributes mat{};
  mat.albedo      = albedo;
  mat.specular    = 0.0f;
  mat.roughness   = 1.0f;
  mat.doubleSided = true;
  return mat;
}

MaterialAttributes EmissiveMaterial(const glm::vec3& emission)
{
  MaterialAttributes mat{};
  mat.albedo      = glm::vec3(1.0f);
  mat.emission    = emission;
  mat.roughness   = 1.0f;
  mat.doubleSided = true;
  return mat;
}

MaterialAttributes RoughMetalMaterial(const glm::vec3& albedo, float roughness)
{
  MaterialAttributes mat = DiffuseMaterial(albedo);
  mat.metallic           = 1.0f;
  mat.roughness          = roughness;
  return mat;
}

MaterialAttributes GlossyDielectricMaterial(const glm::vec3& albedo, float roughness)
{
  MaterialAttributes mat = DiffuseMaterial(albedo);
  mat.roughness          = roughness;
  mat.specular           = 1.0f;
  return mat;
}

MaterialAttributes GlassMaterial(const glm::vec3& tint)
{
  MaterialAttributes mat{};
  mat.albedo       = tint;
  mat.roughness    = 0.02f;
  mat.specular     = 1.0f;
  mat.transmission = 1.0f;
  mat.refraction   = 1.5f;
  mat.doubleSided  = true;
  return mat;
}

SceneModelEntry MaterialModelEntry(std::string name, const char* assetPath, const glm::mat4& transform,
                                   const MaterialAttributes& material)
{
  SceneModelEntry entry = ModelEntry(std::move(name), assetPath, transform);
  entry.materialOverrides.push_back({.materialSlot = -1, .attributes = material});
  return entry;
}

void AddCornellRoom(SceneDefinition& scene)
{
  static constexpr float kHalfSize     = 2.5f;
  static constexpr float kHalfHeight   = 2.0f;
  static constexpr float kRoomHeight   = 4.0f;
  static constexpr float kPi           = 3.14159265359f;
  static constexpr float kHalfPi       = kPi * 0.5f;

  const MaterialAttributes white = DiffuseMaterial(glm::vec3(0.72f));
  const MaterialAttributes red   = DiffuseMaterial(glm::vec3(0.78f, 0.12f, 0.08f));
  const MaterialAttributes green = DiffuseMaterial(glm::vec3(0.10f, 0.55f, 0.18f));

  scene.models.push_back(MaterialModelEntry("Floor", "Models/Plane.glb",
                                            ComposeTransform(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f),
                                                             glm::vec3(kHalfSize, 1.0f, kHalfSize)),
                                            white));
  scene.models.push_back(MaterialModelEntry("Ceiling", "Models/Plane.glb",
                                            ComposeTransform(glm::vec3(0.0f, kRoomHeight, 0.0f), glm::vec3(0.0f),
                                                             glm::vec3(kHalfSize, 1.0f, kHalfSize)),
                                            white));
  scene.models.push_back(MaterialModelEntry("Back Wall", "Models/Plane.glb",
                                            ComposeTransform(glm::vec3(0.0f, kHalfHeight, -kHalfSize),
                                                             glm::vec3(kHalfPi, 0.0f, 0.0f),
                                                             glm::vec3(kHalfSize, 1.0f, kHalfHeight)),
                                            white));
  scene.models.push_back(MaterialModelEntry("Left Wall", "Models/Plane.glb",
                                            ComposeTransform(glm::vec3(-kHalfSize, kHalfHeight, 0.0f),
                                                             glm::vec3(0.0f, 0.0f, kHalfPi),
                                                             glm::vec3(kHalfHeight, 1.0f, kHalfSize)),
                                            red));
  scene.models.push_back(MaterialModelEntry("Right Wall", "Models/Plane.glb",
                                            ComposeTransform(glm::vec3(kHalfSize, kHalfHeight, 0.0f),
                                                             glm::vec3(0.0f, 0.0f, kHalfPi),
                                                             glm::vec3(kHalfHeight, 1.0f, kHalfSize)),
                                            green));
}

void AddCornellObjects(SceneDefinition& scene)
{
  scene.models.push_back(MaterialModelEntry("Rough Metal Cube", "Models/Cube.glb",
                                            ComposeTransform(glm::vec3(-0.85f, 0.65f, -0.65f),
                                                             glm::vec3(0.0f, 0.55f, 0.0f),
                                                             glm::vec3(0.45f, 0.65f, 0.45f)),
                                            RoughMetalMaterial(glm::vec3(0.86f, 0.78f, 0.62f), 0.32f)));
  scene.models.push_back(MaterialModelEntry("Glossy Sphere", "Models/Sphere.glb",
                                            ComposeTransform(glm::vec3(0.72f, 0.58f, 0.10f), glm::vec3(0.0f),
                                                             glm::vec3(0.58f)),
                                            GlossyDielectricMaterial(glm::vec3(0.92f, 0.95f, 1.0f), 0.08f)));
  scene.models.push_back(MaterialModelEntry("Small Glass Sphere", "Models/Sphere.glb",
                                            ComposeTransform(glm::vec3(0.05f, 0.33f, -1.35f), glm::vec3(0.0f),
                                                             glm::vec3(0.33f)),
                                            GlassMaterial(glm::vec3(0.82f, 0.95f, 1.0f))));
}

void AddSingleCornellLight(SceneDefinition& scene)
{
  scene.models.push_back(MaterialModelEntry("Ceiling Area Light", "Models/Plane.glb",
                                            ComposeTransform(glm::vec3(0.0f, 3.96f, -0.25f), glm::vec3(0.0f),
                                                             glm::vec3(0.75f, 1.0f, 0.55f)),
                                            EmissiveMaterial(glm::vec3(18.0f, 16.0f, 13.0f))));
}

void AddManyCornellLights(SceneDefinition& scene)
{
  int lightIndex = 0;
  for(int z = 0; z < 4; ++z)
  {
    for(int x = 0; x < 6; ++x)
    {
      const float fx = -1.75f + static_cast<float>(x) * 0.70f;
      const float fz = -1.65f + static_cast<float>(z) * 0.75f;
      const glm::vec3 color = glm::vec3(36.0f + 4.0f * static_cast<float>((x + z) % 2),
                                        30.0f + 5.0f * static_cast<float>(x % 3),
                                        24.0f + 6.0f * static_cast<float>(z % 2));

      scene.models.push_back(MaterialModelEntry("Ceiling Light " + std::to_string(lightIndex), "Models/Plane.glb",
                                                ComposeTransform(glm::vec3(fx, 3.96f, fz), glm::vec3(0.0f),
                                                                 glm::vec3(0.16f, 1.0f, 0.16f)),
                                                EmissiveMaterial(color)));
      ++lightIndex;
    }
  }
}

// Material presets used by the built-in thesis scenes.
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

SceneDefinition CornellScene(const char* label, bool manyLights)
{
  SceneDefinition scene{};
  scene.label = label;
  AddCornellRoom(scene);
  AddCornellObjects(scene);
  if(manyLights)
  {
    AddManyCornellLights(scene);
  }
  else
  {
    AddSingleCornellLight(scene);
  }
  return scene;
}

SceneDefinition SponzaStudioScene()
{
  SceneDefinition scene{};
  scene.label = "Sponza Studio";
  scene.models.push_back(ModelEntry("Sponza Reduced", "Models/Sponza/SponzaReduced.gltf",
                                    ComposeTransform(glm::vec3(0.0f, 0.46f, 0.0f), glm::vec3(0.0f),
                                                     glm::vec3(0.45f))));
  return scene;
}

}  // namespace

std::vector<SceneDefinition> CreateSceneCatalog()
{
  // Central scene registry.
  // Add new scenes here and they automatically appear in ImGui.
  std::vector<SceneDefinition> scenes;
  scenes.reserve(16);

  scenes.push_back(SingleModelScene("Area Light", "Models/AreaLight/AreaLight.gltf"));
  scenes.push_back(CornellScene("Cornell Box", false));
  scenes.push_back(CornellScene("Cornell Many Lights", true));
  scenes.push_back(SponzaStudioScene());
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
