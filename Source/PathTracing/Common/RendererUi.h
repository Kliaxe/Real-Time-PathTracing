#pragma once

// RendererUi
// Renderer-agnostic ImGui controls.
// These describe concepts every ray tracing renderer here shares - how a noisy frame is resolved, the denoiser settings, a bounce limit, the resolve status - so they live in one place rather than being duplicated per renderer.
// Kept header-only and inline: they are small, and a translation unit of their own would add a build edge for no benefit.
// Every control returns true when the user changed its value, so the caller can invalidate history in one place.
// Each control carries a hover explanation, because none of these are values a reader can infer from the label alone.

#include <cstdint>

#include <imgui.h>

#include "Framework/Presentation/UiControls.h"
#include "PathTracing/Common/ResolveMode.h"
#include "Shaders/ShaderIo.h"

namespace rtpt
{

inline bool DrawResolveModeControl(RenderResolveMode& resolveMode, const char* label = "Resolve")
{
  // Item order must match the RenderResolveMode enumerators, because the combo index is cast straight back to the enum.
  int mode = static_cast<int>(resolveMode);
  const char* resolveModes[] = { "Off", "Accumulate", "Denoise" };

  const bool changed = ImGui::Combo(label, &mode, resolveModes, IM_ARRAYSIZE(resolveModes));

  DrawTooltip("What happens to the noisy frame the renderer just produced.\n\nOff shows it raw, which is the honest view of how much noise a single frame carries.\n\nAccumulate averages every frame since the camera last moved, and converges to the ground truth. Use it to compare renderers.\n\nDenoise runs NRD REBLUR on this frame using its history, which is the real-time path: it stays responsive while the camera moves, at the cost of some detail.");

  if(!changed)
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

  const bool changed = ImGui::Combo(label, &view, debugViews, IM_ARRAYSIZE(debugViews));

  DrawTooltip("Which stage of the denoiser is shown. Final is the composed image; the others show what NRD was given and what it returned, which is how a denoising artifact is traced back to the input that caused it - a wrong normal, a bad motion vector, or genuinely missing radiance.");

  if(!changed)
  {
    return false;
  }

  debugView = static_cast<DenoiserDebugView>(view);
  return true;
}

inline bool DrawDenoiserSettingsSection(const char* treeLabel, DenoiserSettings& settings)
{
  const bool open = ImGui::TreeNodeEx(treeLabel);

  DrawTooltip("NRD REBLUR parameters. They control how much history the denoiser trusts and how wide it is allowed to blur.");

  if(!open)
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

  DrawTooltip("How many past frames the denoiser may blend into a pixel. More history means less noise and more lag behind moving light and moving geometry.");

  int maxFastAccumulatedFrames = static_cast<int>(settings.maxFastAccumulatedFrames);

  if(ImGui::SliderInt("Max Fast Accumulated Frames", &maxFastAccumulatedFrames, 0, static_cast<int>(settings.maxAccumulatedFrames)))
  {
    settings.maxFastAccumulatedFrames = static_cast<uint32_t>(maxFastAccumulatedFrames);
    changed = true;
  }

  DrawTooltip("Length of the second, shorter history REBLUR keeps alongside the first. Comparing the two is how it notices that lighting has changed and discards the stale long history, so a lower value reacts faster and is noisier.");

  // Spatial filtering

  changed |= ImGui::SliderFloat("Diffuse Prepass Radius", &settings.diffusePrepassBlurRadius, 0.0f, 64.0f, "%.1f");
  DrawTooltip("Blur applied to the diffuse signal before temporal accumulation, in pixels. It removes the worst fireflies so they never enter the history, at the cost of contact detail.");

  changed |= ImGui::SliderFloat("Specular Prepass Radius", &settings.specularPrepassBlurRadius, 0.0f, 64.0f, "%.1f");
  DrawTooltip("The same prepass for the specular signal. Keep it lower than the diffuse one: specular detail is what the eye reads as a reflection.");

  changed |= ImGui::SliderFloat("Disocclusion Threshold", &settings.disocclusionThreshold, 0.001f, 0.20f, "%.3f");
  DrawTooltip("How far reprojected depth may disagree before history is thrown away, as a fraction of view depth. Too low re-noises every silhouette; too high drags stale lighting across edges as a ghost.");

  changed |= ImGui::SliderFloat("Max Blur Radius", &settings.maxBlurRadius, 0.0f, 60.0f, "%.1f");
  DrawTooltip("Upper bound on the spatial filter, in pixels. It is what a pixel with almost no history falls back on, so it sets how smeared a disoccluded region looks.");

  // Hit distance normalization
  // Scene-scale dependent. Raise A when transport is longer than a few units, or every hit distance saturates and the denoiser blurs at its widest radius.

  const bool hitDistanceOpen = ImGui::TreeNodeEx("Hit Distance Normalization");

  DrawTooltip("How the distance to the first bounce is mapped into [0, 1] before REBLUR reads it. This is the one group of settings that depends on the size of the scene.");

  if(hitDistanceOpen)
  {
    changed |= ImGui::SliderFloat("Hit Distance A", &settings.hitDistanceA, 0.1f, 100.0f, "%.2f");
    DrawTooltip("Constant term, in world units. Set it to the scale of the scene: it is the distance that maps to roughly half the range.");

    changed |= ImGui::SliderFloat("Hit Distance B", &settings.hitDistanceB, 0.0f, 10.0f, "%.2f");
    DrawTooltip("View-depth term. Above zero, distant geometry normalizes more generously than near geometry, which keeps a large scene from saturating in the background.");

    changed |= ImGui::SliderFloat("Hit Distance C", &settings.hitDistanceC, 1.0f, 50.0f, "%.1f");
    DrawTooltip("Roughness term. Rough surfaces get their hit distances scaled up by this factor, because their lighting is broad enough to tolerate a wider blur.");

    ImGui::TextDisabled("REBLUR divides hit distance by (A + |viewZ| * B) * lerp(C, 1, f(roughness)) and saturates. Anything longer normalizes to 1 and gets the widest blur.");
    ImGui::TreePop();
  }

  // Firefly suppression

  if(ImGui::Checkbox("Anti-Firefly", &settings.enableAntiFirefly))
  {
    changed = true;
  }

  DrawTooltip("Clamps isolated bright pixels before they are accumulated. A single unlucky sample can otherwise smear into a bright blob that survives for the whole history length.");

  ImGui::TreePop();
  return changed;
}

inline bool DrawBounceLimitControl(const char* label, uint32_t& settingValue, uint32_t bounceLimit)
{
  // The slider range stops at the renderer's bounce limit, so the UI cannot request a longer path than the renderer allows.
  int value = static_cast<int>(settingValue);

  const bool changed = ImGui::SliderInt(label, &value, 0, static_cast<int>(bounceLimit));

  DrawTooltip("How many times a path may bounce before it is cut off. One bounce is direct lighting only; each further bounce adds a round of indirect light and costs roughly a ray per pixel. Energy is lost rather than biased toward a color, so a low limit reads as darker corners.");

  if(!changed)
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
    DrawTooltip("Frames averaged into the image since the last reset. It restarts whenever the camera, the scene, or any setting that changes the result moves.");

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
