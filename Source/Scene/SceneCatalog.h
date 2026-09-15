#pragma once

#include <cstddef>
#include <vector>

#include "SceneTypes.h"

namespace rtpt
{

// Returns all scene presets displayed in ImGui. The order is stable because scene indices are referenced externally.
std::vector<SceneDefinition> CreateSceneCatalog();

// Picks the initial scene shown on startup.
size_t FindDefaultSceneIndex(const std::vector<SceneDefinition>& scenes);

}  // namespace rtpt
