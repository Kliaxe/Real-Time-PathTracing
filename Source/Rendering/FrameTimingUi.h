#pragma once

#include "Rendering/FrameTimingStatistics.h"

namespace rtpt
{

// Draws the Profiler section of the Settings window: CPU frame time and each GPU scope's mean and 95th percentile over the rolling window.
// profilerAvailable is false when the device cannot record timestamps, in which case only CPU frame time is shown.
void DrawFrameTimingSection(const FrameTimingStatistics& statistics, bool profilerAvailable);

}  // namespace rtpt
