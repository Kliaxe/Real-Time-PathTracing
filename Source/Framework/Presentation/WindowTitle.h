#pragma once

#include <string>

#include <volk.h>

namespace rtpt
{

class Window;

// WindowTitle
// Shows the executable name, the size of the image the caller displays, and the frame rate in the window title, in the layout nvpro_core2's ElementDefaultWindowTitle used before this project had its own framework.
// The title is refreshed once per second rather than every frame, because numbers changing at frame rate are unreadable and setting a title is a synchronous call into the OS window system.

class WindowTitle
{
public:

  // Call once per interactive frame after UiRenderer::BeginFrame, because the frame rate comes from ImGui's frame timing, which NewFrame updates.
  // displaySize is the pixel size of what the user is looking at, such as the Display panel, not the whole window.
  void Update(Window& window, VkExtent2D displaySize);

private:

  // Executable file name without its extension. Looked up on the first refresh, since the path is queried from the OS.
  std::string m_ExecutableName;

  // Frame time accumulated since the title was last written, in seconds.
  float m_SecondsSinceRefresh = 0.0f;
};

}  // namespace rtpt
