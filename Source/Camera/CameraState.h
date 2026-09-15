#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace rtpt
{

// CameraProjection
// Selects which projection ProjectionMatrix builds from a CameraState.

enum class CameraProjection
{
  Perspective,
  Orthographic,
};

// CameraState
// The complete, copyable description of a camera: placement, lens, and projection.
// It is a plain value separate from CameraController, so UI edits, animation endpoints, scene defaults, and capture metadata can pass whole cameras around.

struct CameraState
{
  // World-space camera position.
  glm::vec3        eye { 10.0F, 10.0F, 10.0F };

  // World-space point the camera looks at, and the pivot for orbiting.
  glm::vec3        center { 0.0F };

  // World-space up hint. It need not be unit length; ViewMatrix normalizes it.
  glm::vec3        up { 0.0F, 1.0F, 0.0F };

  // Full vertical field of view used by the perspective projection. IsValid requires [0.01, 179].
  float            verticalFovDegrees = 60.0F;

  // Near and far clip distances. IsValid requires 0 < near < far.
  glm::vec2        clipPlanes { 0.001F, 100000.0F };

  // Half width and half height of the orthographic view volume in world units.
  glm::vec2        orthographicHalfSize { 5.0F };

  // Which projection ProjectionMatrix builds.
  CameraProjection projection = CameraProjection::Perspective;

  bool operator==(const CameraState&) const = default;
};

// True when every value is finite, eye and center are distinct, up is non-zero, the field of view is in [0.01, 179] degrees, the clip planes are positive and ordered, and the orthographic extent is positive.
[[nodiscard]] bool IsValid(const CameraState& state) noexcept;

// Right-handed look-at matrix. Throws std::invalid_argument for an invalid state.
[[nodiscard]] glm::mat4 ViewMatrix(const CameraState& state);

// Right-handed projection with depth in [0, 1] and Y flipped for Vulkan. Throws for an invalid state or an empty viewport.
[[nodiscard]] glm::mat4 ProjectionMatrix(const CameraState& state, glm::uvec2 viewport);

}  // namespace rtpt
