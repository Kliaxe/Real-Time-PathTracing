#include "Camera/CameraUi.h"

#include <iterator>

#include <imgui.h>

#include "Framework/Presentation/UiControls.h"

namespace rtpt
{

void DrawCameraSection(CameraController& camera)
{
  const bool open = ImGui::CollapsingHeader("Camera");

  DrawTooltip("Viewpoint the renderers trace from. Moving it restarts accumulation and forces the denoiser to reproject its history.");

  if(!open) return;

  // Input mode
  // Combo indices follow the CameraMode declaration order. The hint below the speed slider spells out the bindings of the active mode.

  int mode = static_cast<int>(camera.Mode());

  constexpr const char* modes[] = { "Orbit", "Fly" };

  if(ImGui::Combo("Mode##Camera", &mode, modes, std::size(modes)))
  {
    camera.SetMode(static_cast<CameraMode>(mode));
  }

  DrawTooltip("How the mouse moves the camera. Orbit turns around the center point below, which is the easier way to inspect one object. Fly moves the eye freely, for walking through a scene.");

  float speed = camera.Speed();

  if(ImGui::SliderFloat("Fly Speed", &speed, 0.1F, 100.0F, "%.1f", ImGuiSliderFlags_Logarithmic))
  {
    camera.SetSpeed(speed);
  }

  DrawTooltip("Movement rate in fly mode, in world units per second. Scenes here differ in scale by orders of magnitude, which is why the slider is logarithmic.");

  if(camera.Mode() == CameraMode::Orbit)
  {
    ImGui::TextDisabled("LMB orbit  |  MMB pan  |  RMB/scroll zoom");
  }
  else
  {
    ImGui::TextDisabled("RMB look  |  WASD move  |  E/Q up/down  |  Shift boost");
  }

  // State
  // The controls edit a copy, which is only written back when it is still a valid camera.

  CameraState state = camera.State();

  bool changed = DrawFloat3("Eye", state.eye, 0.02F, -10000.0F, 10000.0F);
  DrawTooltip("World-space position of the camera. Type a value here to return to an exact viewpoint, which is how a capture is reproduced.");

  changed |= DrawFloat3("Center", state.center, 0.02F, -10000.0F, 10000.0F);
  DrawTooltip("World-space point the camera looks at, and the pivot orbit mode turns around.");

  changed |= ImGui::SliderFloat("Vertical FOV", &state.verticalFovDegrees, 10.0F, 120.0F);
  DrawTooltip("Vertical field of view in degrees. A wider angle puts more of the scene in frame and gives each pixel more of it to resolve, so noise per pixel rises.");

  // Edits that would make the camera invalid, such as eye equal to center, are dropped instead of letting SetState throw.
  if(changed && IsValid(state))
  {
    camera.SetState(state);
  }
}

}  // namespace rtpt
