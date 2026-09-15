#pragma once

// UiControls
// Small ImGui controls shared by the subsystem panels in the Settings window.
// Header-only because each control is a one-line wrapper that only fixes a format or a widget choice every panel should agree on.

#include <glm/gtc/type_ptr.hpp>
#include <glm/vec3.hpp>
#include <imgui.h>

namespace rtpt
{

// Three-component drag with the three-decimal format every vector control in the Settings window shares.
inline bool DrawFloat3(const char* label, glm::vec3& value, float speed, float minimum, float maximum)
{
  return ImGui::DragFloat3(label, glm::value_ptr(value), speed, minimum, maximum, "%.3f");
}

}  // namespace rtpt
