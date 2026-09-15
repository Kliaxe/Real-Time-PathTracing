#pragma once

#include "CameraState.h"

#include <array>
#include <optional>

#include <glm/vec2.hpp>

namespace rtpt
{

// CameraMode
// Selects how pointer, wheel, and keyboard input move the camera.
// Orbit keeps the center as a pivot and moves the eye around it; Fly moves eye and center together like a first-person camera.

enum class CameraMode
{
  Orbit,
  Fly,
};

// CameraAction
// The manipulation a pointer drag resolved to. PointerMove returns it so callers and tests can see which gesture ran.

enum class CameraAction
{
  None,
  Orbit,
  Dolly,
  Pan,
  LookAround,
};

// CameraInput
// Mouse button and modifier state for one input update.
// It holds no windowing types, so the application can fill it from GLFW and the CPU tests can fill it with plain values.

struct CameraInput
{
  // Orbits in Orbit mode.
  bool leftMouse = false;

  // Pans in both modes.
  bool middleMouse = false;

  // Dollies in Orbit mode and looks around in Fly mode.
  bool rightMouse = false;

  // Makes the wheel zoom the lens (field of view or orthographic extent) instead of dollying.
  bool shift = false;

  // Makes a wheel dolly keep the center fixed in Fly mode.
  bool control = false;

  // Filled by the application but not read by CameraController.
  bool alt = false;
};

// CameraController
// Owns the interactive camera: turns pointer drags, wheel steps, and fly movement into CameraState edits, and animates between states.
// Any direct manipulation cancels a running animation so user input always wins. Pointer motion is measured as a fraction of the viewport, so gestures behave the same at every resolution.

class CameraController
{
public:

  CameraController() = default;

  // Replaces the state outright and cancels any animation. Throws std::invalid_argument for an invalid state.
  void SetState(CameraState state);

  // Starts animating toward state. A negative start time means now; a projection change or zero duration snaps instead.
  void AnimateTo(CameraState state, double startTimeSeconds = -1.0);

  void SetLookAt(glm::vec3 eye, glm::vec3 center, glm::vec3 up);

  // Throws for a zero dimension, which pointer normalization and aspect ratios would divide by.
  void SetViewport(glm::uvec2 viewport);

  void SetMode(CameraMode mode) noexcept { m_Mode = mode; }

  // Fly speed in world units per second. Throws for a negative or non-finite value.
  void SetSpeed(float speed);

  // Throws for a negative or non-finite duration. Zero makes AnimateTo snap.
  void SetAnimationDuration(double seconds);

  // Moves the drag origin without moving the camera, so the next PointerMove measures from here.
  void SetPointerPosition(glm::vec2 position) noexcept { m_Pointer = position; }

  // Advances a running animation. A negative time means now.
  void UpdateAnimation(double currentTimeSeconds = -1.0);

  // Applies the drag from the previous pointer position to position, using the gesture the mode and held buttons select.
  CameraAction PointerMove(glm::vec2 position, const CameraInput& input);

  // Moves eye and center along right, up, and forward in Fly mode. Axes longer than one are shortened to unit length.
  void MoveFly(glm::vec3 axes, float deltaTimeSeconds);

  // Dollies, or zooms the lens with Shift, by a wheel step.
  void Wheel(float value, const CameraInput& input);

  // Backs the eye off along its current direction to the box center until the box fits. tight fits the corners; otherwise the bounding sphere.
  void Fit(glm::vec3 boundsMin, glm::vec3 boundsMax, bool tight, float aspect, bool animated = false);

  // Switches to perspective with the field of view that keeps the visible height at the center distance.
  void ConvertToPerspective();

  // Switches to orthographic with the extent that keeps the visible height at the center distance.
  void ConvertToOrthographic();

  // Rederives the orthographic width from its height after a viewport change.
  void AdjustOrthographicAspect();

  [[nodiscard]] const CameraState& State() const noexcept { return m_State; }
  [[nodiscard]] glm::mat4 View() const { return ViewMatrix(m_State); }
  [[nodiscard]] glm::mat4 Projection() const { return ProjectionMatrix(m_State, m_Viewport); }
  [[nodiscard]] glm::uvec2 Viewport() const noexcept { return m_Viewport; }
  [[nodiscard]] CameraMode Mode() const noexcept { return m_Mode; }
  [[nodiscard]] float Speed() const noexcept { return m_Speed; }
  [[nodiscard]] bool IsAnimating() const noexcept { return m_AnimationStart.has_value(); }

private:

  // CameraFrame
  // Orthonormal camera basis derived from eye, center, and up. It is recomputed on demand, so it can never go stale against the state.

  struct CameraFrame
  {
    // Unit direction from eye toward center.
    glm::vec3 forward {};

    // Unit screen-right direction.
    glm::vec3 right {};

    // Unit screen-up direction, orthogonal to forward and right.
    glm::vec3 up {};
  };

  // Monotonic clock used when callers pass a negative time.
  static double CurrentTimeSeconds();

  [[nodiscard]] CameraFrame Frame() const;

  // World-space width and height of the view at the center distance, or of the orthographic volume.
  [[nodiscard]] glm::vec2 ViewDimensions() const;

  void Pan(glm::vec2 displacement);

  // Rotates the eye around the center, or with invert the center around the eye for Fly look-around.
  void Orbit(glm::vec2 displacement, bool invert);

  // Moves the eye toward the center, or shrinks the orthographic extent. keepCenterFixed stops Fly mode from carrying the center along.
  void Dolly(glm::vec2 displacement, bool keepCenterFixed = false);

  void CancelAnimation() noexcept;

  // Current camera; the only state View and Projection are built from.
  CameraState                m_State;

  // State when the running animation started.
  CameraState                m_AnimationSource;

  // State the running animation ends at.
  CameraState                m_AnimationTarget;

  // Quadratic Bezier control points for the animated eye: source eye, arc control point, target eye.
  std::array<glm::vec3, 3>   m_AnimationBezier {};

  // distance * tan(fov / 2) at the source, the half-height visible at the center. Interpolated so framing changes smoothly while the distance changes.
  float                      m_AnimationDollyZoomSource = 0.0F;

  // The same visible half-height at the target.
  float                      m_AnimationDollyZoomTarget = 0.0F;

  // Start of the running animation in steady-clock seconds. Empty when not animating.
  std::optional<double>      m_AnimationStart;

  // Viewport size in pixels, for pointer normalization and aspect ratio. Never zero.
  glm::uvec2                 m_Viewport { 1, 1 };

  // Last pointer position in pixels, the origin of the next drag displacement.
  glm::vec2                  m_Pointer { 0.0F };

  // Fly speed in world units per second.
  float                      m_Speed = 3.0F;

  // Animation length in seconds.
  double                     m_AnimationDuration = 0.5;

  // Active input mapping.
  CameraMode                 m_Mode = CameraMode::Orbit;
};

}  // namespace rtpt
