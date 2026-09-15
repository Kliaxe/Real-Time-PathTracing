#pragma once

#include <array>
#include <cstdint>

namespace rtpt
{

// InputState
// Snapshot of keyboard, mouse, and focus state that Window fills from GLFW callbacks.
// The application reads it once per frame instead of registering its own GLFW callbacks. Indices and modifier bits still use GLFW's key, button, and GLFW_MOD_* values.

struct InputState
{
  // Room for every GLFW key code; GLFW_KEY_LAST is 348, so 512 leaves headroom.
  static constexpr uint32_t keyCapacity = 512;

  // Room for every GLFW mouse button; GLFW_MOUSE_BUTTON_LAST is 7.
  static constexpr uint32_t mouseButtonCapacity = 16;

  // Pressed state indexed by GLFW key code. Key repeats count as pressed.
  std::array<bool, keyCapacity> keys {};

  // Pressed state indexed by GLFW mouse button.
  std::array<bool, mouseButtonCapacity> mouseButtons {};

  // Latest cursor X in window coordinates, as reported by GLFW.
  double cursorX = 0.0;

  // Latest cursor Y in window coordinates, as reported by GLFW.
  double cursorY = 0.0;

  // Horizontal cursor movement accumulated since the last Window::PollEvents, which resets it.
  double cursorDeltaX = 0.0;

  // Vertical cursor movement accumulated since the last Window::PollEvents, which resets it.
  double cursorDeltaY = 0.0;

  // Horizontal scroll accumulated since the last Window::PollEvents, which resets it.
  double scrollX = 0.0;

  // Vertical scroll accumulated since the last Window::PollEvents, which resets it.
  double scrollY = 0.0;

  // GLFW_MOD_* bits from the most recent key or mouse button event.
  int modifiers = 0;

  // Whether the window has input focus. Losing focus clears every pressed key and button, since their release events would be missed.
  bool focused = true;

  // Out-of-range codes, including GLFW_KEY_UNKNOWN (-1), read as released instead of indexing out of bounds.
  [[nodiscard]] bool KeyDown(int key) const noexcept
  {
    return key >= 0 && static_cast<uint32_t>(key) < keys.size() && keys[static_cast<uint32_t>(key)];
  }

  // Out-of-range buttons read as released instead of indexing out of bounds.
  [[nodiscard]] bool MouseButtonDown(int button) const noexcept
  {
    return button >= 0 && static_cast<uint32_t>(button) < mouseButtons.size() && mouseButtons[static_cast<uint32_t>(button)];
  }
};

}  // namespace rtpt
