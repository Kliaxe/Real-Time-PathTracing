#include "CameraState.h"

#include <cmath>
#include <stdexcept>

#include <glm/gtc/matrix_transform.hpp>

namespace rtpt
{
namespace
{
// Smallest eye-to-center distance and up length a valid state may have.
constexpr float kEpsilon = 1.0e-6F;

bool IsFinite(glm::vec2 value)
{
  return std::isfinite(value.x) && std::isfinite(value.y);
}

bool IsFinite(glm::vec3 value)
{
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
}  // namespace

bool IsValid(const CameraState& state) noexcept
{
  return IsFinite(state.eye) && IsFinite(state.center) && IsFinite(state.up) && glm::length(state.center - state.eye) >= kEpsilon && glm::length(state.up) >= kEpsilon && std::isfinite(state.verticalFovDegrees) && state.verticalFovDegrees >= 0.01F && state.verticalFovDegrees <= 179.0F && IsFinite(state.clipPlanes) && state.clipPlanes.x > 0.0F && state.clipPlanes.y > state.clipPlanes.x && IsFinite(state.orthographicHalfSize) && state.orthographicHalfSize.x > 0.0F && state.orthographicHalfSize.y > 0.0F;
}

glm::mat4 ViewMatrix(const CameraState& state)
{
  if(!IsValid(state))
  {
    throw std::invalid_argument("cannot build a view matrix from an invalid camera state");
  }

  return glm::lookAt(state.eye, state.center, glm::normalize(state.up));
}

glm::mat4 ProjectionMatrix(const CameraState& state, glm::uvec2 viewport)
{
  if(!IsValid(state) || viewport.x == 0 || viewport.y == 0)
  {
    throw std::invalid_argument("cannot build a projection matrix from an invalid camera or viewport");
  }

  // Projection
  // The _ZO variants map depth to [0, 1], which is Vulkan's clip-space depth range.

  glm::mat4 projection;

  if(state.projection == CameraProjection::Orthographic)
  {
    projection = glm::orthoRH_ZO(-state.orthographicHalfSize.x, state.orthographicHalfSize.x, -state.orthographicHalfSize.y, state.orthographicHalfSize.y, state.clipPlanes.x, state.clipPlanes.y);
  }
  else
  {
    const float aspect = static_cast<float>(viewport.x) / static_cast<float>(viewport.y);

    projection = glm::perspectiveRH_ZO(glm::radians(state.verticalFovDegrees), aspect, state.clipPlanes.x, state.clipPlanes.y);
  }

  // Vulkan's framebuffer Y axis points down, the opposite of the convention glm's projections assume.
  projection[1][1] *= -1.0F;

  return projection;
}

}  // namespace rtpt
