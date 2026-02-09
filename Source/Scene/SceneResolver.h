#pragma once

// Role:
// CPU-only selection step that resolves valid scene/HDRI choices from UI indices.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

#include "SceneAssetCatalog.h"

namespace nvsamples
{

// SceneResolver is a CPU-only step that chooses a stable scene/HDRI selection
// from UI indices and discovered catalogs. It intentionally has no Vulkan code.
class SceneResolver
{
public:
  struct Input
  {
    const std::vector<SceneDefinition>& sceneDefinitions;
    size_t                              selectedSceneIndex = 0;
    const std::vector<AssetEntry>&      hdriAssets;
    size_t                              selectedHdriIndex = 0;
  };

  struct Output
  {
    size_t                           resolvedSceneIndex = 0;
    size_t                           resolvedHdriIndex  = 0;
    const SceneDefinition*           sceneDefinition    = nullptr;
    std::optional<std::filesystem::path> hdriRelativePath;
  };

  Output Resolve(const Input& input) const;
};

}  // namespace nvsamples
