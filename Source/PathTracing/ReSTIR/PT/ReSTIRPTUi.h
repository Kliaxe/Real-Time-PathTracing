#pragma once

// ReSTIRPTUi
// ImGui panel for the ReSTIR PT Enhanced renderer.
// Extracted from Application so the conductor does not accumulate one renderer's control surface.
// The panel is organized by paper section: each Enhanced technique can be toggled on its own and its effect observed in isolation, which is what makes the paper's ablation reproducible from the UI.
// Every function returns true when a control changed, so the caller can invalidate accumulation and reuse history in one place.

#include <cstdint>

#include "PathTracing/ReSTIR/PT/ReSTIRPTSettings.h"

namespace rtpt
{

bool DrawReSTIRPTCommonControls(ReSTIRPTCommonSettings& settings);

bool DrawReSTIRPTInitialSamplingSection(ReSTIRPTInitialSamplingParameters& settings, uint32_t bounceLimit);

bool DrawReSTIRPTShiftSection(ReSTIRPTShiftParameters& settings);

bool DrawReSTIRPTResamplingSection(ReSTIRPTTemporalResamplingParameters& temporalSettings, ReSTIRPTSpatialResamplingParameters& spatialSettings, float maxRadius);

bool DrawReSTIRPTDecorrelationSection(ReSTIRPTDecorrelationParameters& settings);

bool DrawReSTIRPTShadingSection(ReSTIRPTShadingParameters& settings);

bool DrawReSTIRPTNeeSection(ReSTIRPTNeeParameters& settings);

}  // namespace rtpt
