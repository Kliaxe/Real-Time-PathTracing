#include "Camera/CameraUi.h"

#include <iterator>

#include <imgui.h>

#include "Framework/Presentation/UiControls.h"

namespace rtpt
{

void DrawCameraSection(CameraController& camera)
{
  if(!ImGui::CollapsingHeader("Camera")) return;

  // Input mode
  // Combo indices follow the CameraMode declaration order. The hint below the speed slider spells out the bindings of the active mode.

  int mode = static_cast<int>(camera.Mode());

  constexpr const char* modes[] = { "Orbit", "Fly" };

  if(ImGui::Combo("Mode##Camera", &mode, modes, std::size(modes)))
  {
    camera.SetMode(static_cast<CameraMode>(mode));
  }

  float speed = camera.Speed();

  if(ImGui::SliderFloat("Fly Speed", &speed, 0.1F, 100.0F, "%.1f", ImGuiSliderFlags_Logarithmic))
  {
    camera.SetSpeed(speed);
  }

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

  changed |= DrawFloat3("Center", state.center, 0.02F, -10000.0F, 10000.0F);
  changed |= ImGui::SliderFloat("Vertical FOV", &state.verticalFovDegrees, 10.0F, 120.0F);

  // Edits that would make the camera invalid, such as eye equal to center, are dropped instead of letting SetState throw.
  if(changed && IsValid(state))
  {
    camera.SetState(state);
  }
}

}  // namespace rtpt
