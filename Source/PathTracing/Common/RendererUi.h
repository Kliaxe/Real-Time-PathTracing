#pragma once

// RendererUi
// Renderer-agnostic ImGui controls.
// These describe concepts every ray tracing renderer here shares - how a noisy frame is resolved, the denoiser settings, a bounce limit, the resolve status - so they live in one place rather than being duplicated per renderer.
// Kept header-only and inline: they are small, and a translation unit of their own would add a build edge for no benefit.
// Every control returns true when the user changed its value, so the caller can invalidate history in one place.

#include <cstdint>

#include <imgui.h>

#include "PathTracing/Common/ResolveMode.h"
#include "Shaders/ShaderIo.h"

namespace rtpt
{

inline bool DrawResolveModeControl(RenderResolveMode& resolveMode, const char* label = "Resolve")
{
  // Item order must match the RenderResolveMode enumerators, because the combo index is cast straight back to the enum.
  int mode = static_cast<int>(resolveMode);
  const char* resolveModes[] = { "Off", "Accumulate", "Denoise" };

  if(!ImGui::Combo(label, &mode, resolveModes, IM_ARRAYSIZE(resolveModes)))
  {
    return false;
  }

  resolveMode = static_cast<RenderResolveMode>(mode);
  return true;
}

inline bool DrawDenoiserDebugViewControl(DenoiserDebugView& debugView, const char* label = "Denoiser Output")
{
  // Item order must match the DenoiserDebugView enumerators, because the combo index is cast straight back to the enum.
  int view = static_cast<int>(debugView);
  const char* debugViews[] = { "Final",          "Raw Beauty",      "Denoised Beauty", "Diffuse Input", "Specular Input",
                               "Denoised Diffuse", "Denoised Specular", "Normal Roughness", "ViewZ",         "Motion Vectors" };

  if(!ImGui::Combo(label, &view, debugViews, IM_ARRAYSIZE(debugViews)))
  {
    return false;
  }

  debugView = static_cast<DenoiserDebugView>(view);
  return true;
}

inline bool DrawDenoiserSettingsSection(const char* treeLabel, DenoiserSettings& settings)
{
  if(!ImGui::TreeNodeEx(treeLabel))
  {
    return false;
  }

  bool changed = false;

  // History lengths
  // ImGui sliders edit int, so each value round-trips through a local.
  // The fast history is bounded by the full history length.

  int maxAccumulatedFrames = static_cast<int>(settings.maxAccumulatedFrames);

  if(ImGui::SliderInt("Max Accumulated Frames", &maxAccumulatedFrames, 1, 63))
  {
    settings.maxAccumulatedFrames = static_cast<uint32_t>(maxAccumulatedFrames);
    changed = true;
  }

  int maxFastAccumulatedFrames = static_cast<int>(settings.maxFastAccumulatedFrames);

  if(ImGui::SliderInt("Max Fast Accumulated Frames", &maxFastAccumulatedFrames, 0, static_cast<int>(settings.maxAccumulatedFrames)))
  {
    settings.maxFastAccumulatedFrames = static_cast<uint32_t>(maxFastAccumulatedFrames);
    changed = true;
  }

  // Spatial filtering

  changed |= ImGui::SliderFloat("Diffuse Prepass Radius", &settings.diffusePrepassBlurRadius, 0.0f, 64.0f, "%.1f");
  changed |= ImGui::SliderFloat("Specular Prepass Radius", &settings.specularPrepassBlurRadius, 0.0f, 64.0f, "%.1f");
  changed |= ImGui::SliderFloat("Disocclusion Threshold", &settings.disocclusionThreshold, 0.001f, 0.20f, "%.3f");
  changed |= ImGui::SliderFloat("Max Blur Radius", &settings.maxBlurRadius, 0.0f, 60.0f, "%.1f");

  // Hit distance normalization
  // Scene-scale dependent. Raise A when transport is longer than a few units, or every hit distance saturates and the denoiser blurs at its widest radius.

  if(ImGui::TreeNodeEx("Hit Distance Normalization"))
  {
    changed |= ImGui::SliderFloat("Hit Distance A", &settings.hitDistanceA, 0.1f, 100.0f, "%.2f");
    changed |= ImGui::SliderFloat("Hit Distance B", &settings.hitDistanceB, 0.0f, 10.0f, "%.2f");
    changed |= ImGui::SliderFloat("Hit Distance C", &settings.hitDistanceC, 1.0f, 50.0f, "%.1f");
    ImGui::TextDisabled("REBLUR divides hit distance by (A + |viewZ| * B) * lerp(C, 1, f(roughness)) and saturates. Anything longer normalizes to 1 and gets the widest blur.");
    ImGui::TreePop();
  }

  // Firefly suppression

  if(ImGui::Checkbox("Anti-Firefly", &settings.enableAntiFirefly))
  {
    changed = true;
  }

  ImGui::TreePop();
  return changed;
}

inline bool DrawBounceLimitControl(const char* label, uint32_t& settingValue, uint32_t bounceLimit)
{
  // The slider range stops at the pipeline's bounce limit, so the UI cannot request more recursion than the device supports.
  int value = static_cast<int>(settingValue);

  if(!ImGui::SliderInt(label, &value, 0, static_cast<int>(bounceLimit)))
  {
    return false;
  }

  settingValue = static_cast<uint32_t>(value);
  return true;
}

inline void DrawResolveStatus(RenderResolveMode resolveMode, uint32_t accumulatedFrames)
{
  if(IsAccumulationResolveMode(resolveMode))
  {
    ImGui::Text("Accumulated Frames: %u", accumulatedFrames);
    return;
  }

  if(IsDenoiseResolveMode(resolveMode))
  {
    ImGui::TextUnformatted("Resolve Mode: Denoise");
    return;
  }

  ImGui::TextUnformatted("Resolve Mode: Off");
}

}  // namespace rtpt
