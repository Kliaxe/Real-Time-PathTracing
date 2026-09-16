#include "PostProcessing/TonemapperUi.h"

#include <imgui.h>

#include "Framework/Presentation/UiControls.h"

namespace rtpt
{

void DrawTonemapperSection(TonemapperSettings& settings)
{
  if(!ImGui::CollapsingHeader("Tonemapper")) return;

  // The settings store flags as int32_t to match the shader layout, so checkboxes edit local bools.
  bool enabled = settings.active != 0;

  if(ImGui::Checkbox("Enabled", &enabled)) settings.active = enabled ? 1 : 0;

  DrawTooltip("Runs the tonemapping pass. It is display processing only, applied after the renderers, so nothing here changes what is being rendered or restarts accumulation. Off shows the raw HDR values clipped to the display range.");

  ImGui::SliderFloat("Exposure", &settings.exposure, 0.01F, 10.0F, "%.3f", ImGuiSliderFlags_Logarithmic);
  DrawTooltip("Multiplier applied to radiance before the tone curve. It decides which part of the scene's dynamic range lands in the visible range - the equivalent of a camera's exposure setting.");

  ImGui::SliderFloat("Temperature", &settings.temperature, 1000.0F, 15000.0F, "%.0f K");
  DrawTooltip("White balance in kelvin. Low values read as warm and orange, high values as cool and blue.");

  ImGui::SliderFloat("Tint", &settings.tint, -0.05F, 0.05F, "%.4f");
  DrawTooltip("White balance along the green-magenta axis, the direction temperature alone cannot correct.");

  ImGui::SliderFloat("Contrast", &settings.contrast, 0.0F, 2.0F);
  DrawTooltip("Spread of the image around middle grey. Above 1 deepens shadows and brightens highlights; below 1 flattens both.");

  ImGui::SliderFloat("Brightness", &settings.brightness, 0.01F, 2.0F);
  DrawTooltip("Lifts or lowers the whole image after the tone curve. Unlike exposure, it does not change which part of the dynamic range was captured.");

  ImGui::SliderFloat("Saturation", &settings.saturation, 0.0F, 2.0F);
  DrawTooltip("Color intensity. 0 is greyscale, which is a useful way to judge noise without chroma distracting from it.");

  ImGui::SliderFloat("Vignette", &settings.vignette, 0.0F, 1.0F);
  DrawTooltip("Darkens the corners of the frame. Purely a look; it also hides noise at the edges, so turn it off when judging image quality.");

  bool dither = settings.dither != 0;

  if(ImGui::Checkbox("Dither", &dither)) settings.dither = dither ? 1 : 0;

  DrawTooltip("Adds a sub-quantization-step of noise before the image is written to 8 bits, which breaks up the banding a smooth gradient would otherwise show.");
}

}  // namespace rtpt
