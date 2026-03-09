#pragma once

#include <cstdint>

#include "Shaders/ShaderIo.h"

namespace nvsamples
{

// Shared ReSTIR resampling topology used by all thesis methods.
enum class ReSTIRResamplingMode : uint32_t
{
  eNone = 0,
  eTemporal,
  eSpatial,
  eTemporalAndSpatial,
};

inline shaderio::ReSTIRInitialSamplingParameters GetDefaultReSTIRInitialSamplingParameters()
{
  // The baseline starts with one candidate per pixel so later methods can
  // justify higher counts explicitly.
  return shaderio::ReSTIRInitialSamplingParameters{
      .numInitialSamples = 1u,
      .maxBounceDepth    = 3u,
  };
}

inline shaderio::ReSTIRTemporalResamplingParameters GetDefaultReSTIRTemporalResamplingParameters()
{
  // Conservative defaults keep the baseline readable and stable for
  // side-by-side comparisons.
  return shaderio::ReSTIRTemporalResamplingParameters{
      .depthThreshold  = 0.1f,
      .normalThreshold = 0.6f,
      .maxHistoryLength = 8u,
      .maxReservoirAge  = 30u,
  };
}

inline shaderio::ReSTIRSpatialResamplingParameters GetDefaultReSTIRSpatialResamplingParameters()
{
  // One spatial sample keeps the baseline close to the canonical minimal form.
  return shaderio::ReSTIRSpatialResamplingParameters{
      .numSpatialSamples = 1u,
      .samplingRadius    = 32.0f,
      .normalThreshold   = 0.6f,
      .depthThreshold    = 0.1f,
  };
}

struct ReSTIRSettings
{
  // Final-image accumulation after replay. Reservoir history is tracked
  // separately, so debugging can disable accumulation without disabling reuse.
  bool                                       accumulate                 = false;
  // Selects which reuse passes run between initial candidate generation and
  // final replay.
  ReSTIRResamplingMode                       resamplingMode             = ReSTIRResamplingMode::eTemporalAndSpatial;
  // Shared baseline knobs used by all future ReSTIR-family methods.
  shaderio::ReSTIRInitialSamplingParameters  initialSampling            = GetDefaultReSTIRInitialSamplingParameters();
  shaderio::ReSTIRTemporalResamplingParameters temporalResampling       = GetDefaultReSTIRTemporalResamplingParameters();
  shaderio::ReSTIRSpatialResamplingParameters spatialResampling         = GetDefaultReSTIRSpatialResamplingParameters();
  // Compute-stage visibility checks use ray queries and can be disabled for
  // controlled estimator comparisons.
  bool                                       enableVisibilityValidation = true;
  // Shared debug slot so future ReSTIR methods can present the same UI hook.
  shaderio::ReSTIRDebugView                  debugView                  = shaderio::eReSTIRDebugViewDisabled;
};

}  // namespace nvsamples
