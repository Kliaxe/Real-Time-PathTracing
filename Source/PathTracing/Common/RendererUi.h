#pragma once

// Renderer-agnostic ImGui controls.
//
// These describe concepts every ray tracing renderer here shares - how a noisy
// frame is resolved, the denoiser settings, a bounce limit, the reservoir debug
// views - so they live in one place rather than being duplicated per renderer.
// Kept header-only and inline: they are small, and a translation unit of its own
// would add a build edge for no benefit.

#include <cstdint>

#include <imgui/imgui.h>

#include "PathTracing/Common/ResolveMode.h"
#include "Shaders/ShaderIo.h"

namespace nvsamples
{

inline bool DrawResolveModeControl(RenderResolveMode& resolveMode, const char* label = "Resolve")
{
  int mode = static_cast<int>(resolveMode);
  const char* resolveModes[] = {"Off", "Accumulate", "Denoise"};
  if(!ImGui::Combo(label, &mode, resolveModes, IM_ARRAYSIZE(resolveModes)))
  {
    return false;
  }

  resolveMode = static_cast<RenderResolveMode>(mode);
  return true;
}

inline bool DrawDenoiserDebugViewControl(DenoiserDebugView& debugView, const char* label = "Denoiser Output")
{
  int view = static_cast<int>(debugView);
  const char* debugViews[] = {"Final",          "Raw Beauty",      "Denoised Beauty", "Diffuse Input", "Specular Input",
                              "Denoised Diffuse", "Denoised Specular", "Normal Roughness", "ViewZ",         "Motion Vectors"};
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

  changed |= ImGui::SliderFloat("Diffuse Prepass Radius", &settings.diffusePrepassBlurRadius, 0.0f, 64.0f, "%.1f");
  changed |= ImGui::SliderFloat("Specular Prepass Radius", &settings.specularPrepassBlurRadius, 0.0f, 64.0f, "%.1f");
  changed |= ImGui::SliderFloat("Disocclusion Threshold", &settings.disocclusionThreshold, 0.001f, 0.20f, "%.3f");
  changed |= ImGui::SliderFloat("Max Blur Radius", &settings.maxBlurRadius, 0.0f, 60.0f, "%.1f");

  if(ImGui::TreeNodeEx("Hit Distance Normalization"))
  {
    // Scene-scale dependent. Raise A when transport is longer than a few units, or
    // every hit distance saturates and the denoiser blurs at its widest radius.
    changed |= ImGui::SliderFloat("Hit Distance A", &settings.hitDistanceA, 0.1f, 100.0f, "%.2f");
    changed |= ImGui::SliderFloat("Hit Distance B", &settings.hitDistanceB, 0.0f, 10.0f, "%.2f");
    changed |= ImGui::SliderFloat("Hit Distance C", &settings.hitDistanceC, 1.0f, 50.0f, "%.1f");
    ImGui::TextDisabled(
        "REBLUR divides hit distance by (A + |viewZ| * B) * lerp(C, 1, f(roughness)) and saturates. Anything longer normalizes to 1 and gets the widest blur.");
    ImGui::TreePop();
  }

  if(ImGui::Checkbox("Anti-Firefly", &settings.enableAntiFirefly))
  {
    changed = true;
  }

  ImGui::TreePop();
  return changed;
}

inline bool DrawBounceLimitControl(const char* label, uint32_t& settingValue, uint32_t bounceLimit)
{
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

inline void DrawReSTIRMethodFooter(const char* description, RenderResolveMode resolveMode, uint32_t accumulatedFrames)
{
  ImGui::TextWrapped("%s", description);
  DrawResolveStatus(resolveMode, accumulatedFrames);
}

}  // namespace nvsamples
