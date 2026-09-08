#pragma once

#include <cstdint>

#include "PathTracing/Common/ResolveMode.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTParameterContext.h"
#include "Shaders/ShaderIo.h"

namespace nvsamples
{

// User-facing switches for the whole ReSTIR PT Enhanced renderer.
//
// Deliberately small. Several knobs a ReSTIR DI implementation would need have no
// counterpart here, and are absent by construction rather than by omission:
//   - bias correction mode: DI picks between 1/M, MIS-like, and ray-traced
//     normalization. PT's bias correction is the shift Jacobian plus pairwise
//     MIS; it is structural, not a mode.
//   - the five visibility knobs: a DI reservoir stores a light whose visibility
//     is unknown, so DI defers and caches shadow rays. A PT reservoir stores a
//     path that was actually traced, so it is already visibility-resolved.
//   - permutation sampling: superseded by duplication maps (Section 5).
//   - disocclusion boost: superseded by dual motion vectors (Section 6.4).
struct ReSTIRPTCommonSettings
{
  // Resolve mode decides whether the output is raw, accumulated, or denoised.
  RenderResolveMode         resolveMode    = RenderResolveMode::eOff;
  // Resampling mode controls which reuse passes are recorded.
  ReSTIRPTResamplingMode    resamplingMode = ReSTIRPTResamplingMode::eTemporalAndSpatial;
  // Substitute the plain path-traced radiance for the resampled estimate, leaving
  // every other step of the frame identical. The correctness gate: with reuse off
  // the two must converge to the same image. It replaces the beauty image, so the
  // denoiser stands down while it is on.
  bool                      referencePathTracer = false;
  DenoiserDebugView         denoiserDebugView = DenoiserDebugView::eFinal;
  // Initial sampling draws ONE BSDF lobe at the primary hit, so each frame only one
  // of the two hit distances exists and the other is zero, which is what NRD asks
  // for and what it reconstructs from neighbours. The path tracer and ReSTIR DI
  // leave this off because both always supply a distance for each lobe.
  DenoiserSettings          denoiserSettings{.hitDistanceReconstructionMode =
                                                 HitDistanceReconstructionMode::eArea5x5};
  // Section 6.2.2. Compact the spatial pre-pass into a work list of pairs that need
  // a shift and trace it indirectly, instead of launching every (pixel, slot).
  //
  // This was off by default for a long time because it diverged - energy growing
  // frame over frame until the image was infinite. The cause was NOT this pass. It
  // was Section 5's alpha, tuned here to 0.5 instead of the paper's 0.1, which let
  // high-energy samples spread through reuse unchecked; this pass is simply the most
  // sensitive thing downstream of that. With the paper's alpha, six camera poses on
  // Bunny Metallic that previously diverged at 1200 frames are all clean.
  //
  // Two things from that investigation are worth keeping, because both cost real
  // time and neither is obvious:
  //
  //  - This pass and the per-pixel arrangement it replaces are numerically
  //    equivalent. A record-by-record readback over 1.44M records found zero
  //    differences in validity or outcome and only last-bit differences in values
  //    (median 6.3e-08). If they ever disagree by more than that, something else
  //    has changed.
  //  - The failure was invisible to every diagnostic here, because they all measure
  //    shift QUALITY - outcomes, Jacobians, reciprocity, the MIS partition - and it
  //    lived entirely in weight MAGNITUDE. That is why the per-pass report now
  //    includes a ucw tail.
  // 0 = off, non-zero = on. ON, because this is what the paper prescribes and it is
  // worth about 2x.
  //
  // Section 6.2.2, "Stream Compaction for Random Replay": "We parallelize over
  // pixel-neighbor pairs, applying stream compaction to discard pairs that do not
  // require replay. This reduces warp divergence and the number of active warps,
  // yielding a substantial speedup." That is this pass.
  //
  // It was off for a long while because it diverged. It did - but the cause was a
  // mistuned Section 5 (decorrelation alpha at 0.5 instead of the paper's 0.1),
  // which let high-energy samples spread through reuse unchecked. With the paper's
  // alpha this pass is stable on all six camera poses that previously broke it.
  //
  // Note for anyone re-tuning decorrelation: this pass is the most sensitive
  // consumer of it. If alpha drifts back up, this is what fails first, and it fails
  // by growing without bound rather than by looking noisy.
  uint32_t                  sortPrepass         = 1;
};

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

// One settings object maps directly to the GPU parameter block sections.
// Note there is no separate direct-lighting parameter group: Section 6.1 unifies
// direct and global illumination into a single reservoir, so direct light is
// just the shortest path the initial resampler can select.
struct ReSTIRPTSettings
{
  ReSTIRPTCommonSettings               common;
  // These parameter structs are shared with Slang through ShaderIo/ReSTIR headers.
  ReSTIRPTInitialSamplingParameters    initialSampling    = GetDefaultReSTIRPTInitialSamplingParameters();
  ReSTIRPTShiftParameters              shift              = GetDefaultReSTIRPTShiftParameters();
  ReSTIRPTTemporalResamplingParameters temporalResampling = GetDefaultReSTIRPTTemporalParameters();
  ReSTIRPTSpatialResamplingParameters  spatialResampling  = GetDefaultReSTIRPTSpatialParameters();
  ReSTIRPTDecorrelationParameters      decorrelation      = GetDefaultReSTIRPTDecorrelationParameters();
  ReSTIRPTShadingParameters            shading            = GetDefaultReSTIRPTShadingParameters();
  ReSTIRPTNeeParameters                nee                = GetDefaultReSTIRPTNeeParameters();
};

}  // namespace nvsamples
