#include "PostProcessing/TonemapperUi.h"

#include <imgui.h>

namespace rtpt
{

void DrawTonemapperSection(TonemapperSettings& settings)
{
  if(!ImGui::CollapsingHeader("Tonemapper")) return;

  // The settings store flags as int32_t to match the shader layout, so checkboxes edit local bools.
  bool enabled = settings.active != 0;

  if(ImGui::Checkbox("Enabled", &enabled)) settings.active = enabled ? 1 : 0;

  ImGui::SliderFloat("Exposure", &settings.exposure, 0.01F, 10.0F, "%.3f", ImGuiSliderFlags_Logarithmic);
  ImGui::SliderFloat("Temperature", &settings.temperature, 1000.0F, 15000.0F, "%.0f K");
  ImGui::SliderFloat("Tint", &settings.tint, -0.05F, 0.05F, "%.4f");
  ImGui::SliderFloat("Contrast", &settings.contrast, 0.0F, 2.0F);
  ImGui::SliderFloat("Brightness", &settings.brightness, 0.01F, 2.0F);
  ImGui::SliderFloat("Saturation", &settings.saturation, 0.0F, 2.0F);
  ImGui::SliderFloat("Vignette", &settings.vignette, 0.0F, 1.0F);

  bool dither = settings.dither != 0;

  if(ImGui::Checkbox("Dither", &dither)) settings.dither = dither ? 1 : 0;
}

}  // namespace rtpt
