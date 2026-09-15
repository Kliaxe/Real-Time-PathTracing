#include "PathTracing/PathTracerUi.h"

#include <iterator>

#include <imgui.h>

#include "PathTracing/Common/RendererUi.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTUi.h"

namespace rtpt
{

bool DrawRendererSection(RenderMode& renderMode, PathTracer& pathTracer, ReSTIRPTRenderer& restirPT)
{
  if(!ImGui::CollapsingHeader("Renderer", ImGuiTreeNodeFlags_DefaultOpen)) return false;

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

  bool changed = DrawResolveModeControl(settings.resolveMode);

  int bounces = static_cast<int>(settings.maxBounces);

  // The slider stops at the bounce limit the path tracer's pipeline reports.
  if(ImGui::SliderInt("Max Bounces", &bounces, 1, static_cast<int>(pathTracer.GetPipelineBounceLimit())))
  {
    settings.maxBounces = static_cast<uint32_t>(bounces);
    changed             = true;
  }

  // Denoiser controls only appear when the resolve mode uses the denoiser.
  if(IsDenoiseResolveMode(settings.resolveMode))
  {
    changed |= DrawDenoiserDebugViewControl(settings.denoiserDebugView);
    changed |= DrawDenoiserSettingsSection("Denoiser Settings", settings.denoiserSettings);
  }

  DrawResolveStatus(settings.resolveMode, pathTracer.GetAccumulatedFrameCount());

  return changed;
}

bool DrawReSTIRPTControls(ReSTIRPTRenderer& restirPT)
{
  ReSTIRPTSettings& settings = restirPT.GetSettings();

  // Sections
  // The order follows the paper: sampling, shift, resampling, decorrelation, shading, NEE. The spatial radius cap matches what the shaders were written against.

  bool changed = DrawReSTIRPTCommonControls(settings.common);

  changed |= DrawReSTIRPTInitialSamplingSection(settings.initialSampling, restirPT.GetPipelineBounceLimit());
  changed |= DrawReSTIRPTShiftSection(settings.shift);
  changed |= DrawReSTIRPTResamplingSection(settings.temporalResampling, settings.spatialResampling, 128.0F);
  changed |= DrawReSTIRPTDecorrelationSection(settings.decorrelation);
  changed |= DrawReSTIRPTShadingSection(settings.shading);
  changed |= DrawReSTIRPTNeeSection(settings.nee);

  // Denoiser controls only appear when the resolve mode uses the denoiser. The label suffix keeps the tree node distinct from the reference path tracer's.
  if(IsDenoiseResolveMode(settings.common.resolveMode))
  {
    changed |= DrawDenoiserDebugViewControl(settings.common.denoiserDebugView);
    changed |= DrawDenoiserSettingsSection("Denoiser Settings##ReSTIR", settings.common.denoiserSettings);
  }

  return changed;
}

}  // namespace rtpt
