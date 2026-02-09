#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "SceneTypes.h"

namespace nvsamples
{

// Generic discovered asset entry used by scene/HDRI UI combos.
struct AssetEntry
{
  std::string           label;
  std::filesystem::path relativePath;
};

// Fully resolved catalog data returned to runtime systems.
// RtFoundation consumes this as immutable setup data after discovery.
struct SceneAssetCatalogData
{
  std::vector<AssetEntry>       modelAssets;
  std::vector<AssetEntry>       hdriAssets;
  std::vector<SceneDefinition>  sceneDefinitions;
  size_t                        selectedSceneIndex = 0;
  size_t                        selectedHdriIndex  = 0;
  std::vector<std::string>      warnings;
};

// Discovers runtime assets and scene presets in one place.
// This keeps Main.cpp focused on orchestration/UI instead of file scanning logic.
class SceneAssetCatalog
{
public:
  SceneAssetCatalog() = default;

  SceneAssetCatalogData Discover() const;
};

}  // namespace nvsamples

