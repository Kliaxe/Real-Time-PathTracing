#include "PathTracing/PathTracerUi.h"

#include <iterator>

#include <imgui.h>

#include "Framework/Presentation/UiControls.h"
#include "PathTracing/Common/RendererUi.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTUi.h"

namespace rtpt
{

bool DrawRendererSection(RenderMode& renderMode, PathTracer& pathTracer, ReSTIRPTRenderer& restirPT)
{
  const bool open = ImGui::CollapsingHeader("Renderer", ImGuiTreeNodeFlags_DefaultOpen);

  DrawTooltip("Which renderer draws the scene, and its settings. The three modes exist to be compared against each other, so they share the resolve and bounce controls wherever the concept is the same.");

  if(!open) return false;

  // Mode
  // Combo indices follow the RenderMode declaration order. Switching renderer discards history so the new renderer starts clean.

  bool changed = false;

  int mode = static_cast<int>(renderMode);

  constexpr const char* modes[] = { "Rasterizer", "Path Tracing", "ReSTIR PT Enhanced" };

  if(ImGui::Combo("Mode", &mode, modes, std::size(modes)))
  {
    renderMode = static_cast<RenderMode>(mode);
    changed    = true;
  }

  DrawTooltip("Which renderer fills the Display window.\n\nRasterizer is a cheap environment-lit preview, there to show the scene without ray tracing; it is not a reference for anything.\n\nPath Tracing is the brute-force reference: correct by construction and far too noisy to use at one sample per pixel.\n\nReSTIR PT Enhanced reuses paths across frames and neighbouring pixels to get close to that reference in real time, and is what the rest of this panel is about.");

  // Renderer controls
  // Each renderer owns its settings, so only the active one is drawn.

  if(renderMode == RenderMode::ePathTracing)
  {
    changed |= DrawPathTracerControls(pathTracer);
  }
  else if(renderMode == RenderMode::eReSTIRPTEnhanced)
  {
    changed |= DrawReSTIRPTControls(restirPT);
  }

  return changed;
}

bool DrawPathTracerControls(PathTracer& pathTracer)
{
  PathTracer::Settings& settings = pathTracer.GetSettings();

  bool changed = DrawResolveModeControl(settings.resolveMode, pathTracer.IsRayReconstructionAvailable(), pathTracer.GetRayReconstructionUnavailableReason());

  int bounces = static_cast<int>(settings.maxBounces);

  // The slider stops at the path tracer's fixed bounce limit.
  if(ImGui::SliderInt("Max Bounces", &bounces, 1, static_cast<int>(pathTracer.GetBounceLimit())))
  {
    settings.maxBounces = static_cast<uint32_t>(bounces);
    changed             = true;
  }

  DrawTooltip("How many times a path may bounce before it is cut off. One bounce is direct lighting only; each further bounce adds a round of indirect light and costs roughly a ray per pixel. Keep it equal to the ReSTIR bounce limit when comparing the two renderers.");

  // Denoiser controls only appear for the denoiser the resolve mode uses.
  if(IsNrdResolveMode(settings.resolveMode))
  {
    changed |= DrawDenoiserDebugViewControl(settings.denoiserDebugView);
    changed |= DrawDenoiserSettingsSection("Denoiser Settings", settings.denoiserSettings);
  }

  if(IsRayReconstructionResolveMode(settings.resolveMode))
  {
    changed |= DrawRayReconstructionSettingsSection("Ray Reconstruction Settings", settings.rayReconstructionSettings);
  }

  DrawResolveStatus(settings.resolveMode, pathTracer.GetAccumulatedFrameCount());

  return changed;
}

bool DrawReSTIRPTControls(ReSTIRPTRenderer& restirPT)
{
  ReSTIRPTSettings& settings = restirPT.GetSettings();

  // Sections
  // The order follows the paper: sampling, shift, resampling, decorrelation, shading, NEE. The spatial radius cap matches what the shaders were written against.

  bool changed = DrawReSTIRPTCommonControls(settings.common, restirPT.IsRayReconstructionAvailable(), restirPT.GetRayReconstructionUnavailableReason());

  changed |= DrawReSTIRPTInitialSamplingSection(settings.initialSampling, restirPT.GetBounceLimit());
  changed |= DrawReSTIRPTShiftSection(settings.shift);
  changed |= DrawReSTIRPTResamplingSection(settings.temporalResampling, settings.spatialResampling, 128.0F);
  changed |= DrawReSTIRPTDecorrelationSection(settings.decorrelation);
  changed |= DrawReSTIRPTShadingSection(settings.shading);
  changed |= DrawReSTIRPTNeeSection(settings.nee);

  // Denoiser controls only appear for the denoiser the resolve mode uses. The label suffixes keep the tree nodes distinct from the reference path tracer's.
  if(IsNrdResolveMode(settings.common.resolveMode))
  {
    changed |= DrawDenoiserDebugViewControl(settings.common.denoiserDebugView);
    changed |= DrawDenoiserSettingsSection("Denoiser Settings##ReSTIR", settings.common.denoiserSettings);
  }

  if(IsRayReconstructionResolveMode(settings.common.resolveMode))
  {
    changed |= DrawRayReconstructionSettingsSection("Ray Reconstruction Settings##ReSTIR", settings.common.rayReconstructionSettings);
  }

  return changed;
}

}  // namespace rtpt
