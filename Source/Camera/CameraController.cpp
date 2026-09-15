#include "CameraController.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace rtpt
{
namespace
{
// Threshold below which lengths and squared lengths are treated as degenerate.
constexpr float kEpsilon = 1.0e-6F;

// Eye-to-center distance below which a perspective dolly does nothing.
constexpr float kMinimumDistance = 1.0e-6F;

// Dolly steps at or above this fraction of the eye-to-center distance are rejected, so the eye never reaches the center.
constexpr float kMaximumDollyDisplacement = 0.99F;

// Smallest orthographic half-size a zoom can reach, keeping the extent positive as IsValid requires.
constexpr float kMinimumOrthographicSize = 0.01F;

// Smallest eye-to-center distance Fit places the camera at. It stays well clear of IsValid's distinctness epsilon even after the eye position is rounded at scene-sized coordinates.
constexpr float kMinimumFitDistance = 1.0e-3F;

// Validates a state and normalizes its up vector; every externally supplied state passes through here.
CameraState Normalized(CameraState state)
{
  if(!IsValid(state))
  {
    throw std::invalid_argument("invalid camera state");
  }

  state.up = glm::normalize(state.up);

  return state;
}

// Turns an animation's up vector from sourceUp toward targetUp at progress t and returns it at unit length. Both inputs are unit length, because every state passes through Normalized and this function keeps it so.
// Up vectors that are not opposite keep the direction of their linear blend, so those animations build the same view matrices as before. Opposite ones would blend through zero halfway, which has no direction, so they roll around the view direction instead.
// forward is this frame's eye-to-center vector; only its direction is used, to choose the roll axis.
glm::vec3 InterpolateUp(glm::vec3 sourceUp, glm::vec3 targetUp, glm::vec3 forward, float t)
{
  const glm::vec3 upSum = sourceUp + targetUp;

  // The same opposite test AnimateTo uses for the arc. Past it the blend is shortest halfway, at half of sqrt(kEpsilon) or more, so normalizing it is safe.
  if(glm::dot(upSum, upSum) >= kEpsilon)
  {
    return glm::normalize(glm::mix(sourceUp, targetUp, t));
  }

  // Roll axis
  // The part of this frame's view direction perpendicular to sourceUp. Rolling around it leaves up perpendicular to the view direction halfway through, so the look-at basis cannot degenerate there.

  glm::vec3 axis = forward - glm::dot(forward, sourceUp) * sourceUp;

  // Looking straight along up leaves no perpendicular part, so a world axis not parallel to up stands in, as it does in Frame.
  if(glm::dot(axis, axis) <= kEpsilon * glm::dot(forward, forward))
  {
    const glm::vec3 fallback = std::abs(sourceUp.y) < 0.99F ? glm::vec3(0.0F, 1.0F, 0.0F) : glm::vec3(1.0F, 0.0F, 0.0F);

    axis = glm::cross(sourceUp, fallback);
  }

  axis = glm::normalize(axis);

  // Rotation
  // A unit vector turned by an angle around a perpendicular unit axis is cos(angle) * v + sin(angle) * (axis x v), which is unit length at every angle.
  // Half a turn lands on -sourceUp, which matches targetUp within the opposite test above; UpdateAnimation copies the target exactly when the animation finishes.

  const float angle = glm::pi<float>() * t;

  return std::cos(angle) * sourceUp + std::sin(angle) * glm::cross(axis, sourceUp);
}
}  // namespace

void CameraController::SetState(CameraState state)
{
  m_State = Normalized(state);

  CancelAnimation();
}

void CameraController::AnimateTo(CameraState state, double startTimeSeconds)
{
  state = Normalized(state);

  // Perspective and orthographic parameters cannot be blended, and a zero duration would divide by zero in UpdateAnimation.
  if(state.projection != m_State.projection || m_AnimationDuration == 0.0)
  {
    SetState(state);
    return;
  }

  m_AnimationSource = m_State;
  m_AnimationTarget = state;

  // Eye path
  // Instead of a straight line, the eye follows a quadratic Bezier arc around the point midway between the two centers.
  // Halfway through, the arc passes through a point at the average of the two eye distances from that interest point.
  // The control point is then moved to the eye midpoint's height along the average up vector, so the arc adds no rise or dip along up.

  const glm::vec3 interest = 0.5F * (m_AnimationSource.center + m_AnimationTarget.center);
  const glm::vec3 midpoint = 0.5F * (m_AnimationSource.eye + m_AnimationTarget.eye);
  const float radius       = 0.5F * (glm::length(m_AnimationSource.eye - interest) + glm::length(m_AnimationTarget.eye - interest));

  glm::vec3 interestToMidpoint = midpoint - interest;

  // With no outward direction from the interest point, an arbitrary +Z direction is used.
  if(glm::dot(interestToMidpoint, interestToMidpoint) < kEpsilon)
  {
    interestToMidpoint = { 0.0F, 0.0F, 1.0F };
  }

  const glm::vec3 curveMidpoint = interest + radius * glm::normalize(interestToMidpoint);

  // B(0.5) = 0.25 * P0 + 0.5 * C + 0.25 * P2, so this control point makes the curve pass through curveMidpoint.
  glm::vec3 control = 2.0F * curveMidpoint - midpoint;

  const glm::vec3 upSum = m_AnimationSource.up + m_AnimationTarget.up;

  // Opposite up vectors cancel and have no average. They still share one axis and the projection below ignores its sign, so the target's already-normalized up stands in.
  const glm::vec3 averageUp = glm::dot(upSum, upSum) < kEpsilon ? m_AnimationTarget.up : glm::normalize(upSum);

  control += glm::dot(midpoint - control, averageUp) * averageUp;

  m_AnimationBezier = { m_AnimationSource.eye, control, m_AnimationTarget.eye };

  // Dolly zoom
  // distance * tan(fov / 2) is the half-height visible at the center. UpdateAnimation interpolates it and derives the field of view
  // from the current eye distance, so the framing changes smoothly even though the distance along the arc does not change linearly.

  const float sourceDistance = glm::length(m_AnimationSource.eye - m_AnimationSource.center);
  const float targetDistance = glm::length(m_AnimationTarget.eye - m_AnimationTarget.center);

  m_AnimationDollyZoomSource = sourceDistance * std::tan(glm::radians(m_AnimationSource.verticalFovDegrees * 0.5F));
  m_AnimationDollyZoomTarget = targetDistance * std::tan(glm::radians(m_AnimationTarget.verticalFovDegrees * 0.5F));

  // A negative start time means now.
  m_AnimationStart = startTimeSeconds < 0.0 ? CurrentTimeSeconds() : startTimeSeconds;
}

void CameraController::SetLookAt(glm::vec3 eye, glm::vec3 center, glm::vec3 up)
{
  CameraState state = m_State;

  state.eye    = eye;
  state.center = center;
  state.up     = up;

  SetState(state);
}

void CameraController::SetViewport(glm::uvec2 viewport)
{
  if(viewport.x == 0 || viewport.y == 0)
  {
    throw std::invalid_argument("camera viewport must have nonzero dimensions");
  }

  m_Viewport = viewport;
}

void CameraController::SetSpeed(float speed)
{
  if(!std::isfinite(speed) || speed < 0.0F)
  {
    throw std::invalid_argument("camera speed must be finite and nonnegative");
  }

  m_Speed = speed;
}

void CameraController::SetAnimationDuration(double seconds)
{
  if(!std::isfinite(seconds) || seconds < 0.0)
  {
    throw std::invalid_argument("camera animation duration must be finite and nonnegative");
  }

  m_AnimationDuration = seconds;
}

void CameraController::UpdateAnimation(double currentTimeSeconds)
{
  if(!m_AnimationStart)
  {
    return;
  }

  if(currentTimeSeconds < 0.0)
  {
    currentTimeSeconds = CurrentTimeSeconds();
  }

  // Progress
  // Smootherstep (6t^5 - 15t^4 + 10t^3) has zero velocity and acceleration at both ends, so the camera eases into and out of the motion.

  const double normalizedTime = (currentTimeSeconds - *m_AnimationStart) / m_AnimationDuration;

  float t = std::clamp(static_cast<float>(normalizedTime), 0.0F, 1.0F);

  t = t * t * t * (t * (t * 6.0F - 15.0F) + 10.0F);

  // Finishing copies the target exactly, so no interpolation error remains in the final state.
  if(t >= 1.0F)
  {
    m_State = m_AnimationTarget;
    CancelAnimation();
    return;
  }

  // Interpolation
  // Center, clip planes, and orthographic extent blend linearly, and the eye follows the Bezier arc set up by AnimateTo.
  // Up turns from the source up to the target up at unit length, so it never passes through zero even when the two are opposite. Opposite up vectors roll around this frame's view direction, so up follows the eye.

  m_State.center = glm::mix(m_AnimationSource.center, m_AnimationTarget.center, t);

  const float inverseT = 1.0F - t;

  m_State.eye = inverseT * inverseT * m_AnimationBezier[0] + 2.0F * inverseT * t * m_AnimationBezier[1] + t * t * m_AnimationBezier[2];
  m_State.up  = InterpolateUp(m_AnimationSource.up, m_AnimationTarget.up, m_State.center - m_State.eye, t);

  const float distance  = glm::length(m_State.eye - m_State.center);
  const float dollyZoom = glm::mix(m_AnimationDollyZoomSource, m_AnimationDollyZoomTarget, t);

  // The field of view that shows the interpolated half-height at the current distance, clamped to the range IsValid accepts; degenerate cases blend linearly.
  if(distance > kEpsilon && dollyZoom > 0.0F)
  {
    m_State.verticalFovDegrees = std::clamp(glm::degrees(2.0F * std::atan(dollyZoom / distance)), 0.01F, 179.0F);
  }
  else
  {
    m_State.verticalFovDegrees = glm::mix(m_AnimationSource.verticalFovDegrees, m_AnimationTarget.verticalFovDegrees, t);
  }

  m_State.clipPlanes           = glm::mix(m_AnimationSource.clipPlanes, m_AnimationTarget.clipPlanes, t);
  m_State.orthographicHalfSize = glm::mix(m_AnimationSource.orthographicHalfSize, m_AnimationTarget.orthographicHalfSize, t);
}

CameraAction CameraController::PointerMove(glm::vec2 position, const CameraInput& input)
{
  // Without a button the pointer is only tracked, so the next drag starts from where the pointer really is.
  if(!input.leftMouse && !input.middleMouse && !input.rightMouse)
  {
    m_Pointer = position;
    return CameraAction::None;
  }

  // Gesture mapping
  // Fly mode: right drag looks around, middle drag pans. Orbit mode: left drag orbits, middle drag pans, right drag dollies.
  // When several buttons are held, the first match in that order wins.

  CameraAction action = CameraAction::None;

  if(m_Mode == CameraMode::Fly)
  {
    if(input.rightMouse)
    {
      action = CameraAction::LookAround;
    }
    else if(input.middleMouse)
    {
      action = CameraAction::Pan;
    }
  }
  else
  {
    if(input.leftMouse)
    {
      action = CameraAction::Orbit;
    }
    else if(input.middleMouse)
    {
      action = CameraAction::Pan;
    }
    else if(input.rightMouse)
    {
      action = CameraAction::Dolly;
    }
  }

  // Apply
  // Displacement is a fraction of the viewport, so the same hand motion gives the same camera motion at any resolution.
  // Look-around reuses Orbit with the eye as pivot and the vertical displacement negated.

  const glm::vec2 displacement {
      (position.x - m_Pointer.x) / static_cast<float>(m_Viewport.x),
      (position.y - m_Pointer.y) / static_cast<float>(m_Viewport.y),
  };

  switch(action)
  {
    case CameraAction::Orbit:
      Orbit(displacement, false);
      break;
    case CameraAction::Dolly:
      Dolly(displacement);
      break;
    case CameraAction::Pan:
      Pan(displacement);
      break;
    case CameraAction::LookAround:
      Orbit({ displacement.x, -displacement.y }, true);
      break;
    case CameraAction::None:
      break;
  }

  CancelAnimation();

  m_Pointer = position;

  return action;
}

void CameraController::MoveFly(glm::vec3 axes, float deltaTimeSeconds)
{
  if(m_Mode != CameraMode::Fly || deltaTimeSeconds <= 0.0F || axes == glm::vec3(0.0F))
  {
    return;
  }

  const float magnitudeSquared = glm::dot(axes, axes);

  // Only over-long input is shortened, so diagonal key combinations are no faster than a single key.
  if(magnitudeSquared > 1.0F)
  {
    axes /= std::sqrt(magnitudeSquared);
  }

  // Translation
  // Eye and center move together along the camera basis, so the view direction does not change.

  const CameraFrame frame  = Frame();
  const glm::vec3 movement = m_Speed * deltaTimeSeconds * (frame.right * axes.x + frame.up * axes.y + frame.forward * axes.z);

  m_State.eye    += movement;
  m_State.center += movement;

  CancelAnimation();
}

void CameraController::Wheel(float value, const CameraInput& input)
{
  if(value == 0.0F)
  {
    return;
  }

  // Squaring the step while keeping its sign makes larger wheel steps move disproportionately further; dividing by the width puts it on the same scale as drags.
  const float displacement = value * std::abs(value) / static_cast<float>(m_Viewport.x);

  // Shift zooms the lens instead of moving the camera: the orthographic extent, or the field of view by one degree per wheel unit.
  if(input.shift)
  {
    if(m_State.projection == CameraProjection::Orthographic)
    {
      m_State.orthographicHalfSize = glm::max(m_State.orthographicHalfSize * (1.0F + displacement), glm::vec2(kMinimumOrthographicSize));
    }
    else
    {
      m_State.verticalFovDegrees = std::clamp(m_State.verticalFovDegrees + value, 0.01F, 179.0F);
    }
  }
  else
  {
    Dolly(glm::vec2(displacement), input.control);
  }

  CancelAnimation();
}

void CameraController::Fit(glm::vec3 boundsMin, glm::vec3 boundsMax, bool tight, float aspect, bool animated)
{
  if(glm::any(glm::greaterThan(boundsMin, boundsMax)) || !std::isfinite(aspect) || aspect <= 0.0F)
  {
    throw std::invalid_argument("invalid camera fit bounds or aspect ratio");
  }

  const glm::vec3 halfSize     = 0.5F * (boundsMax - boundsMin);
  const glm::vec3 boundsCenter = 0.5F * (boundsMin + boundsMax);
  const float tangentY         = std::tan(glm::radians(m_State.verticalFovDegrees * 0.5F));
  const float tangentX         = tangentY * aspect;

  // Fit distance
  // A tight fit rotates each box corner into a view looking from the eye at the box center and finds the distance at which it fits both the vertical and horizontal field of view.
  // Only corners farther than the center (negative view z) are visited, but the box is symmetric, so each one has a mirrored nearer corner with the same absolute offsets, and adding |z| accounts for that nearer corner's reduced depth.
  // A loose fit uses the bounding sphere instead. Either result is then raised to kMinimumFitDistance, so a box that measures zero never puts the eye on its center.

  float distance = 0.0F;

  if(tight)
  {
    const glm::mat3 viewRotation = glm::mat3(glm::lookAt(m_State.eye, boundsCenter, m_State.up));

    for(uint32_t corner = 0; corner < 8; ++corner)
    {
      glm::vec3 relative {
          (corner & 1u) != 0 ? halfSize.x : -halfSize.x,
          (corner & 2u) != 0 ? halfSize.y : -halfSize.y,
          (corner & 4u) != 0 ? halfSize.z : -halfSize.z,
      };

      relative = viewRotation * relative;

      if(relative.z < 0.0F)
      {
        distance = std::max(distance, std::abs(relative.y) / tangentY + std::abs(relative.z));
        distance = std::max(distance, std::abs(relative.x) / tangentX + std::abs(relative.z));
      }
    }
  }
  else
  {
    const float radius = glm::length(halfSize);

    distance = std::max(radius / tangentX, radius / tangentY);
  }

  // A box with no depth along the view, such as a flat box whose thin axis points at the eye, leaves every corner at the center's depth and measures zero, which would put the eye on the center.
  distance = std::max(distance, kMinimumFitDistance);

  // Placement
  // The eye keeps its current direction toward the box center and moves to the fit distance from it.

  CameraState target = m_State;

  target.eye    = boundsCenter - distance * glm::normalize(boundsCenter - m_State.eye);
  target.center = boundsCenter;

  if(animated)
  {
    AnimateTo(target);
  }
  else
  {
    SetState(target);
  }
}

void CameraController::ConvertToPerspective()
{
  if(m_State.projection == CameraProjection::Perspective)
  {
    return;
  }

  // Matching framing
  // The new field of view shows the same half-height at the center distance as the orthographic extent did.

  const float distance = glm::length(m_State.eye - m_State.center);

  m_State.verticalFovDegrees = std::clamp(glm::degrees(2.0F * std::atan(m_State.orthographicHalfSize.y / distance)), 0.01F, 179.0F);
  m_State.projection         = CameraProjection::Perspective;

  CancelAnimation();
}

void CameraController::ConvertToOrthographic()
{
  if(m_State.projection == CameraProjection::Orthographic)
  {
    return;
  }

  // Matching framing
  // The orthographic half-height equals the perspective half-height at the center distance, and the width follows the viewport aspect.

  const float distance = glm::length(m_State.eye - m_State.center);

  m_State.orthographicHalfSize.y = distance * std::tan(glm::radians(m_State.verticalFovDegrees * 0.5F));
  m_State.orthographicHalfSize.x = m_State.orthographicHalfSize.y * static_cast<float>(m_Viewport.x) / static_cast<float>(m_Viewport.y);
  m_State.projection             = CameraProjection::Orthographic;

  CancelAnimation();
}

void CameraController::AdjustOrthographicAspect()
{
  if(m_State.projection == CameraProjection::Orthographic)
  {
    m_State.orthographicHalfSize.x = m_State.orthographicHalfSize.y * static_cast<float>(m_Viewport.x) / static_cast<float>(m_Viewport.y);
  }
}

double CameraController::CurrentTimeSeconds()
{
  // steady_clock is monotonic, so system clock adjustments cannot jump or reverse an animation.
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

CameraController::CameraFrame CameraController::Frame() const
{
  CameraFrame frame;

  frame.forward = glm::normalize(m_State.center - m_State.eye);

  glm::vec3 right = glm::cross(frame.forward, m_State.up);

  // Looking along up makes the cross product degenerate, so a world axis not parallel to forward stands in for up.
  if(glm::dot(right, right) < kEpsilon)
  {
    const glm::vec3 fallback = std::abs(frame.forward.y) < 0.99F ? glm::vec3(0.0F, 1.0F, 0.0F) : glm::vec3(1.0F, 0.0F, 0.0F);

    right = glm::cross(frame.forward, fallback);
  }

  frame.right = glm::normalize(right);
  frame.up    = glm::cross(frame.right, frame.forward);

  return frame;
}

glm::vec2 CameraController::ViewDimensions() const
{
  if(m_State.projection == CameraProjection::Orthographic)
  {
    return 2.0F * m_State.orthographicHalfSize;
  }

  // Perspective view height at the center distance; the width follows the viewport aspect.
  const float height = 2.0F * glm::length(m_State.eye - m_State.center) * std::tan(glm::radians(m_State.verticalFovDegrees * 0.5F));

  return { height * static_cast<float>(m_Viewport.x) / static_cast<float>(m_Viewport.y), height };
}

void CameraController::Pan(glm::vec2 displacement)
{
  // Fly mode pans in the opposite direction to Orbit mode.
  if(m_Mode == CameraMode::Fly)
  {
    displacement *= -1.0F;
  }

  // Translation
  // Displacement is a viewport fraction; scaling it by the view size at the center distance moves the view by that fraction of what is visible there.
  // Eye and center move together, so the view direction does not change.

  const CameraFrame frame = Frame();
  const glm::vec2 view    = ViewDimensions();
  const glm::vec3 offset  = -displacement.x * frame.right * view.x + displacement.y * frame.up * view.y;

  m_State.eye    += offset;
  m_State.center += offset;
}

void CameraController::Orbit(glm::vec2 displacement, bool invert)
{
  // A drag across the whole viewport turns a full circle.
  displacement *= glm::two_pi<float>();

  // Pivot
  // Normally the eye circles the center. Inverted, for Fly look-around, the center circles the eye, which turns the view in place.

  const glm::vec3 origin = invert ? m_State.eye : m_State.center;

  glm::vec3 position = invert ? m_State.center : m_State.eye;
  glm::vec3 radial   = position - origin;

  const float radius = glm::length(radial);

  if(radius < kEpsilon)
  {
    return;
  }

  // Rotation
  // Yaw turns the radial around up, then pitch turns it around the resulting right axis.
  // A pitch that would flip the sign of the radial's world-space x component is discarded, keeping only the yaw.

  radial = glm::mat3(glm::rotate(glm::mat4(1.0F), -displacement.x, m_State.up)) * glm::normalize(radial);

  glm::vec3 right = glm::cross(m_State.up, radial);

  // A radial parallel to up has no pitch axis.
  if(glm::dot(right, right) < kEpsilon)
  {
    return;
  }

  right = glm::normalize(right);

  const glm::vec3 pitched = glm::mat3(glm::rotate(glm::mat4(1.0F), -displacement.y, right)) * radial;

  if(glm::sign(pitched.x) == glm::sign(radial.x))
  {
    radial = pitched;
  }

  position = origin + radius * radial;

  if(invert)
  {
    m_State.center = position;
  }
  else
  {
    m_State.eye = position;
  }
}

void CameraController::Dolly(glm::vec2 displacement, bool keepCenterFixed)
{
  // The dominant drag axis sets the amount; positive x or negative y moves toward the center.
  const float amount = std::abs(displacement.x) > std::abs(displacement.y) ? displacement.x : -displacement.y;

  // An orthographic camera has no depth to move through, so dollying scales the visible extent instead.
  if(m_State.projection == CameraProjection::Orthographic)
  {
    m_State.orthographicHalfSize = glm::max(m_State.orthographicHalfSize * (1.0F - amount), glm::vec2(kMinimumOrthographicSize));
    return;
  }

  glm::vec3 movement = m_State.center - m_State.eye;

  // A step that would bring the eye to or past the center would invalidate the state, so it is ignored.
  if(glm::length(movement) < kMinimumDistance || amount >= kMaximumDollyDisplacement)
  {
    return;
  }

  movement *= amount;

  m_State.eye += movement;

  // Fly mode carries the center along so the camera travels; Orbit mode, or keepCenterFixed, keeps the center as the pivot.
  if(m_Mode == CameraMode::Fly && !keepCenterFixed)
  {
    m_State.center += movement;
  }
}

void CameraController::CancelAnimation() noexcept
{
  m_AnimationStart.reset();
}

}  // namespace rtpt
