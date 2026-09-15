#include "WindowTitle.h"

#include "Framework/Platform/Paths.h"
#include "Framework/Platform/Window.h"

#include <imgui.h>

#include <cstdio>

namespace rtpt
{

void WindowTitle::Update(Window& window, VkExtent2D displaySize)
{
  const ImGuiIO& io = ImGui::GetIO();

  // Refresh interval
  // Measured in ImGui's frame time, so the interval and the reported frame rate come from the same clock.

  m_SecondsSinceRefresh += io.DeltaTime;

  if(m_SecondsSinceRefresh <= 1.0f)
  {
    return;
  }

  m_SecondsSinceRefresh = 0.0f;

  // Title text
  // ImGui's Framerate is a running average over recent frames. The guard keeps the frame time finite before any frame time has been measured.
  // Same layout as nvpro_core2: name | width x height | FPS / ms.

  const float framerate    = io.Framerate;
  const float milliseconds = framerate > 0.0f ? 1000.0f / framerate : 0.0f;

  if(m_ExecutableName.empty())
  {
    m_ExecutableName = ExecutablePath().stem().string();
  }

  char title[256] {};

  std::snprintf(title, sizeof(title), "%s | %ux%u | %.0f FPS / %.3fms", m_ExecutableName.c_str(), displaySize.width, displaySize.height, static_cast<double>(framerate), static_cast<double>(milliseconds));

  window.SetTitle(title);
}

}  // namespace rtpt
