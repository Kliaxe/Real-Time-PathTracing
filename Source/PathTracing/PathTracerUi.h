#pragma once

// PathTracerUi
// Renderer section of the Settings window: the render mode combo and the control panel of whichever path tracer is active.
// The reference path tracer's panel is defined here, beside PathTracer. The ReSTIR PT Enhanced panel is composed here from the per-settings sections in ReSTIRPTUi, so both renderers reach the window through the same shape.
// Every function returns true when a control changed something that makes existing render history wrong, so the caller can invalidate it in one place.

#include "ApplicationOptions.h"
#include "PathTracing/PathTracer.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTRenderer.h"

namespace rtpt
{

// Draws the collapsing header, the mode combo, and the active renderer's controls. The rasterizer has no controls of its own.
bool DrawRendererSection(RenderMode& renderMode, PathTracer& pathTracer, ReSTIRPTRenderer& restirPT);

// Resolve mode, bounce limit, denoiser controls when the resolve mode uses the denoiser, and the resolve status.
bool DrawPathTracerControls(PathTracer& pathTracer);

// Every ReSTIRPTUi section for the renderer's settings, plus the denoiser controls when the resolve mode uses the denoiser.
bool DrawReSTIRPTControls(ReSTIRPTRenderer& restirPT);

}  // namespace rtpt
