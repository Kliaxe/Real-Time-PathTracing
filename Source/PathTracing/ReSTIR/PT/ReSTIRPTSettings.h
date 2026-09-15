#pragma once

#include <cstdint>

#include "PathTracing/Common/ResolveMode.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTParameterContext.h"
#include "Shaders/ShaderIo.h"

namespace rtpt
{

// ReSTIRPTCommonSettings
// User-facing switches for the whole ReSTIR PT Enhanced renderer.
// Deliberately small. Several knobs a ReSTIR DI implementation would need have no counterpart here, and are absent by construction rather than by omission.
// Bias correction mode: DI picks between 1/M, MIS-like, and ray-traced normalization. PT's bias correction is the shift Jacobian plus pairwise MIS; it is structural, not a mode.
// The five visibility knobs: a DI reservoir stores a light whose visibility is unknown, so DI defers and caches shadow rays. A PT reservoir stores a path that was actually traced, so it is already visibility-resolved.
// Permutation sampling is superseded by duplication maps (Section 5), and disocclusion boost by dual motion vectors (Section 6.4).

struct ReSTIRPTCommonSettings
{
  // Resolve mode decides whether the output is raw, accumulated, or denoised.
  RenderResolveMode         resolveMode    = RenderResolveMode::eOff;
  // Resampling mode controls which reuse passes are recorded.
  ReSTIRPTResamplingMode    resamplingMode = ReSTIRPTResamplingMode::eTemporalAndSpatial;
  // Substitute the plain path-traced radiance for the resampled estimate, leaving every other step of the frame identical.
  // The correctness gate: with reuse off the two must converge to the same image. It replaces the beauty image, so the denoiser stands down while it is on.
  bool                      referencePathTracer = false;
  // Which NRD input or output is shown when resolving with the denoiser.
  DenoiserDebugView         denoiserDebugView = DenoiserDebugView::eFinal;
  // Initial sampling draws ONE BSDF lobe at the primary hit, so each frame only one of the two hit distances exists and the other is zero, which is what NRD asks for and what it reconstructs from neighbours.
  // The reference tracer uses the same reconstruction mode because it samples only one primary lobe too.
  DenoiserSettings          denoiserSettings { .hitDistanceReconstructionMode = HitDistanceReconstructionMode::eArea5x5 };
  // Section 6.2.2, "Stream Compaction for Random Replay". 0 = off, non-zero = on. Compacts the spatial pre-pass into a work list of the (pixel, slot) pairs that need a shift and traces it indirectly, instead of launching every (pixel, slot).
  // The paper: "We parallelize over pixel-neighbor pairs, applying stream compaction to discard pairs that do not require replay. This reduces warp divergence and the number of active warps, yielding a substantial speedup." That is this pass. ON, because this is what the paper prescribes and it is worth about 2x.
  // It was off by default for a long time because it diverged - energy growing frame over frame until the image was infinite. The cause was NOT this pass. It was Section 5's alpha (the decorrelation gamma), which at the time was tuned to 0.5 instead of the paper's 0.1 (now the default), and which let high-energy samples spread through reuse unchecked; this pass is simply the most sensitive thing downstream of that. With the paper's alpha, six camera poses on Bunny Metallic that previously diverged at 1200 frames are all clean.
  // Note for anyone re-tuning decorrelation: this pass is the most sensitive consumer of it. If alpha drifts back up, this is what fails first, and it fails by growing without bound rather than by looking noisy.
  // Two things from that investigation are worth keeping, because both cost real time and neither is obvious.
  // This pass and the per-pixel arrangement it replaces are numerically equivalent. A record-by-record readback over 1.44M records found zero differences in validity or outcome and only last-bit differences in values (median 6.3e-08). If they ever disagree by more than that, something else has changed.
  // The failure was invisible to every diagnostic here, because they all measure shift QUALITY - outcomes, Jacobians, reciprocity, the MIS partition - and it lived entirely in weight MAGNITUDE. That is why a ucw tail was added to the per-pass report.
  uint32_t                  sortPrepass         = 1;
};

// Default parameter wrappers
// Settings-side names for the parameter context's defaults, so ReSTIRPTSettings below reads in terms of its own field names.

inline ReSTIRPTInitialSamplingParameters GetDefaultReSTIRPTInitialSamplingParameters()
{
  return GetDefaultReSTIRPTInitialSamplingParams();
}

inline ReSTIRPTShiftParameters GetDefaultReSTIRPTShiftParameters()
{
  return GetDefaultReSTIRPTShiftParams();
}

inline ReSTIRPTTemporalResamplingParameters GetDefaultReSTIRPTTemporalParameters()
{
  return GetDefaultReSTIRPTTemporalResamplingParams();
}

inline ReSTIRPTSpatialResamplingParameters GetDefaultReSTIRPTSpatialParameters()
{
  return GetDefaultReSTIRPTSpatialResamplingParams();
}

inline ReSTIRPTDecorrelationParameters GetDefaultReSTIRPTDecorrelationParameters()
{
  return GetDefaultReSTIRPTDecorrelationParams();
}

inline ReSTIRPTShadingParameters GetDefaultReSTIRPTShadingParameters()
{
  return GetDefaultReSTIRPTShadingParams();
}

inline ReSTIRPTNeeParameters GetDefaultReSTIRPTNeeParameters()
{
  return GetDefaultReSTIRPTNeeParams();
}

// ReSTIRPTSettings
// One settings object maps directly to the GPU parameter block sections.
// Note there is no separate direct-lighting parameter group: Section 6.1 unifies direct and global illumination into a single reservoir, so direct light is just the shortest path the initial resampler can select.

struct ReSTIRPTSettings
{
  // Renderer-level switches that are not part of the GPU parameter block.
  ReSTIRPTCommonSettings               common;

  // Shader parameter blocks
  // These parameter structs are shared with HLSL through ShaderIo/ReSTIR headers.

  // Path length, roulette, and environment importance sampling.
  ReSTIRPTInitialSamplingParameters    initialSampling    = GetDefaultReSTIRPTInitialSamplingParameters();
  // Shift mapping and reconnection criteria (Section 4).
  ReSTIRPTShiftParameters              shift              = GetDefaultReSTIRPTShiftParameters();
  // Temporal reuse cap and reprojection gates (Section 6.4).
  ReSTIRPTTemporalResamplingParameters temporalResampling = GetDefaultReSTIRPTTemporalParameters();
  // Spatial reuse radius, pairing, and similarity gates (Section 3). pairingSigma is derived, and mirrored back by the renderer each frame.
  ReSTIRPTSpatialResamplingParameters  spatialResampling  = GetDefaultReSTIRPTSpatialParameters();
  // Duplication-map cap reduction (Section 5).
  ReSTIRPTDecorrelationParameters      decorrelation      = GetDefaultReSTIRPTDecorrelationParameters();
  // Vector-valued shading weights (Section 6.3).
  ReSTIRPTShadingParameters            shading            = GetDefaultReSTIRPTShadingParameters();
  // Light-tile NEE candidates (Section 6.1).
  ReSTIRPTNeeParameters                nee                = GetDefaultReSTIRPTNeeParameters();
};

}  // namespace rtpt
