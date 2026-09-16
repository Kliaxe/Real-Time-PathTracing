#pragma once

#include <cstdint>

namespace rtpt
{

// RenderResolveMode
// Controls how a noisy path-traced frame is resolved into the displayed image.
// Shared by the reference path tracer and ReSTIR PT so both renderers expose the same choice.
// The two denoisers are separate modes rather than a denoiser setting, because they consume different inputs and keep separate histories: switching between them is a restart either way.

enum class RenderResolveMode : uint32_t
{
  eOff = 0,
  eAccumulate,
  // NRD REBLUR on the demodulated diffuse and specular signals, composed back into the HDR target.
  eDenoiseNrd,
  // DLSS Ray Reconstruction on the full noisy radiance and guide buffers, written straight to the HDR target. Needs Streamline and an NVIDIA RTX GPU.
  eDenoiseRayReconstruction,
};

// DenoiserDebugView
// Shared denoiser inspection modes.
// Individual renderers can expose only the views they actually support while still reusing the same UI shell.

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

// HitDistanceReconstructionMode
// Mirrors nrd::HitDistanceReconstructionMode.
// Declared here rather than including the NRD headers, because this file is pulled in by renderer settings that have no other reason to know NRD exists.

enum class HitDistanceReconstructionMode : uint32_t
{
  // The renderer supplies a valid hit distance for both lobes every frame.
  eOff = 0,
  // The renderer picks ONE lobe per pixel per frame and leaves the other's hit distance at zero; NRD reconstructs the gaps from a 3x3 or 5x5 neighbourhood.
  // NRD asks for lobe probabilities clamped to [1/4, 3/4] and Bayer dithering to guarantee a donor in a 3x3 area, neither of which a BSDF-sampled path tracer does, so 5x5 is the honest choice: the wider search is what makes an unclamped, white-noise lobe draw survive.
  eArea3x3 = 1,
  eArea5x5 = 2,
};

// DenoiserSettings
// User-tunable NRD REBLUR settings shared by every renderer that denoises.
// NrdDenoiser translates them into nrd::ReblurSettings, and the renderers forward the hit distance normalization to their shaders so both sides agree.
// The defaults follow NVIDIA's RTXPT sample (Rtxpt/NRD/NrdConfig.cpp and SampleUI.h) wherever it departs from NRD's own defaults: longer history, smaller prepass blurs, a looser disocclusion threshold, and a radiance clamp before NRD. RTXPT's per-pixel alternate threshold is available but off; see enableDisocclusionThresholdMix.

struct DenoiserSettings
{
  // Upper bound on REBLUR's temporal history length, in frames. RTXPT uses 50 against NRD's 30.
  uint32_t maxAccumulatedFrames     = 50;
  // History length REBLUR's fast responsive history is capped at, in frames.
  uint32_t maxFastAccumulatedFrames = 6;
  // Radius of REBLUR's diffuse pre-pass blur, in pixels. RTXPT halves NRD's 30 "to reduce loss of sharp shadows".
  float    diffusePrepassBlurRadius = 15.0f;
  // Radius of REBLUR's specular pre-pass blur, in pixels. RTXPT uses 40 against NRD's 50.
  float    specularPrepassBlurRadius = 40.0f;
  // Larger values preserve more temporal history during motion, but can allow ghosting. RTXPT uses 0.03 against NRD's 0.01.
  float    disocclusionThreshold    = 0.03f;
  // The looser threshold NRD mixes toward per pixel, by the amount in IN_DISOCCLUSION_THRESHOLD_MIX. RTXPT uses 0.2.
  float    disocclusionThresholdAlternate = 0.2f;
  // Whether the disocclusion threshold mix is produced and handed to NRD. Off, every pixel uses disocclusionThreshold alone.
  // Off by default, unlike RTXPT: it only relaxes surfaces seen through mirrors and glass, while here the mix applies to every surface, and at object silhouettes that keeps stale history as a visible rim along the edge.
  bool     enableDisocclusionThresholdMix = false;
  // Radiance clamp before NRD, as a multiple of the luminance the tonemapper maps to middle grey: demodulated luminance is capped at min(255, grey * K * 16), matching RTXPT's DenoiserRadianceClampK. Zero disables the clamp.
  // The 255 ceiling is RTXPT's too: NRD stores squared luminance in half floats internally, and larger values overflow.
  float    radianceClampK = 8.0f;
  // On by default, matching NRD's own default since 4.17: it is cheap, and at one sample per pixel the input reliably contains fireflies that the spatial filter would otherwise smear into blobs.
  bool     enableAntiFirefly        = true;
  // REBLUR's hit distance normalization, mirroring nrd::ReblurHitDistanceParameters: f = (A + |viewZ| * B) * lerp(C, 1, specMagicCurve(roughness)), and the denoiser sees hitDist / f, saturated to [0, 1].
  // These are scene-scale dependent, which is why they are settings rather than constants. At the defaults the diffuse factor is only A + 0.1*|viewZ|, so on a scene whose transport is longer than a few units every hit distance normalizes to 1 and REBLUR picks its widest blur for every pixel - which reads as shadows dissolving. Raising A (and B for depth-dependent scenes) is the lever for that.
  // The shaders normalize hit distances with the SAME values, fed to them by the renderer, because NRD's filter and the front-end packing must agree on the normalization or the denoiser interprets the signal on a different scale.
  float    hitDistanceA            = 3.0f;
  // Depth-proportional term B of the normalization above.
  float    hitDistanceB            = 0.1f;
  // Roughness-dependent scale C of the normalization above.
  float    hitDistanceC            = 20.0f;
  // Widest spatial blur REBLUR may apply, in pixels.
  float    maxBlurRadius           = 30.0f;
  // Off by default: a renderer that always has both hit distances must not pay for reconstruction, and telling NRD that gaps are expected when they are not makes it repair signals that were already correct.
  HitDistanceReconstructionMode hitDistanceReconstructionMode = HitDistanceReconstructionMode::eOff;
};

// RayReconstructionPreset
// Which DLSS Ray Reconstruction model runs. StreamlineRuntime maps these to Streamline's preset values, so the numbering here is free.

enum class RayReconstructionPreset : uint32_t
{
  // Whatever the installed DLSS library considers current; it can change when the DLL is updated.
  eDefault = 0,
  // Transformer model D.
  eD,
  // Transformer model E, RTXPT's choice.
  eE,
};

// RayReconstructionSettings
// User-tunable DLSS Ray Reconstruction settings shared by every renderer that denoises. The resolution mode is fixed to DLAA: the renderers always render at the output resolution.

struct RayReconstructionSettings
{
  // Model to run. Default follows the DLSS library rather than pinning one.
  RayReconstructionPreset preset = RayReconstructionPreset::eDefault;
  // Radiance clamp on the noisy color before Ray Reconstruction, as a multiple of the luminance the tonemapper maps to middle grey, applied to the brightest channel so hue is kept. Zero disables it.
  // RTXPT's DLSSRRBrightnessClampK, including its 4096 default: far looser than the NRD clamp, because Ray Reconstruction sees undemodulated radiance and handles outliers itself - the clamp only catches the rare sample bright enough to survive the model as a blotch.
  float                   radianceClampK = 4096.0f;
};

// Brightest-channel ceiling the Ray Reconstruction input pass clamps noisy radiance to, or zero when the clamp is disabled. Follows exposure like ComputeDenoiserRadianceClamp.
inline float ComputeRayReconstructionRadianceClamp(const RayReconstructionSettings& settings, float greyLuminance)
{
  return settings.radianceClampK > 0.0f ? greyLuminance * settings.radianceClampK : 0.0f;
}

// Luminance ceiling the shaders clamp demodulated radiance to before writing NRD's inputs, or zero when the clamp is disabled.
// greyLuminance is the scene luminance the tonemapper maps to middle grey, so the clamp follows exposure the way RTXPT's does: a darker exposure lets brighter radiance through.
inline float ComputeDenoiserRadianceClamp(const DenoiserSettings& settings, float greyLuminance)
{
  if(settings.radianceClampK <= 0.0f)
  {
    return 0.0f;
  }

  const float clamp = greyLuminance * settings.radianceClampK * 16.0f;

  return clamp < 255.0f ? clamp : 255.0f;
}

inline bool IsAccumulationResolveMode(RenderResolveMode mode)
{
  return mode == RenderResolveMode::eAccumulate;
}

// True for both denoisers: the renderer writes guide buffers and denoiser signals, and the denoiser owns the output.
inline bool IsDenoiseResolveMode(RenderResolveMode mode)
{
  return mode == RenderResolveMode::eDenoiseNrd || mode == RenderResolveMode::eDenoiseRayReconstruction;
}

inline bool IsNrdResolveMode(RenderResolveMode mode)
{
  return mode == RenderResolveMode::eDenoiseNrd;
}

inline bool IsRayReconstructionResolveMode(RenderResolveMode mode)
{
  return mode == RenderResolveMode::eDenoiseRayReconstruction;
}

}  // namespace rtpt
