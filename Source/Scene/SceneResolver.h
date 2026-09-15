#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

#include "SceneAssetCatalog.h"

namespace rtpt
{

// SceneSelectionInput
// The catalogs and the indices the UI or command line currently holds, which may be out of range.

struct SceneSelectionInput
{
  // Scene presets to choose from.
  const std::vector<SceneDefinition>& sceneDefinitions;

  // Requested scene index; clamped to 0 when out of range.
  size_t selectedSceneIndex = 0;

  // Discovered environment maps to choose from.
  const std::vector<AssetEntry>& hdriAssets;

  // Requested HDRI index; clamped to 0 when out of range.
  size_t selectedHdriIndex = 0;
};

// SceneSelectionOutput
// The validated selection, written back by the caller so the UI shows what is actually loaded.

struct SceneSelectionOutput
{
  // Scene index after clamping.
  size_t resolvedSceneIndex = 0;

  // HDRI index after clamping.
  size_t resolvedHdriIndex = 0;

  // Chosen scene, or null when the catalog is empty.
  const SceneDefinition* sceneDefinition = nullptr;

  // Content-relative HDRI path, or empty when no HDRI assets were discovered.
  std::optional<std::filesystem::path> hdriRelativePath;
};

// Chooses a stable scene and HDRI selection from UI indices and the discovered catalogs.
// This is a CPU-only step with no Vulkan code, so selection logic stays separate from GPU upload.
SceneSelectionOutput ResolveSceneSelection(const SceneSelectionInput& input);

}  // namespace rtpt
