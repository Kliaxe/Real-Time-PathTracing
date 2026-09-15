#include "Camera/CameraController.h"

#include <cmath>
#include <exception>
#include <iostream>
#include <string_view>

#include <glm/geometric.hpp>

namespace
{
// Absolute tolerance for comparing against recorded float values, loose enough to absorb compiler and platform rounding differences.
bool Near(float left, float right, float tolerance = 2.0e-5F)
{
  return std::abs(left - right) <= tolerance;
}

bool Near(glm::vec2 left, glm::vec2 right)
{
  return Near(left.x, right.x) && Near(left.y, right.y);
}

bool Near(glm::vec3 left, glm::vec3 right)
{
  return Near(left.x, right.x) && Near(left.y, right.y) && Near(left.z, right.z);
}

bool Near(const glm::mat4& left, const glm::mat4& right)
{
  for(uint32_t column = 0; column < 4; ++column)
  {
    for(uint32_t row = 0; row < 4; ++row)
    {
      if(!Near(left[column][row], right[column][row]))
      {
        return false;
      }
    }
  }

  return true;
}

// True when no element of the matrix is NaN or infinite.
bool IsFinite(const glm::mat4& matrix)
{
  for(uint32_t column = 0; column < 4; ++column)
  {
    for(uint32_t row = 0; row < 4; ++row)
    {
      if(!std::isfinite(matrix[column][row]))
      {
        return false;
      }
    }
  }

  return true;
}

// Compares every CameraState field and reports which operation diverged, so a failure points at the step that broke.
bool Matches(const rtpt::CameraController& actual, const rtpt::CameraState& expected, std::string_view operation)
{
  const rtpt::CameraState& state = actual.State();

  if(!Near(state.eye, expected.eye) || !Near(state.center, expected.center) || !Near(state.up, expected.up) || !Near(state.verticalFovDegrees, expected.verticalFovDegrees) || !Near(state.clipPlanes, expected.clipPlanes) || !Near(state.orthographicHalfSize, expected.orthographicHalfSize) || state.projection != expected.projection)
  {
    std::cerr << "camera behavior diverged after " << operation << '\n';
    return false;
  }

  return true;
}
}  // namespace

int main()
{
  // Default camera
  // A fresh controller must start in orbit mode with the default CameraState.
  // The expected matrices are a recorded baseline of the Vulkan conventions: the projection's Y is negated for Vulkan clip space.

  rtpt::CameraController camera;

  camera.SetViewport({ 1280, 720 });

  if(camera.Mode() != rtpt::CameraMode::Orbit)
  {
    std::cerr << "orbit camera is not the default mode\n";
    return 1;
  }

  if(!Matches(camera, {}, "initialization"))
  {
    return 1;
  }

  const glm::mat4 expectedInitialView(
      0.707106769F, -0.408248246F, 0.577350199F, 0.0F,
      0.0F, 0.816496491F, 0.577350199F, 0.0F,
      -0.707106769F, -0.408248246F, 0.577350199F, 0.0F,
      0.0F, 0.0F, -17.3205051F, 1.0F);

  const glm::mat4 expectedInitialProjection(
      0.974278629F, 0.0F, 0.0F, 0.0F,
      0.0F, -1.7320509F, 0.0F, 0.0F,
      0.0F, 0.0F, -1.0F, -1.0F,
      0.0F, 0.0F, -0.00100000005F, 0.0F);

  if(!Near(camera.View(), expectedInitialView) || !Near(camera.Projection(), expectedInitialProjection))
  {
    std::cerr << "camera matrix convention differs from the recorded Vulkan baseline\n";
    return 1;
  }

  // Orbit dolly
  // A separate camera checks that a right drag in orbit mode moves the eye while leaving the orbit center fixed.

  rtpt::CameraController orbitCamera;

  orbitCamera.SetViewport({ 1280, 720 });
  orbitCamera.SetLookAt({ 0.0F, 0.0F, 5.0F }, { 0.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F });
  orbitCamera.SetPointerPosition({ 100.0F, 100.0F });

  const rtpt::CameraAction orbitDolly = orbitCamera.PointerMove({ 100.0F, 150.0F }, { .rightMouse = true });

  if(orbitDolly != rtpt::CameraAction::Dolly || !Near(orbitCamera.State().center, glm::vec3(0.0F)) || Near(orbitCamera.State().eye, glm::vec3(0.0F, 0.0F, 5.0F)))
  {
    std::cerr << "right drag does not dolly around the orbit center\n";
    return 1;
  }

  // Recorded interaction trajectory
  // The main camera runs a fixed sequence of interactions and is compared against recorded states after each one.
  // Every step starts from the state the previous step left behind, so the steps cannot be reordered or checked in isolation.

  camera.SetLookAt({ 2.0F, 3.0F, 7.0F }, { 0.5F, -0.25F, 0.0F }, { 0.0F, 1.0F, 0.0F });
  camera.SetPointerPosition({ 100.0F, 200.0F });
  camera.PointerMove({ 156.0F, 219.0F }, { .leftMouse = true });

  if(!Matches(camera, { .eye = { 0.0840562582F, 4.13698912F, 6.51105356F }, .center = { 0.5F, -0.25F, 0.0F } }, "orbit rotation"))
  {
    return 1;
  }

  camera.SetPointerPosition({ 156.0F, 219.0F });
  camera.PointerMove({ 171.0F, 244.0F }, { .middleMouse = true });

  if(!Matches(camera, { .eye = { -0.093478024F, 4.39857388F, 6.32346249F }, .center = { 0.322465718F, 0.0115848482F, -0.187590852F } }, "view-plane pan"))
  {
    return 1;
  }

  camera.Wheel(-10.0F, {});

  if(!Matches(camera, { .eye = { -0.060982421F, 4.05584049F, 5.81478643F }, .center = { 0.322465718F, 0.0115848482F, -0.187590852F } }, "wheel dolly"))
  {
    return 1;
  }

  camera.SetMode(rtpt::CameraMode::Fly);
  camera.MoveFly({ 0.0F, 0.0F, 1.0F }, 0.016F);
  camera.SetPointerPosition({ 171.0F, 244.0F });
  camera.PointerMove({ 191.0F, 230.0F }, { .rightMouse = true });

  if(!Matches(camera, { .eye = { -0.0584429838F, 4.02905703F, 5.7750349F }, .center = { 0.983746827F, 0.747943878F, -0.603029251F } }, "fly movement"))
  {
    return 1;
  }

  // Projection conversion
  // The camera is placed at a recorded state before the orthographic and perspective conversions, then zoomed, resized to a square viewport, and fitted to a box.

  camera.SetState({ .eye = { -0.0442075133F, 4.02905703F, 5.6879158F }, .center = { 0.997982323F, 0.747943878F, -0.690148473F } });

  camera.ConvertToOrthographic();
  camera.Wheel(-10.0F, { .shift = true });
  camera.SetViewport({ 900, 900 });
  camera.AdjustOrthographicAspect();

  if(!Matches(camera, { .eye = { -0.0442075133F, 4.02905703F, 5.6879158F }, .center = { 0.997982323F, 0.747943878F, -0.690148473F }, .orthographicHalfSize = { 3.85763884F, 3.85763884F }, .projection = rtpt::CameraProjection::Orthographic }, "orthographic conversion and zoom"))
  {
    return 1;
  }

  camera.ConvertToPerspective();
  camera.Fit({ -1.0F, -2.0F, -0.5F }, { 2.0F, 1.0F, 3.0F }, false, 1.0F);

  // The orthographic half size is expected to survive the conversion back to perspective.
  const rtpt::CameraState expectedFinal {
    .eye                  = { 0.0581865311F, 3.17690349F, 4.85291052F },
    .center               = { 0.5F, -0.5F, 1.25F },
    .verticalFovDegrees   = 56.0478134F,
    .orthographicHalfSize = { 3.85763884F, 3.85763884F },
  };

  if(!Matches(camera, expectedFinal, "projection conversion and scene fit"))
  {
    return 1;
  }

  const glm::mat4 expectedFinalView(
      0.992565155F, 0.0866174474F, -0.0855101421F, 0.0F,
      0.0F, 0.702543437F, 0.711640894F, 0.0F,
      0.121715106F, -0.706349969F, 0.697320044F, 0.0F,
      -0.648426414F, 1.19090056F, -5.63987064F, 1.0F);

  const glm::mat4 expectedFinalProjection(
      1.87883496F, 0.0F, 0.0F, 0.0F,
      0.0F, -1.87883496F, 0.0F, 0.0F,
      0.0F, 0.0F, -1.0F, -1.0F,
      0.0F, 0.0F, -0.00100000005F, 0.0F);

  if(!Near(camera.View(), expectedFinalView) || !Near(camera.Projection(), expectedFinalProjection))
  {
    std::cerr << "camera matrices diverged after the recorded interaction trajectory\n";
    return 1;
  }

  // Fly translation
  // Moving along all three axes at once must travel speed * time along the normalized direction, not faster along diagonals.
  // With speed 2 and 0.5 seconds the distance is 1, so each axis moves 1 / sqrt(3).

  rtpt::CameraController flyCamera;

  flyCamera.SetLookAt({ 0.0F, 0.0F, 5.0F }, { 0.0F, 0.0F, 4.0F }, { 0.0F, 1.0F, 0.0F });
  flyCamera.SetMode(rtpt::CameraMode::Fly);
  flyCamera.SetSpeed(2.0F);
  flyCamera.MoveFly({ 1.0F, 1.0F, 1.0F }, 0.5F);

  const float normalizedAxis = 1.0F / std::sqrt(3.0F);

  if(!Matches(flyCamera, { .eye = { normalizedAxis, normalizedAxis, 5.0F - normalizedAxis }, .center = { normalizedAxis, normalizedAxis, 4.0F - normalizedAxis } }, "normalized fly translation"))
  {
    return 1;
  }

  // Opposite up vectors
  // Animating between exactly opposite up vectors used to normalize their zero sum, which made the arc's control point, and every eye position along the arc, NaN.
  // Both eyes lie in the horizontal plane at distance 5 from the shared center, so halfway through the eye must pass through the arc midpoint, 5 / sqrt(2) along x and along z.

  rtpt::CameraController flipCamera;

  flipCamera.SetAnimationDuration(1.0);
  flipCamera.SetLookAt({ 0.0F, 0.0F, 5.0F }, { 0.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F });

  rtpt::CameraState flippedState = flipCamera.State();

  flippedState.eye = { 5.0F, 0.0F, 0.0F };
  flippedState.up  = { 0.0F, -1.0F, 0.0F };

  flipCamera.AnimateTo(flippedState, 0.0);
  flipCamera.UpdateAnimation(0.5);

  const float arcMidpoint = 5.0F / std::sqrt(2.0F);

  if(!Near(flipCamera.State().eye, glm::vec3(arcMidpoint, 0.0F, arcMidpoint)))
  {
    std::cerr << "animating between opposite up vectors does not follow a finite arc\n";
    return 1;
  }

  // Opposite up vector midpoint
  // Blending opposite up vectors linearly made the up vector zero halfway, so that frame's state was invalid and its view matrix could not be built.
  // The same halfway state must keep a unit-length up vector and give a finite view matrix.

  if(!Near(glm::length(flipCamera.State().up), 1.0F))
  {
    std::cerr << "animating between opposite up vectors does not keep the up vector unit length\n";
    return 1;
  }

  glm::mat4 flipMidpointView(0.0F);

  try
  {
    flipMidpointView = flipCamera.View();
  }
  catch(const std::exception& error)
  {
    std::cerr << "animating between opposite up vectors made an invalid view matrix: " << error.what() << '\n';
    return 1;
  }

  if(!IsFinite(flipMidpointView))
  {
    std::cerr << "animating between opposite up vectors made a non-finite view matrix\n";
    return 1;
  }

  // Flat box fit
  // A flat box whose thin axis points straight at the eye leaves every corner at the center's depth, so the tight fit measured a distance of zero, put the eye on the center, and SetState rejected it.
  // The fit must keep the view direction and leave the eye a small positive distance in front of the box.

  rtpt::CameraController flatFitCamera;

  flatFitCamera.SetLookAt({ 0.0F, 0.0F, 5.0F }, { 0.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F });

  try
  {
    flatFitCamera.Fit({ -1.0F, -1.0F, 0.0F }, { 1.0F, 1.0F, 0.0F }, true, 1.0F);
  }
  catch(const std::exception& error)
  {
    std::cerr << "fitting a flat box along its thin axis threw: " << error.what() << '\n';
    return 1;
  }

  const glm::vec3 flatFitOffset = flatFitCamera.State().eye - flatFitCamera.State().center;

  if(!Near(flatFitCamera.State().center, glm::vec3(0.0F)) || !Near(flatFitOffset.x, 0.0F) || !Near(flatFitOffset.y, 0.0F) || flatFitOffset.z <= 0.0F)
  {
    std::cerr << "fitting a flat box along its thin axis did not keep the eye in front of it\n";
    return 1;
  }

  return 0;
}
