#include "SceneCatalog.h"

#include <array>
#include <cmath>
#include <string>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

namespace rtpt
{

namespace
{

// Small helper to keep scene entries concise and readable.
SceneModelEntry ModelEntry(std::string name, const char* assetPath, const glm::mat4& transform = glm::mat4(1.0f))
{
  SceneModelEntry entry {};

  entry.name      = std::move(name);
  entry.assetPath = assetPath;
  entry.transform = transform;

  return entry;
}

// Builds translate * rotateX * rotateY * rotateZ * scale, so scale applies first and translation last.
glm::mat4 ComposeTransform(const glm::vec3& translation, const glm::vec3& rotationRadians, const glm::vec3& scale)
{
  glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation);
  transform           = glm::rotate(transform, rotationRadians.x, glm::vec3(1.0f, 0.0f, 0.0f));
  transform           = glm::rotate(transform, rotationRadians.y, glm::vec3(0.0f, 1.0f, 0.0f));
  transform           = glm::rotate(transform, rotationRadians.z, glm::vec3(0.0f, 0.0f, 1.0f));
  transform           = glm::scale(transform, scale);

  return transform;
}

// Material builders
// Parameterized materials for the procedurally assembled rooms. All start from MaterialAttributes defaults and set only what defines the look.

MaterialAttributes DiffuseMaterial(const glm::vec3& albedo)
{
  MaterialAttributes mat {};

  mat.albedo      = albedo;
  mat.specular    = 0.0f;
  mat.roughness   = 1.0f;
  mat.doubleSided = true;

  return mat;
}

MaterialAttributes EmissiveMaterial(const glm::vec3& emission)
{
  MaterialAttributes mat {};

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
  MaterialAttributes mat {};

  mat.albedo       = tint;
  mat.roughness    = 0.02f;
  mat.specular     = 1.0f;
  mat.transmission = 1.0f;
  mat.refraction   = 1.5f;
  mat.doubleSided  = true;

  return mat;
}

// A model entry whose every material is replaced, which is how stock primitives like Plane.glb take on a scene-specific look.
SceneModelEntry MaterialModelEntry(std::string name, const char* assetPath, const glm::mat4& transform, const MaterialAttributes& material)
{
  SceneModelEntry entry = ModelEntry(std::move(name), assetPath, transform);

  entry.materialOverrides.push_back({ .materialSlot = -1, .attributes = material });

  return entry;
}

// Cornell room
// Floor, ceiling, and three walls built from scaled planes; the front stays open for the camera. The side walls carry the classic red and green.

void AddCornellRoom(SceneDefinition& scene)
{
  static constexpr float kHalfSize   = 2.5f;
  static constexpr float kHalfHeight = 2.0f;
  static constexpr float kRoomHeight = 4.0f;
  static constexpr float kPi         = 3.14159265359f;
  static constexpr float kHalfPi     = kPi * 0.5f;

  const MaterialAttributes white = DiffuseMaterial(glm::vec3(0.72f));
  const MaterialAttributes red   = DiffuseMaterial(glm::vec3(0.78f, 0.12f, 0.08f));
  const MaterialAttributes green = DiffuseMaterial(glm::vec3(0.10f, 0.55f, 0.18f));

  scene.models.push_back(MaterialModelEntry("Floor", "Models/Plane.glb", ComposeTransform(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(kHalfSize, 1.0f, kHalfSize)), white));
  scene.models.push_back(MaterialModelEntry("Ceiling", "Models/Plane.glb", ComposeTransform(glm::vec3(0.0f, kRoomHeight, 0.0f), glm::vec3(0.0f), glm::vec3(kHalfSize, 1.0f, kHalfSize)), white));
  scene.models.push_back(MaterialModelEntry("Back Wall", "Models/Plane.glb", ComposeTransform(glm::vec3(0.0f, kHalfHeight, -kHalfSize), glm::vec3(kHalfPi, 0.0f, 0.0f), glm::vec3(kHalfSize, 1.0f, kHalfHeight)), white));
  scene.models.push_back(MaterialModelEntry("Left Wall", "Models/Plane.glb", ComposeTransform(glm::vec3(-kHalfSize, kHalfHeight, 0.0f), glm::vec3(0.0f, 0.0f, kHalfPi), glm::vec3(kHalfHeight, 1.0f, kHalfSize)), red));
  scene.models.push_back(MaterialModelEntry("Right Wall", "Models/Plane.glb", ComposeTransform(glm::vec3(kHalfSize, kHalfHeight, 0.0f), glm::vec3(0.0f, 0.0f, kHalfPi), glm::vec3(kHalfHeight, 1.0f, kHalfSize)), green));
}

// One object per material family: rough metal, glossy dielectric, and glass.
void AddCornellObjects(SceneDefinition& scene)
{
  scene.models.push_back(MaterialModelEntry("Rough Metal Cube", "Models/Cube.glb", ComposeTransform(glm::vec3(-0.85f, 0.65f, -0.65f), glm::vec3(0.0f, 0.55f, 0.0f), glm::vec3(0.45f, 0.65f, 0.45f)), RoughMetalMaterial(glm::vec3(0.86f, 0.78f, 0.62f), 0.32f)));
  scene.models.push_back(MaterialModelEntry("Glossy Sphere", "Models/Sphere.glb", ComposeTransform(glm::vec3(0.72f, 0.58f, 0.10f), glm::vec3(0.0f), glm::vec3(0.58f)), GlossyDielectricMaterial(glm::vec3(0.92f, 0.95f, 1.0f), 0.08f)));
  scene.models.push_back(MaterialModelEntry("Small Glass Sphere", "Models/Sphere.glb", ComposeTransform(glm::vec3(0.05f, 0.33f, -1.35f), glm::vec3(0.0f), glm::vec3(0.33f)), GlassMaterial(glm::vec3(0.82f, 0.95f, 1.0f))));
}

// A single warm area light hanging just below the ceiling plane at y = 4.
void AddSingleCornellLight(SceneDefinition& scene)
{
  scene.models.push_back(MaterialModelEntry("Ceiling Area Light", "Models/Plane.glb", ComposeTransform(glm::vec3(0.0f, 3.96f, -0.25f), glm::vec3(0.0f), glm::vec3(0.75f, 1.0f, 0.55f)), EmissiveMaterial(glm::vec3(18.0f, 16.0f, 13.0f))));
}

// A 6 x 4 grid of small ceiling panels with slightly varied colors, all of similar power.
void AddManyCornellLights(SceneDefinition& scene)
{
  int lightIndex = 0;

  for(int z = 0; z < 4; ++z)
  {
    for(int x = 0; x < 6; ++x)
    {
      const float     fx    = -1.75f + static_cast<float>(x) * 0.70f;
      const float     fz    = -1.65f + static_cast<float>(z) * 0.75f;
      const glm::vec3 color = glm::vec3(36.0f + 4.0f * static_cast<float>((x + z) % 2), 30.0f + 5.0f * static_cast<float>(x % 3), 24.0f + 6.0f * static_cast<float>(z % 2));

      scene.models.push_back(MaterialModelEntry("Ceiling Light " + std::to_string(lightIndex), "Models/Plane.glb", ComposeTransform(glm::vec3(fx, 3.96f, fz), glm::vec3(0.0f), glm::vec3(0.16f, 1.0f, 0.16f)), EmissiveMaterial(color)));

      ++lightIndex;
    }
  }
}

// Material presets
// Presets used by the built-in bunny and dragon variants, applied to the model's first material slot.

MaterialAttributes DielectricPreset()
{
  MaterialAttributes mat {};

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
  MaterialAttributes mat {};

  mat.albedo       = glm::vec3(1.0f, 0.15f, 0.0f) * 0.8f;
  mat.roughness    = 0.05f;
  mat.transmission = 1.0f;

  return mat;
}

MaterialAttributes ClearcoatPreset()
{
  MaterialAttributes mat {};

  mat.albedo             = glm::vec3(0.0f, 0.4f, 1.0f) * 0.8f;
  mat.metallic           = 1.0f;
  mat.roughness          = 0.8f;
  mat.clearcoat          = 1.0f;
  mat.clearcoatRoughness = 0.01f;

  return mat;
}

// Scene builders
// Each returns a complete SceneDefinition; CreateSceneCatalog decides their order.

// A scene holding one asset with its own authored materials.
SceneDefinition SingleModelScene(const char* label, const char* assetPath)
{
  SceneDefinition scene {};

  scene.label = label;
  scene.models.push_back(ModelEntry(label, assetPath));

  return scene;
}

SceneDefinition BunnyVariantScene(const char* label, const MaterialAttributes& bunnyMaterial)
{
  SceneDefinition scene {};
  scene.label = label;

  SceneModelEntry bunny = ModelEntry("Bunny", "Models/Bunny.glb");

  bunny.materialOverrides.push_back({ .materialSlot = 0, .attributes = bunnyMaterial });

  scene.models.push_back(bunny);
  scene.models.push_back(ModelEntry("Floor", "Models/Floor.glb"));

  return scene;
}

SceneDefinition DragonVariantScene(const char* label, const MaterialAttributes& dragonMaterial)
{
  SceneDefinition scene {};
  scene.label = label;

  SceneModelEntry dragon = ModelEntry("Dragon", "Models/Dragon.glb");

  dragon.materialOverrides.push_back({ .materialSlot = 0, .attributes = dragonMaterial });

  scene.models.push_back(dragon);
  scene.models.push_back(ModelEntry("Floor", "Models/Floor.glb"));

  return scene;
}

SceneDefinition CornellScene(const char* label, bool manyLights)
{
  SceneDefinition scene {};

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

// Disocclusion scene
// A deliberately layered scene for measuring temporal reuse under motion.
// The rest of the catalog is poor at this: a closed room or a single centred model gives a reprojected pixel a compatible surface almost everywhere, so disocclusion, the case dual motion vectors (Section 6.4) exist to rescue, barely occurs and cannot be measured.
// Here a row of narrow pillars stands far in front of the back wall, so lateral camera motion sweeps them across it and each pillar edge exposes wall that had no history in the previous frame.

SceneDefinition DisocclusionScene()
{
  SceneDefinition scene {};
  scene.label = "Disocclusion Pillars";

  AddCornellRoom(scene);
  AddSingleCornellLight(scene);

  // Pillars
  // Near the camera and thin, so a small camera movement moves each pillar many pixels across a background that is nearly stationary.
  // The gaps matter as much as the pillars: they are what the wall is revealed through.

  static constexpr float kPillarZ      = 1.35f;
  static constexpr float kPillarHeight = 1.75f;
  static constexpr float kPillarHalf   = 0.16f;
  const float            pillarX[4]    = { -1.70f, -0.60f, 0.55f, 1.65f };

  for(int i = 0; i < 4; ++i)
  {
    scene.models.push_back(MaterialModelEntry("Pillar " + std::to_string(i), "Models/Cube.glb", ComposeTransform(glm::vec3(pillarX[i], kPillarHeight, kPillarZ), glm::vec3(0.0f), glm::vec3(kPillarHalf, kPillarHeight, kPillarHalf)), DiffuseMaterial(glm::vec3(0.62f, 0.60f, 0.58f))));
  }

  // Spheres
  // A second, shallower layer. Two depths of occluder mean a disoccluded pixel is sometimes revealed by a pillar and sometimes by a sphere, so the test is not measuring one specific silhouette.

  scene.models.push_back(MaterialModelEntry("Front Sphere", "Models/Sphere.glb", ComposeTransform(glm::vec3(-1.05f, 0.52f, 0.35f), glm::vec3(0.0f), glm::vec3(0.52f)), DiffuseMaterial(glm::vec3(0.70f, 0.35f, 0.25f))));
  scene.models.push_back(MaterialModelEntry("Mid Sphere", "Models/Sphere.glb", ComposeTransform(glm::vec3(1.05f, 0.45f, -0.55f), glm::vec3(0.0f), glm::vec3(0.45f)), DiffuseMaterial(glm::vec3(0.30f, 0.45f, 0.70f))));

  return scene;
}

// Scattered lights scene
// A scene where choosing the light actually matters, for measuring RIS-based NEE (Section 6.1).
// "Cornell Many Lights" does not test this: its 24 ceiling panels have similar power and all illuminate the whole room, so a power-weighted draw is already close to the best proposal available and resampling has nothing to improve.
// The regime RIS is built for is the opposite one: most lights are irrelevant to any given surface, and which ones are irrelevant depends on where the surface is.
// So: a long corridor, lights spread down its whole length, and their power spread over roughly 60x. The brightest emitters are the ones a power CDF picks most often, and from any particular point most of them are far away, behind a partition, or facing the wrong way.
// Resampling against a target that knows the receiving surface is what recovers the nearby dim light that actually lights it.

SceneDefinition ScatteredLightsScene()
{
  SceneDefinition scene {};
  scene.label = "Scattered Lights";

  // Corridor shell
  // A closed box, 6 wide, 24 long, and 4 high, with tinted side walls.

  static constexpr float kHalfWidth  = 3.0f;
  static constexpr float kHalfLength = 12.0f;
  static constexpr float kHeight     = 4.0f;
  static constexpr float kHalfPi     = 1.57079632679f;

  const MaterialAttributes shell = DiffuseMaterial(glm::vec3(0.62f));

  scene.models.push_back(MaterialModelEntry("Floor", "Models/Plane.glb", ComposeTransform(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(kHalfWidth, 1.0f, kHalfLength)), shell));
  scene.models.push_back(MaterialModelEntry("Ceiling", "Models/Plane.glb", ComposeTransform(glm::vec3(0.0f, kHeight, 0.0f), glm::vec3(0.0f), glm::vec3(kHalfWidth, 1.0f, kHalfLength)), shell));
  scene.models.push_back(MaterialModelEntry("Left Wall", "Models/Plane.glb", ComposeTransform(glm::vec3(-kHalfWidth, kHeight * 0.5f, 0.0f), glm::vec3(0.0f, 0.0f, kHalfPi), glm::vec3(kHeight * 0.5f, 1.0f, kHalfLength)), DiffuseMaterial(glm::vec3(0.70f, 0.28f, 0.22f))));
  scene.models.push_back(MaterialModelEntry("Right Wall", "Models/Plane.glb", ComposeTransform(glm::vec3(kHalfWidth, kHeight * 0.5f, 0.0f), glm::vec3(0.0f, 0.0f, kHalfPi), glm::vec3(kHeight * 0.5f, 1.0f, kHalfLength)), DiffuseMaterial(glm::vec3(0.24f, 0.52f, 0.30f))));
  scene.models.push_back(MaterialModelEntry("Far Wall", "Models/Plane.glb", ComposeTransform(glm::vec3(0.0f, kHeight * 0.5f, -kHalfLength), glm::vec3(kHalfPi, 0.0f, 0.0f), glm::vec3(kHalfWidth, 1.0f, kHeight * 0.5f)), shell));
  scene.models.push_back(MaterialModelEntry("Near Wall", "Models/Plane.glb", ComposeTransform(glm::vec3(0.0f, kHeight * 0.5f, kHalfLength), glm::vec3(kHalfPi, 0.0f, 0.0f), glm::vec3(kHalfWidth, 1.0f, kHeight * 0.5f)), shell));

  // Partitions
  // Partitions with alternating gaps. Without them every light reaches every surface and distance alone decides relevance, which is a much weaker test.
  // Occlusion is the part the RIS target deliberately does NOT model, so the estimator has to stay correct where its own target is most wrong.

  for(int i = 0; i < 5; ++i)
  {
    const float z    = -9.0f + static_cast<float>(i) * 4.0f;
    const float side = (i % 2 == 0) ? -1.0f : 1.0f;

    scene.models.push_back(MaterialModelEntry("Partition " + std::to_string(i), "Models/Cube.glb", ComposeTransform(glm::vec3(side * 1.35f, kHeight * 0.5f, z), glm::vec3(0.0f), glm::vec3(1.65f, kHeight * 0.5f, 0.12f)), DiffuseMaterial(glm::vec3(0.58f, 0.56f, 0.60f))));
  }

  // Emitters
  // 132 emitters in a 22 x 6 grid. The power range is the point: a CDF proportional to power spends most of its samples on the few brightest, and those are uniformly distributed along the corridor rather than near any particular shading point.

  int lightIndex = 0;

  for(int row = 0; row < 22; ++row)
  {
    for(int column = 0; column < 6; ++column)
    {
      const float z = -11.0f + static_cast<float>(row) * (22.0f / 21.0f);
      const float x = -2.4f + static_cast<float>(column) * 0.96f;
      const float y = 0.8f + static_cast<float>((row * 6 + column) % 5) * 0.72f;

      // Intensity
      // Deterministic but strongly varying: an integer hash spread across two decades, so neighbouring emitters are not similar and the distribution is reproducible run to run.

      const int       hash      = (lightIndex * 2654435761u) % 97u;
      const float     intensity = 0.12f + static_cast<float>(hash) * 0.075f;
      const glm::vec3 tint(0.55f + 0.45f * static_cast<float>((hash + 0) % 7) / 6.0f, 0.55f + 0.45f * static_cast<float>((hash + 3) % 5) / 4.0f, 0.55f + 0.45f * static_cast<float>((hash + 5) % 11) / 10.0f);

      scene.models.push_back(MaterialModelEntry("Light " + std::to_string(lightIndex), "Models/Plane.glb", ComposeTransform(glm::vec3(x, y, z), glm::vec3(kHalfPi, 0.0f, 0.0f), glm::vec3(0.11f, 1.0f, 0.11f)), EmissiveMaterial(tint * intensity)));

      ++lightIndex;
    }
  }

  return scene;
}

// The reduced Sponza, scaled down and raised slightly.
SceneDefinition SponzaStudioScene()
{
  SceneDefinition scene {};

  scene.label = "Sponza Studio";
  scene.models.push_back(ModelEntry("Sponza Reduced", "Models/Sponza/SponzaReduced.gltf", ComposeTransform(glm::vec3(0.0f, 0.46f, 0.0f), glm::vec3(0.0f), glm::vec3(0.45f))));

  return scene;
}

}  // namespace

std::vector<SceneDefinition> CreateSceneCatalog()
{
  // Scene registry
  // Central scene registry. Add new scenes here and they automatically appear in ImGui.
  // The position in this list is the scene index, so existing entries must keep their order.

  std::vector<SceneDefinition> scenes;
  scenes.reserve(16);

  scenes.push_back(SingleModelScene("Area Light", "Models/AreaLight/AreaLight.gltf"));
  scenes.push_back(CornellScene("Cornell Box", false));
  scenes.push_back(CornellScene("Cornell Many Lights", true));
  scenes.push_back(DisocclusionScene());
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

  // Appended rather than grouped with the other room scenes on purpose: inserting it earlier would renumber every scene after it, and the scene index is what the headless harness and every recorded measurement refer to.
  scenes.push_back(ScatteredLightsScene());

  return scenes;
}

size_t FindDefaultSceneIndex(const std::vector<SceneDefinition>& scenes)
{
  // Prefer a stable material test scene on startup when present, trying labels in priority order.
  static constexpr std::array<const char*, 2> kPreferredDefaults = { "Bunny Dielectric", "Sponza" };

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

}  // namespace rtpt
