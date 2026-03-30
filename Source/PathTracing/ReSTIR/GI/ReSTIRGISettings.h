#pragma once

#include "PathTracing/ReSTIR/Common/ReSTIRSettings.h"
#include "ReSTIR/GI/ReSTIRGI.h"

namespace nvsamples
{

inline ReSTIRGITemporalResamplingParameters GetDefaultReSTIRGITemporalParameters()
{
  return restir::GetDefaultReSTIRGITemporalResamplingParams();
}

inline ReSTIRGISpatialResamplingParameters GetDefaultReSTIRGISpatialParameters()
{
  return restir::GetDefaultReSTIRGISpatialResamplingParams();
}

inline ReSTIRGIShadingParameters GetDefaultReSTIRGIShadingParameters()
{
  return restir::GetDefaultReSTIRGIShadingParams();
}

struct ReSTIRGISettings
{
  ReSTIRMethodSettings                 common;
  ReSTIRGITemporalResamplingParameters temporalResampling = GetDefaultReSTIRGITemporalParameters();
  ReSTIRGISpatialResamplingParameters  spatialResampling  = GetDefaultReSTIRGISpatialParameters();
  ReSTIRGIShadingParameters            shading            = GetDefaultReSTIRGIShadingParameters();
  uint32_t                             continuationMaxBounces = 3u;
};

}  // namespace nvsamples
