#pragma once

#include <cstddef>
#include <vector>

#include "SceneTypes.h"

namespace nvsamples
{

// Returns all scene presets displayed in ImGui.
std::vector<SceneDefinition> CreateSceneCatalog();

// Picks the initial scene shown on startup.
size_t FindDefaultSceneIndex(const std::vector<SceneDefinition>& scenes);

}  // namespace nvsamples
