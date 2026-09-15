#pragma once

#include <cstdint>
#include <cstddef>
#include <span>

#include "Framework/Vulkan/Descriptors.h"

#include <glm/mat3x3.hpp>

namespace rtpt
{

// TonemapperSettings
// The tonemapper's user controls, pushed as the push constants of Tonemap.hlsl.
// CPU and HLSL share this scalar-layout push-constant contract, checked by the assertions below. The matrix is
// refreshed from exposure and white-balance controls immediately before dispatch, so its stored value is never used.

struct TonemapperSettings
{
  // Nonzero applies color correction, the filmic curve, and grading; zero writes the HDR input through unchanged.
  int32_t   active      = 1;

  // Linear multiplier folded into inputMatrix.
  float     exposure    = 1.0f;

  // Source white point in kelvin. The default is the neutral point, where white balance does nothing.
  float     temperature = 6506.11144f;

  // Green-magenta white balance offset. The default is neutral.
  float     tint        = 3.25895312e-3f;

  // Per-channel color correction applied before the curve. Overwritten by Tonemapper::Run.
  glm::mat3 inputMatrix = glm::mat3(1.0f);

  // Scales the curve output away from or toward 0.5.
  float     contrast    = 1.0f;

  // The curve output is raised to the power 1 / brightness.
  float     brightness  = 1.0f;

  // Blend from luminance at 0 through the original color at 1 and beyond.
  float     saturation  = 1.0f;

  // Strength of the radial darkening toward the image corners.
  float     vignette    = 0.0f;

  // Nonzero adds noise before 8-bit quantization to break up banding.
  int32_t   dither      = 1;
};

// Layout checks
// The offsets must match the HLSL struct: the 36-byte float3x3 starts at 16, so contrast begins at 52.

static_assert(sizeof(TonemapperSettings) == 72);
static_assert(offsetof(TonemapperSettings, active) == 0);
static_assert(offsetof(TonemapperSettings, exposure) == 4);
static_assert(offsetof(TonemapperSettings, temperature) == 8);
static_assert(offsetof(TonemapperSettings, tint) == 12);
static_assert(offsetof(TonemapperSettings, inputMatrix) == 16);
static_assert(offsetof(TonemapperSettings, contrast) == 52);
static_assert(offsetof(TonemapperSettings, brightness) == 56);
static_assert(offsetof(TonemapperSettings, saturation) == 60);
static_assert(offsetof(TonemapperSettings, vignette) == 64);
static_assert(offsetof(TonemapperSettings, dither) == 68);

// Tonemapper
// Compute pass that turns the linear HDR viewport target into the 8-bit display image: color correction, a filmic curve, grading, vignette, and dither.
// Input and output are bound through a push descriptor set, so no descriptor pool is needed and resized targets can be passed in directly.

class Tonemapper
{
public:

  // Builds the descriptor layout, pipeline layout, and pipeline. Fails if already initialized, and destroys anything partly built on failure.
  VkResult Initialize(VkDevice device, std::span<const uint32_t> spirv);

  void     Destroy();

  // Records the dispatch. input is read through a combined image sampler and output written as a storage image.
  void     Run(VkCommandBuffer cmd, VkExtent2D extent, const TonemapperSettings& settings, const VkDescriptorImageInfo& input, const VkDescriptorImageInfo& output);

private:

  // Push-descriptor layout: binding 0 is the HDR input, binding 1 the LDR output.
  rtpt::DescriptorPack     m_Descriptors;

  // Device the pipeline objects belong to. Null until initialized, which also makes Destroy a no-op.
  VkDevice                 m_Device         = VK_NULL_HANDLE;

  // Layout combining the push descriptor set and the settings push constant range.
  VkPipelineLayout         m_PipelineLayout = VK_NULL_HANDLE;

  // Compute pipeline built from Tonemap.hlsl.
  VkPipeline               m_Pipeline       = VK_NULL_HANDLE;
};

}  // namespace rtpt
