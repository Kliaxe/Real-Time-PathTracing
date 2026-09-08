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

// Mirrors nrd::HitDistanceReconstructionMode. Declared here rather than including
// the NRD headers, because this file is pulled in by renderer settings that have no
// other reason to know NRD exists.
enum class HitDistanceReconstructionMode : uint32_t
{
  // The renderer supplies a valid hit distance for both lobes every frame.
  eOff = 0,
  // The renderer picks ONE lobe per pixel per frame and leaves the other's hit
  // distance at zero; NRD reconstructs the gaps from a 3x3 or 5x5 neighbourhood.
  // NRD asks for lobe probabilities clamped to [1/4, 3/4] and Bayer dithering to
  // guarantee a donor in a 3x3 area, neither of which a BSDF-sampled path tracer
  // does, so 5x5 is the honest choice: the wider search is what makes an unclamped,
  // white-noise lobe draw survive.
  eArea3x3 = 1,
  eArea5x5 = 2,
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
  // Off by default: a renderer that always has both hit distances must not pay for
  // reconstruction, and telling NRD that gaps are expected when they are not makes
  // it repair signals that were already correct.
  HitDistanceReconstructionMode hitDistanceReconstructionMode = HitDistanceReconstructionMode::eOff;
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
