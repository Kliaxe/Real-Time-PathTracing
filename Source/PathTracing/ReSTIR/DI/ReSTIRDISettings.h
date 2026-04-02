#pragma once

#include "PathTracing/ReSTIR/Common/ReSTIRSettings.h"
#include "ReSTIR/DI/ReSTIRDI.h"

namespace nvsamples
{

inline ReSTIRDIInitialSamplingParameters GetDefaultReSTIRDIInitialSamplingParameters()
{
  ReSTIRDIInitialSamplingParameters parameters = restir::GetDefaultReSTIRDIInitialSamplingParams();
  // The thesis DI path keeps one canonical explicit environment sampler that
  // matches the ground-truth tracer's HDRI proposal.
  parameters.numInfiniteLightSamples = 0;
  parameters.numBrdfSamples = 1;
  return parameters;
}

inline ReSTIRDITemporalResamplingParameters GetDefaultReSTIRDITemporalParameters()
{
  return restir::GetDefaultReSTIRDITemporalResamplingParams();
}

inline ReSTIRDISpatialResamplingParameters GetDefaultReSTIRDISpatialParameters()
{
  ReSTIRDISpatialResamplingParameters parameters = restir::GetDefaultReSTIRDISpatialResamplingParams();
  parameters.samplingRadius = 10.0f;
  return parameters;
}

inline ReSTIRDIShadingParameters GetDefaultReSTIRDIShadingParameters()
{
  return restir::GetDefaultReSTIRDIShadingParams();
}

struct ReSTIRDISettings
{
  ReSTIRMethodSettings                common;
  ReSTIRDIInitialSamplingParameters   initialSampling   = GetDefaultReSTIRDIInitialSamplingParameters();
  ReSTIRDITemporalResamplingParameters temporalResampling = GetDefaultReSTIRDITemporalParameters();
  ReSTIRDISpatialResamplingParameters spatialResampling = GetDefaultReSTIRDISpatialParameters();
  ReSTIRDIShadingParameters           shading           = GetDefaultReSTIRDIShadingParameters();
  uint32_t                            continuationMaxBounces = 3u;
};

}  // namespace nvsamples
