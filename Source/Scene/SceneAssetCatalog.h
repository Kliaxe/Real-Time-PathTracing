#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "SceneTypes.h"

namespace rtpt
{

// AssetEntry
// One discovered file under a content directory, shown in the scene and HDRI UI combos.

struct AssetEntry
{
  // Display text: the content-relative path with forward slashes.
  std::string label;

  // Path relative to the content directory it was found in, so it resolves against any content root.
  std::filesystem::path relativePath;
};

// SceneAssetCatalogData
// Fully resolved catalog data returned to runtime systems.
// The runtime consumes this as immutable setup data after discovery.

struct SceneAssetCatalogData
{
  // glTF and GLB files found under Models/, sorted by label.
  std::vector<AssetEntry> modelAssets;

  // HDR and EXR files found under HDRI/, sorted by label.
  std::vector<AssetEntry> hdriAssets;

  // Built-in scene presets from CreateSceneCatalog.
  std::vector<SceneDefinition> sceneDefinitions;

  // Index into sceneDefinitions of the scene to show first.
  size_t selectedSceneIndex = 0;

  // Index into hdriAssets of the environment to show first.
  size_t selectedHdriIndex = 0;

  // Human-readable problems found during discovery, such as scenes referencing missing models.
  std::vector<std::string> warnings;
};

// Discovers runtime assets and scene presets in one place.
// This keeps the application focused on orchestration and UI instead of file scanning logic.
SceneAssetCatalogData DiscoverSceneAssets();

}  // namespace rtpt
