#pragma once

#include <cstdint>

namespace nvsamples
{

// Controls how a noisy path-traced frame is resolved into the displayed image.
enum class RenderResolveMode : uint32_t
{
  eOff = 0,
  eAccumulate,
  eDenoise,
};

// Shared denoiser inspection modes. Individual renderers can expose only the
// views they actually support while still reusing the same UI shell.
enum class DenoiserDebugView : uint32_t
{
  eFinal = 0,
  eRawBeauty,
  eDenoisedBeauty,
  eDiffuseInput,
  eSpecularInput,
  eDenoisedDiffuse,
  eDenoisedSpecular,
  eNormalRoughness,
  eViewZ,
  eMotionVectors,
};

struct DenoiserSettings
{
  uint32_t maxAccumulatedFrames     = 30;
  uint32_t maxFastAccumulatedFrames = 6;
  float    diffusePrepassBlurRadius = 30.0f;
  float    specularPrepassBlurRadius = 50.0f;
  // Larger values preserve more temporal history during motion, but can allow ghosting.
  float    disocclusionThreshold    = 0.01f;
  bool     enableAntiFirefly        = false;
};

inline bool IsAccumulationResolveMode(RenderResolveMode mode)
{
  return mode == RenderResolveMode::eAccumulate;
}

inline bool IsDenoiseResolveMode(RenderResolveMode mode)
{
  return mode == RenderResolveMode::eDenoise;
}

}  // namespace nvsamples
