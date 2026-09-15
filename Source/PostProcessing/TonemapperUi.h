#pragma once

// TonemapperUi
// Tonemapper section of the Settings window.
// Lives next to Tonemapper rather than in Application. Edits never report history invalidation because tonemapping runs after the renderers and touches no history.

#include "PostProcessing/Tonemapper.h"

namespace rtpt
{

void DrawTonemapperSection(TonemapperSettings& settings);

}  // namespace rtpt
