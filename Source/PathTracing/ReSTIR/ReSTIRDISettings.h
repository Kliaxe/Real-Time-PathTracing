#pragma once

#include <cstdint>

#include "PathTracing/Common/ResolveMode.h"
#include "PathTracing/ReSTIR/ReSTIRDIContext.h"
#include "Shaders/ShaderIo.h"

namespace nvsamples
{

// ReSTIR DI can run without reuse, with temporal reuse, with spatial reuse, or with both.
enum class ReSTIRDIResamplingMode : uint32_t
{
  eNone = 0,
  eTemporal,
  eSpatial,
  eTemporalAndSpatial,
};

struct ReSTIRDICommonSettings
{
  RenderResolveMode          resolveMode    = RenderResolveMode::eOff;
  ReSTIRDIResamplingMode     resamplingMode = ReSTIRDIResamplingMode::eTemporalAndSpatial;
  shaderio::ReSTIRDebugView  debugView      = shaderio::eReSTIRDebugViewDisabled;
  DenoiserDebugView          denoiserDebugView = DenoiserDebugView::eFinal;
  DenoiserSettings           denoiserSettings{};
};

inline ReSTIRDIInitialSamplingParameters GetDefaultReSTIRDIInitialSamplingParameters()
{
  ReSTIRDIInitialSamplingParameters parameters = restir::GetDefaultReSTIRDIInitialSamplingParams();
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
  ReSTIRDICommonSettings              common;
  ReSTIRDIInitialSamplingParameters   initialSampling   = GetDefaultReSTIRDIInitialSamplingParameters();
  ReSTIRDITemporalResamplingParameters temporalResampling = GetDefaultReSTIRDITemporalParameters();
  ReSTIRDISpatialResamplingParameters spatialResampling = GetDefaultReSTIRDISpatialParameters();
  ReSTIRDIShadingParameters           shading           = GetDefaultReSTIRDIShadingParameters();
  uint32_t                            continuationMaxBounces = 3u;
};

}  // namespace nvsamples
