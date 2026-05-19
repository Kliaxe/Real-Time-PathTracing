#pragma once

#include <cstdint>

#include "PathTracing/Common/ResolveMode.h"
#include "PathTracing/ReSTIR/ReSTIRDIParameterContext.h"
#include "Shaders/ShaderIo.h"

namespace nvsamples
{

// User-facing switches for the whole ReSTIR DI renderer.
struct ReSTIRDICommonSettings
{
  RenderResolveMode          resolveMode    = RenderResolveMode::eOff;
  ReSTIRDIResamplingMode     resamplingMode = ReSTIRDIResamplingMode::eTemporalAndSpatial;
  shaderio::ReSTIRDebugView debugView         = shaderio::eReSTIRDebugViewDisabled;
  DenoiserDebugView         denoiserDebugView = DenoiserDebugView::eFinal;
  DenoiserSettings          denoiserSettings{};
};

inline ReSTIRDIInitialSamplingParameters GetDefaultReSTIRDIInitialSamplingParameters()
{
  return GetDefaultReSTIRDIInitialSamplingParams();
}

inline ReSTIRDITemporalResamplingParameters GetDefaultReSTIRDITemporalParameters()
{
  return GetDefaultReSTIRDITemporalResamplingParams();
}

inline ReSTIRDISpatialResamplingParameters GetDefaultReSTIRDISpatialParameters()
{
  return GetDefaultReSTIRDISpatialResamplingParams();
}

inline ReSTIRDIShadingParameters GetDefaultReSTIRDIShadingParameters()
{
  return GetDefaultReSTIRDIShadingParams();
}

// One settings object maps directly to the GPU parameter block sections.
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
