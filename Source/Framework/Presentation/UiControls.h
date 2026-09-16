#pragma once

// UiControls
// Small ImGui controls shared by the subsystem panels in the Settings window.
// Header-only because each control is a one-line wrapper that only fixes a format or a widget choice every panel should agree on.

#include <glm/gtc/type_ptr.hpp>
#include <glm/vec3.hpp>
#include <imgui.h>

namespace rtpt
{

// Attaches a hover explanation to the control drawn immediately before it.
// Everything in the Settings window is a rendering term rather than a plain value, so the panels explain each control here instead of crowding the layout with permanent text.
// The wrap width keeps a long explanation readable rather than letting it stretch across the screen.
inline void DrawTooltip(const char* explanation)
{
  if(!ImGui::BeginItemTooltip())
  {
    return;
  }

  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0F);
  ImGui::TextUnformatted(explanation);
  ImGui::PopTextWrapPos();

  ImGui::EndTooltip();
}

// Three-component drag with the three-decimal format every vector control in the Settings window shares.
inline bool DrawFloat3(const char* label, glm::vec3& value, float speed, float minimum, float maximum)
{
  return ImGui::DragFloat3(label, glm::value_ptr(value), speed, minimum, maximum, "%.3f");
}

}  // namespace rtpt
