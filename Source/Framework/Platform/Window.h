#pragma once

#include "InputState.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <volk.h>

struct GLFWwindow;

namespace rtpt
{

// WindowCreateInfo
// Parameters for Window::Initialize. Defaults describe the normal interactive application window.

struct WindowCreateInfo
{
  // Title bar text. Copied during initialization, so the view only has to outlive the Initialize call.
  std::string_view title = "Real-Time Path Tracing";

  // Requested window width in screen coordinates. Must be nonzero.
  uint32_t width = 1920;

  // Requested window height in screen coordinates. Must be nonzero.
  uint32_t height = 1080;

  // Hidden windows let tests create a Vulkan surface without showing anything on screen.
  bool visible = true;
};

// Window
// The GLFW window that owns the process-wide GLFW library, collects input, and creates the Vulkan presentation surface.
// GLFW is global state, so only one Window may be initialized at a time, and it owns the glfwInit/glfwTerminate pair.

class Window
{
public:

  Window() = default;
  Window(const Window&)            = delete;
  Window& operator=(const Window&) = delete;
  ~Window();

  void Initialize(const WindowCreateInfo& createInfo = {});

  void Destroy();

  // Clears the per-frame input deltas and the resize flag, then dispatches pending events into them.
  void PollEvents();

  // Blocks until an event arrives. Used while minimized so the frame loop does not spin.
  void WaitEvents() const;

  // Hides and locks the cursor for camera drags, using raw mouse motion when the platform supports it.
  void SetCursorCaptured(bool captured);

  [[nodiscard]] bool ShouldClose() const;

  void RequestClose();

  // Replaces the title bar text. Does nothing before Initialize or after Destroy.
  void SetTitle(std::string_view title);

  // A zero-sized framebuffer means the window is minimized and there is nothing to present to.
  [[nodiscard]] bool Minimized() const noexcept { return m_FramebufferWidth == 0 || m_FramebufferHeight == 0; }

  [[nodiscard]] uint32_t FramebufferWidth() const noexcept { return m_FramebufferWidth; }

  [[nodiscard]] uint32_t FramebufferHeight() const noexcept { return m_FramebufferHeight; }

  [[nodiscard]] uint32_t LogicalWidth() const noexcept { return m_LogicalWidth; }

  [[nodiscard]] uint32_t LogicalHeight() const noexcept { return m_LogicalHeight; }

  // True when the framebuffer size changed during the most recent PollEvents.
  [[nodiscard]] bool Resized() const noexcept { return m_Resized; }

  [[nodiscard]] bool CursorCaptured() const noexcept { return m_CursorCaptured; }

  [[nodiscard]] const InputState& Input() const noexcept { return m_Input; }

  [[nodiscard]] GLFWwindow* Handle() const noexcept { return m_Window; }

  // Instance extensions the Vulkan instance must enable to present to this window. Empty until Initialize succeeds.
  [[nodiscard]] std::span<const char* const> RequiredVulkanInstanceExtensions() const noexcept
  {
    return m_VulkanExtensions;
  }

  // Fails with VK_ERROR_INITIALIZATION_FAILED instead of overwriting when surface already holds a handle.
  [[nodiscard]] VkResult CreateVulkanSurface(VkInstance instance, VkSurfaceKHR& surface) const;

private:

  // GLFW callbacks
  // GLFW takes plain function pointers, so each callback recovers its Window from the GLFW user pointer set in Initialize.

  static void FramebufferSizeCallback(GLFWwindow* window, int width, int height);
  static void WindowSizeCallback(GLFWwindow* window, int width, int height);
  static void CursorPositionCallback(GLFWwindow* window, double x, double y);
  static void ScrollCallback(GLFWwindow* window, double x, double y);
  static void KeyCallback(GLFWwindow* window, int key, int scanCode, int action, int modifiers);
  static void MouseButtonCallback(GLFWwindow* window, int button, int action, int modifiers);
  static void FocusCallback(GLFWwindow* window, int focused);

  // Null before Initialize and after Destroy; every public operation checks it.
  GLFWwindow* m_Window = nullptr;

  // True when this Window's Initialize called glfwInit. GLFW is process-global, so only this Window may call glfwTerminate.
  bool m_OwnsGlfw = false;

  // GLFW's required surface extensions plus the surface-maintenance extensions the swapchain relies on.
  std::vector<const char*> m_VulkanExtensions;

  // Input snapshot written by the callbacks during PollEvents.
  InputState m_Input;

  // Framebuffer width in pixels. Differs from the logical width on high-DPI displays.
  uint32_t m_FramebufferWidth = 0;

  // Framebuffer height in pixels. Differs from the logical height on high-DPI displays.
  uint32_t m_FramebufferHeight = 0;

  // Window width in screen coordinates, the space cursor positions are reported in.
  uint32_t m_LogicalWidth = 0;

  // Window height in screen coordinates, the space cursor positions are reported in.
  uint32_t m_LogicalHeight = 0;

  // Set by the framebuffer size callback and cleared at the start of each PollEvents.
  bool m_Resized = false;

  // False until a cursor sample establishes the origin for the next delta. Cleared on capture changes so the synthesized warp is not reported as motion.
  bool m_HasCursorPosition = false;

  // Whether the cursor is currently hidden and locked to the window.
  bool m_CursorCaptured = false;
};

}  // namespace rtpt
