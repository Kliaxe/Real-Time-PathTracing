#include "Scene/SceneUi.h"

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include "Framework/Presentation/UiControls.h"

namespace rtpt
{

void DrawSceneAssetsSection(const std::vector<SceneDefinition>& sceneDefinitions, size_t& selectedSceneIndex, bool& sceneReloadRequested, const std::vector<AssetEntry>& hdriAssets, size_t& selectedHdriIndex, bool& hdriReloadRequested)
{
  if(!ImGui::CollapsingHeader("Assets")) return;

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
}

bool DrawSceneEnvironmentSection(shaderio::GltfSceneInfo& scene)
{
  if(!ImGui::CollapsingHeader("Environment")) return false;

  // Sources
  // The scene info stores flags as integers to match the shader layout, so the checkboxes edit local bools.

  bool changed = false;

  bool hdri = scene.useHdrEnv != 0;
  bool sky  = scene.useSky != 0;

  if(ImGui::Checkbox("Use HDRI", &hdri)) { scene.useHdrEnv = hdri ? 1 : 0; changed = true; }
  if(ImGui::Checkbox("Use Sky", &sky)) { scene.useSky = sky ? 1 : 0; changed = true; }

  // Lighting
  // Sky controls appear while the sky is on; background and punctual light controls only when both sky and HDRI are off, because that is the only case the renderers read them.

  if(sky)
  {
    changed |= DrawFloat3("Sun Direction", scene.skySimpleParam.sunDirection, 0.01F, -1.0F, 1.0F);
    changed |= ImGui::ColorEdit3("Sun Color", glm::value_ptr(scene.skySimpleParam.sunColor));
    changed |= ImGui::SliderFloat("Sun Intensity", &scene.skySimpleParam.sunIntensity, 0.0F, 100.0F);
    changed |= ImGui::ColorEdit3("Sky Color", glm::value_ptr(scene.skySimpleParam.skyColor));
    changed |= ImGui::SliderFloat("Sky Brightness", &scene.skySimpleParam.brightness, 0.0F, 10.0F);
  }
  else if(!hdri)
  {
    changed |= ImGui::ColorEdit3("Background", glm::value_ptr(scene.backgroundColor));
    changed |= DrawFloat3("Light Position", scene.punctualLights[0].position, 0.05F, -100.0F, 100.0F);
    changed |= ImGui::SliderFloat("Light Intensity", &scene.punctualLights[0].intensity, 0.0F, 1000.0F, "%.2f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::ColorEdit3("Light Color", glm::value_ptr(scene.punctualLights[0].color));
  }

  return changed;
}

}  // namespace rtpt
