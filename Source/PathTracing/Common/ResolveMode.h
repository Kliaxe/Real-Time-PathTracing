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
  // On by default, matching NRD's own default since 4.17: it is cheap, and at one
  // sample per pixel the input reliably contains fireflies that the spatial filter
  // would otherwise smear into blobs.
  bool     enableAntiFirefly        = true;
  // REBLUR's hit distance normalization, mirroring nrd::ReblurHitDistanceParameters:
  //   f = (A + |viewZ| * B) * lerp(C, 1, specMagicCurve(roughness))
  // and the denoiser sees hitDist / f, saturated to [0, 1].
  //
  // These are scene-scale dependent, which is why they are settings rather than
  // constants. At the defaults the diffuse factor is only A + 0.1*|viewZ|, so on a
  // scene whose transport is longer than a few units every hit distance normalizes
  // to 1 and REBLUR picks its widest blur for every pixel - which reads as shadows
  // dissolving. Raising A (and B for depth-dependent scenes) is the lever for that.
  //
  // The shaders normalize hit distances with the SAME values, fed to them by the
  // renderer, because NRD's filter and the front-end packing must agree on the
  // normalization or the denoiser interprets the signal on a different scale.
  float    hitDistanceA            = 3.0f;
  float    hitDistanceB            = 0.1f;
  float    hitDistanceC            = 20.0f;
  // Widest spatial blur REBLUR may apply, in pixels.
  float    maxBlurRadius           = 30.0f;
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
