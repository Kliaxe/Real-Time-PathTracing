#pragma once

// SceneUi
// Assets, Environment, and Material Override sections of the Settings window.
// Lives next to the scene systems rather than in Application, which only sequences subsystems and calls into panels like this one.
// Scene and HDRI switches are only requested here: a rebuild drains the GPU and replaces scene resources, so the application performs it before the next frame starts recording.

#include <cstddef>
#include <vector>

#include "Scene/SceneAssetCatalog.h"
#include "Scene/SceneTypes.h"
#include "Shaders/ShaderIo.h"

namespace rtpt
{

// Scene and HDRI combos. Picking an entry writes the new index and raises the matching reload request; the rebuild discards history itself, so nothing is returned.
void DrawSceneAssetsSection(const std::vector<SceneDefinition>& sceneDefinitions, size_t& selectedSceneIndex, bool& sceneReloadRequested, const std::vector<AssetEntry>& hdriAssets, size_t& selectedHdriIndex, bool& hdriReloadRequested);

// Sky, background, and punctual light controls. Returns true when a control changed, since every one of them changes lighting and so invalidates render history.
bool DrawSceneEnvironmentSection(shaderio::GltfSceneInfo& scene);

// Debug override of every material's metallic and roughness. It edits the settings only; copying them into the scene uniform is the caller's job.
// Returns true when a control changed, because an override changes what every surface reflects and so invalidates accumulated and denoised history.
bool DrawSceneMaterialOverrideSection(MaterialDebugOverride& settings);

}  // namespace rtpt
