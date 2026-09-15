#include "Window.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace rtpt
{
namespace
{
// True while some Window holds the glfwInit reference. GLFW is process-global, so this blocks a second Window from initializing it underneath the first. Which Window may terminate it is tracked per instance by m_OwnsGlfw.
bool s_GlfwOwned = false;

// Recovers the Window registered as the GLFW user pointer in Initialize.
Window& Owner(GLFWwindow* window)
{
  return *static_cast<Window*>(glfwGetWindowUserPointer(window));
}

// GLFW reports errors through this callback rather than return codes, so they are printed to stderr where they would otherwise be lost.
void GlfwErrorCallback(int code, const char* description)
{
  std::fprintf(stderr, "GLFW error %d: %s\n", code, description == nullptr ? "unknown" : description);
}
}  // namespace

Window::~Window()
{
  Destroy();
}

void Window::Initialize(const WindowCreateInfo& createInfo)
{
  if(m_Window != nullptr || s_GlfwOwned || createInfo.width == 0 || createInfo.height == 0 || createInfo.title.empty())
  {
    throw std::invalid_argument("invalid Window initialization");
  }

  // GLFW library
  // The error callback is installed before glfwInit so initialization failures are reported too.

  glfwSetErrorCallback(GlfwErrorCallback);

  if(glfwInit() != GLFW_TRUE)
  {
    throw std::runtime_error("glfwInit failed");
  }

  s_GlfwOwned = true;
  m_OwnsGlfw  = true;

  // Window
  // GLFW_NO_API skips creating an OpenGL context, which a Vulkan swapchain does not need.
  // Every failure from here on calls Destroy so glfwTerminate still runs.

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_VISIBLE, createInfo.visible ? GLFW_TRUE : GLFW_FALSE);

  // glfwCreateWindow needs a null-terminated title, which a string_view does not guarantee.
  const std::string title(createInfo.title);

  m_Window = glfwCreateWindow(static_cast<int>(createInfo.width), static_cast<int>(createInfo.height), title.c_str(), nullptr, nullptr);

  if(m_Window == nullptr)
  {
    Destroy();
    throw std::runtime_error("glfwCreateWindow failed");
  }

  // Callbacks
  // The user pointer must be set before any callback is installed, because every callback dereferences it.

  glfwSetWindowUserPointer(m_Window, this);
  glfwSetFramebufferSizeCallback(m_Window, FramebufferSizeCallback);
  glfwSetWindowSizeCallback(m_Window, WindowSizeCallback);
  glfwSetCursorPosCallback(m_Window, CursorPositionCallback);
  glfwSetScrollCallback(m_Window, ScrollCallback);
  glfwSetKeyCallback(m_Window, KeyCallback);
  glfwSetMouseButtonCallback(m_Window, MouseButtonCallback);
  glfwSetWindowFocusCallback(m_Window, FocusCallback);

  // Initial sizes and cursor
  // Callbacks only fire on changes, so the starting values are queried directly.
  // Seeding the cursor position keeps the first motion event from producing a delta measured from the origin.

  int width  = 0;
  int height = 0;

  glfwGetFramebufferSize(m_Window, &width, &height);

  m_FramebufferWidth  = static_cast<uint32_t>(width);
  m_FramebufferHeight = static_cast<uint32_t>(height);

  glfwGetWindowSize(m_Window, &width, &height);

  m_LogicalWidth  = static_cast<uint32_t>(width);
  m_LogicalHeight = static_cast<uint32_t>(height);

  glfwGetCursorPos(m_Window, &m_Input.cursorX, &m_Input.cursorY);

  m_HasCursorPosition = true;

  // Vulkan instance extensions
  // GLFW returns null when it finds no Vulkan loader or surface support, which makes this window unusable for presentation.
  // VK_KHR_surface_maintenance1 is the instance-side counterpart of the swapchain maintenance device extension, and it depends on VK_KHR_get_surface_capabilities2.

  uint32_t extensionCount = 0;
  const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);

  if(extensions == nullptr || extensionCount == 0)
  {
    Destroy();
    throw std::runtime_error("GLFW reported no Vulkan surface extensions");
  }

  m_VulkanExtensions.assign(extensions, extensions + extensionCount);
  m_VulkanExtensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
  m_VulkanExtensions.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
}

void Window::Destroy()
{
  if(m_Window != nullptr)
  {
    glfwDestroyWindow(m_Window);
    m_Window = nullptr;
  }

  // Reset state
  // Destroy also runs on partial initialization failures, so every field returns to its default and the Window can be initialized again.

  m_VulkanExtensions.clear();

  m_Input             = {};
  m_FramebufferWidth  = 0;
  m_FramebufferHeight = 0;
  m_LogicalWidth      = 0;
  m_LogicalHeight     = 0;
  m_Resized           = false;
  m_HasCursorPosition = false;
  m_CursorCaptured    = false;

  // The window is destroyed first because glfwTerminate would invalidate its handle. Only the Window that called glfwInit terminates GLFW, so a Window that never initialized, or was rejected because another one owns GLFW, leaves the live window alone.
  if(m_OwnsGlfw)
  {
    glfwTerminate();
    s_GlfwOwned = false;
    m_OwnsGlfw  = false;
  }
}

void Window::PollEvents()
{
  if(m_Window == nullptr)
  {
    throw std::logic_error("Window is not initialized");
  }

  // Per-frame accumulators
  // Callbacks add to these during glfwPollEvents, so they are zeroed first to report only this frame's motion.

  m_Input.cursorDeltaX = 0.0;
  m_Input.cursorDeltaY = 0.0;
  m_Input.scrollX      = 0.0;
  m_Input.scrollY      = 0.0;
  m_Resized            = false;

  glfwPollEvents();
}

void Window::WaitEvents() const
{
  if(m_Window == nullptr)
  {
    throw std::logic_error("Window is not initialized");
  }

  glfwWaitEvents();
}

void Window::SetCursorCaptured(bool captured)
{
  if(m_Window == nullptr)
  {
    throw std::logic_error("Window is not initialized");
  }

  if(m_CursorCaptured == captured)
  {
    return;
  }

  glfwSetInputMode(m_Window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);

  // Raw motion bypasses OS pointer acceleration, but GLFW only accepts the mode where the platform supports it.
  if(glfwRawMouseMotionSupported() == GLFW_TRUE)
  {
    glfwSetInputMode(m_Window, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
  }

  m_CursorCaptured = captured;

  // Cursor mode changes can synthesize a position callback. The next real event establishes a new delta origin instead of turning that warp into camera input.
  m_HasCursorPosition = false;
}

bool Window::ShouldClose() const
{
  return m_Window == nullptr || glfwWindowShouldClose(m_Window) == GLFW_TRUE;
}

void Window::RequestClose()
{
  if(m_Window != nullptr)
  {
    glfwSetWindowShouldClose(m_Window, GLFW_TRUE);
  }
}

void Window::SetTitle(std::string_view title)
{
  // Headless runs and destroyed windows have no title bar.
  if(m_Window == nullptr)
  {
    return;
  }

  // glfwSetWindowTitle needs a null-terminated string, which a string_view does not guarantee.
  const std::string terminated(title);

  glfwSetWindowTitle(m_Window, terminated.c_str());
}

VkResult Window::CreateVulkanSurface(VkInstance instance, VkSurfaceKHR& surface) const
{
  // A non-null surface is rejected so an existing handle is never leaked by being overwritten.
  if(m_Window == nullptr || instance == VK_NULL_HANDLE || surface != VK_NULL_HANDLE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  return glfwCreateWindowSurface(instance, m_Window, nullptr, &surface);
}

void Window::FramebufferSizeCallback(GLFWwindow* window, int width, int height)
{
  Window& owner = Owner(window);

  owner.m_FramebufferWidth  = static_cast<uint32_t>(width);
  owner.m_FramebufferHeight = static_cast<uint32_t>(height);
  owner.m_Resized           = true;
}

void Window::WindowSizeCallback(GLFWwindow* window, int width, int height)
{
  Window& owner = Owner(window);

  owner.m_LogicalWidth  = static_cast<uint32_t>(width);
  owner.m_LogicalHeight = static_cast<uint32_t>(height);
}

void Window::CursorPositionCallback(GLFWwindow* window, double x, double y)
{
  Window& owner = Owner(window);

  // Several motion events can arrive in one poll, so deltas accumulate. Without a known previous position the sample only sets the origin.
  if(owner.m_HasCursorPosition)
  {
    owner.m_Input.cursorDeltaX += x - owner.m_Input.cursorX;
    owner.m_Input.cursorDeltaY += y - owner.m_Input.cursorY;
  }

  owner.m_Input.cursorX     = x;
  owner.m_Input.cursorY     = y;
  owner.m_HasCursorPosition = true;
}

void Window::ScrollCallback(GLFWwindow* window, double x, double y)
{
  Window& owner = Owner(window);

  owner.m_Input.scrollX += x;
  owner.m_Input.scrollY += y;
}

void Window::KeyCallback(GLFWwindow* window, int key, int, int action, int modifiers)
{
  Window& owner = Owner(window);

  // GLFW_KEY_UNKNOWN is -1, so the range check is required. GLFW_REPEAT counts as held.
  if(key >= 0 && static_cast<uint32_t>(key) < owner.m_Input.keys.size())
  {
    owner.m_Input.keys[static_cast<uint32_t>(key)] = action != GLFW_RELEASE;
  }

  owner.m_Input.modifiers = modifiers;
}

void Window::MouseButtonCallback(GLFWwindow* window, int button, int action, int modifiers)
{
  Window& owner = Owner(window);

  if(button >= 0 && static_cast<uint32_t>(button) < owner.m_Input.mouseButtons.size())
  {
    owner.m_Input.mouseButtons[static_cast<uint32_t>(button)] = action != GLFW_RELEASE;
  }

  owner.m_Input.modifiers = modifiers;
}

void Window::FocusCallback(GLFWwindow* window, int focused)
{
  Window& owner = Owner(window);

  owner.m_Input.focused = focused == GLFW_TRUE;

  if(!owner.m_Input.focused)
  {
    // Release everything
    // Release events for keys and buttons let go while another window has focus never reach this window, so held state is cleared instead of sticking.

    owner.m_Input.keys.fill(false);
    owner.m_Input.mouseButtons.fill(false);
    owner.m_Input.modifiers = 0;

    // Release the cursor
    // A captured cursor would otherwise stay hidden and locked after switching away. This mirrors SetCursorCaptured(false) directly on the GLFW handle the callback received.

    if(owner.m_CursorCaptured)
    {
      glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

      if(glfwRawMouseMotionSupported() == GLFW_TRUE)
      {
        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
      }

      owner.m_CursorCaptured    = false;
      owner.m_HasCursorPosition = false;
    }
  }
}

}  // namespace rtpt
