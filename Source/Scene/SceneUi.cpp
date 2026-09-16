#include "Scene/SceneUi.h"

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include "Framework/Presentation/UiControls.h"

namespace rtpt
{

void DrawSceneAssetsSection(const std::vector<SceneDefinition>& sceneDefinitions, size_t& selectedSceneIndex, bool& sceneReloadRequested, const std::vector<AssetEntry>& hdriAssets, size_t& selectedHdriIndex, bool& hdriReloadRequested)
{
  const bool open = ImGui::CollapsingHeader("Assets");

  DrawTooltip("Scene and environment map to load. Either switch rebuilds GPU resources and discards all render history.");

  if(!open) return;

  // Scene
  // The catalog can be empty until discovery has run, so the preview label falls back instead of indexing.

  const char* sceneLabel = sceneDefinitions.empty() ? "<none>" : sceneDefinitions[selectedSceneIndex].label.c_str();

  if(ImGui::BeginCombo("Scene", sceneLabel))
  {
    for(size_t index = 0; index < sceneDefinitions.size(); ++index)
    {
      if(ImGui::Selectable(sceneDefinitions[index].label.c_str(), index == selectedSceneIndex))
      {
        selectedSceneIndex   = index;
        sceneReloadRequested = true;
      }
    }

    ImGui::EndCombo();
  }

  DrawTooltip("Built-in test scene to load. Switching rebuilds geometry, materials and acceleration structures, which stalls for a moment and discards all accumulated or denoised history.");

  // HDRI
  // The combo is only offered when discovery found environment maps; without any there is no index to show.

  if(!hdriAssets.empty() && ImGui::BeginCombo("HDRI", hdriAssets[selectedHdriIndex].label.c_str()))
  {
    for(size_t index = 0; index < hdriAssets.size(); ++index)
    {
      if(ImGui::Selectable(hdriAssets[index].label.c_str(), index == selectedHdriIndex))
      {
        selectedHdriIndex   = index;
        hdriReloadRequested = true;
      }
    }

    ImGui::EndCombo();
  }

  DrawTooltip("Environment map used for image-based lighting and as the background. Switching reloads the texture and rebuilds its importance sampling tables, and only has an effect while Use HDRI is on.");
}

bool DrawSceneEnvironmentSection(shaderio::GltfSceneInfo& scene)
{
  const bool open = ImGui::CollapsingHeader("Environment");

  DrawTooltip("Where the light that is not emitted by scene geometry comes from: an environment map, a procedural sky, or a constant background with one point light.");

  if(!open) return false;

  // Sources
  // The scene info stores flags as integers to match the shader layout, so the checkboxes edit local bools.

  bool changed = false;

  bool hdri = scene.useHdrEnv != 0;
  bool sky  = scene.useSky != 0;

  if(ImGui::Checkbox("Use HDRI", &hdri)) { scene.useHdrEnv = hdri ? 1 : 0; changed = true; }

  DrawTooltip("Lights the scene with the loaded environment map. Rays that escape the scene read their radiance from it, and the path tracers importance sample it. Off falls back to the sky, or to the constant background below.");

  if(ImGui::Checkbox("Use Sky", &sky)) { scene.useSky = sky ? 1 : 0; changed = true; }

  DrawTooltip("Replaces the environment with a procedural sky and sun. Cheap and easy to steer, and it takes precedence over the constant background, but it has no image detail to reflect.");

  // Lighting
  // Sky controls appear while the sky is on; background and punctual light controls only when both sky and HDRI are off, because that is the only case the renderers read them.

  if(sky)
  {
    changed |= DrawFloat3("Sun Direction", scene.skySimpleParam.sunDirection, 0.01F, -1.0F, 1.0F);
    DrawTooltip("World-space direction of the sun. It is normalized in the shader, so only its orientation matters.");

    changed |= ImGui::ColorEdit3("Sun Color", glm::value_ptr(scene.skySimpleParam.sunColor));
    DrawTooltip("Tint of the sun disc and of the direct sunlight, multiplied by the intensity below.");

    changed |= ImGui::SliderFloat("Sun Intensity", &scene.skySimpleParam.sunIntensity, 0.0F, 100.0F);
    DrawTooltip("Brightness of the sun relative to the sky. High values give hard shadows and a wide dynamic range, which is the case resampling and denoising find hardest.");

    changed |= ImGui::ColorEdit3("Sky Color", glm::value_ptr(scene.skySimpleParam.skyColor));
    DrawTooltip("Color of the sky dome away from the sun. It acts as the scene's ambient fill light.");

    changed |= ImGui::SliderFloat("Sky Brightness", &scene.skySimpleParam.brightness, 0.0F, 10.0F);
    DrawTooltip("Overall multiplier on the sky dome. Lowering it toward zero leaves the sun as the only light and makes shadowed regions noisier.");
  }
  else if(!hdri)
  {
    changed |= ImGui::ColorEdit3("Background", glm::value_ptr(scene.backgroundColor));
    DrawTooltip("Constant radiance seen by escaping rays when neither the HDRI nor the sky is on. It lights the scene uniformly from every direction.");

    changed |= DrawFloat3("Light Position", scene.punctualLights[0].position, 0.05F, -100.0F, 100.0F);
    DrawTooltip("World-space position of the single point light used in this mode.");

    changed |= ImGui::SliderFloat("Light Intensity", &scene.punctualLights[0].intensity, 0.0F, 1000.0F, "%.2f", ImGuiSliderFlags_Logarithmic);
    DrawTooltip("Radiant power of the point light. Its falloff is inverse-square, so the useful range spans orders of magnitude and the slider is logarithmic.");

    changed |= ImGui::ColorEdit3("Light Color", glm::value_ptr(scene.punctualLights[0].color));
    DrawTooltip("Tint of the point light, multiplied by its intensity.");
  }

  return changed;
}

bool DrawSceneMaterialOverrideSection(MaterialDebugOverride& settings)
{
  const bool open = ImGui::CollapsingHeader("Material Override");

  DrawTooltip("Debug override that forces every material in the scene to one metallic and roughness, so a single material response can be studied without editing the assets.");

  if(!open) return false;

  // Debug override
  // Each value slider is disabled while its channel is off, so it cannot report a change that would restart the render for nothing.

  bool changed = false;

  changed |= ImGui::Checkbox("Override Metallic", &settings.overrideMetallic);
  DrawTooltip("Replaces the metalness of every material in the scene, textures included, with the value below. It applies to the rasterizer preview and both path tracers, so all three shade the same material.");

  ImGui::BeginDisabled(!settings.overrideMetallic);
  changed |= ImGui::SliderFloat("Metallic", &settings.metallic, 0.0F, 1.0F, "%.3f");
  ImGui::EndDisabled();

  DrawTooltip("0 is a dielectric, whose base color drives a diffuse response; 1 is a conductor, which has no diffuse lobe and tints its reflection with the base color. Values in between are a blend rather than a real material.");

  changed |= ImGui::Checkbox("Override Roughness", &settings.overrideRoughness);
  DrawTooltip("Replaces the perceptual roughness of every material in the scene, textures included, with the value below.");

  ImGui::BeginDisabled(!settings.overrideRoughness);
  changed |= ImGui::SliderFloat("Roughness", &settings.roughness, 0.0F, 1.0F, "%.3f");
  ImGui::EndDisabled();

  DrawTooltip("0 is a mirror and 1 is fully diffuse. Low roughness is the hard case for ReSTIR: a reservoir can only be reused where the lobes agree, so a near-mirror scene falls back to the path each pixel traced itself.");

  return changed;
}

}  // namespace rtpt
