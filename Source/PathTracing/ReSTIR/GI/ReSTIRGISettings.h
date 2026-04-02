#pragma once

#include "PathTracing/ReSTIR/Common/ReSTIRSettings.h"
#include "ReSTIR/GI/ReSTIRGI.h"

namespace nvsamples
{

inline ReSTIRGITemporalResamplingParameters GetDefaultReSTIRGITemporalParameters()
{
  ReSTIRGITemporalResamplingParameters parameters = restir::GetDefaultReSTIRGITemporalResamplingParams();
  parameters.maxReservoirAge = 1u;
  return parameters;
}

inline ReSTIRGISpatialResamplingParameters GetDefaultReSTIRGISpatialParameters()
{
  ReSTIRGISpatialResamplingParameters parameters = restir::GetDefaultReSTIRGISpatialResamplingParams();
  parameters.samplingRadius = 10.0f;
  return parameters;
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
