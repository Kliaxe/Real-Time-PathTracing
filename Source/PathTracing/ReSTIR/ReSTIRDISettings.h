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
  // Resolve mode decides whether the output is raw, accumulated, or denoised.
  RenderResolveMode          resolveMode    = RenderResolveMode::eOff;
  // Resampling mode controls which ReSTIR reuse passes are recorded.
  ReSTIRDIResamplingMode     resamplingMode = ReSTIRDIResamplingMode::eTemporalAndSpatial;
  // ReSTIR debug views replace the beauty image before denoising.
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
  // These parameter structs are shared with Slang through ShaderIo/ReSTIR headers.
  ReSTIRDIInitialSamplingParameters   initialSampling   = GetDefaultReSTIRDIInitialSamplingParameters();
  ReSTIRDITemporalResamplingParameters temporalResampling = GetDefaultReSTIRDITemporalParameters();
  ReSTIRDISpatialResamplingParameters spatialResampling = GetDefaultReSTIRDISpatialParameters();
  ReSTIRDIShadingParameters           shading           = GetDefaultReSTIRDIShadingParameters();
  // ReSTIR handles direct lighting; extra path bounces are traced during final shading.
  uint32_t                            secondaryPathMaxBounces = 3u;
};

}  // namespace nvsamples
