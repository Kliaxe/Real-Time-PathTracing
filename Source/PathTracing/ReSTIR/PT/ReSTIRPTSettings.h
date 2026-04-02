#pragma once

#include "PathTracing/ReSTIR/Common/ReSTIRSettings.h"

namespace nvsamples
{

inline shaderio::ReSTIRPTReconnectionParameters GetDefaultReSTIRPTReconnectionParameters()
{
  // The thesis baseline keeps one reconnection model: fixed thresholds plus
  // optional visibility validation.
  return shaderio::ReSTIRPTReconnectionParameters{
      .roughnessThreshold = 0.1f,
      .distanceThreshold  = 0.0f,
  };
}

inline ReSTIRSettings GetDefaultReSTIRPTSettings()
{
  ReSTIRSettings settings = {};
  // The local RTXDI PT reference keeps fallback sampling on but defaults
  // permutation sampling to off.
  settings.temporalResampling.enablePermutationSampling = 0u;
  settings.temporalResampling.maxReservoirAge           = 1u;
  settings.spatialResampling.samplingRadius             = 10.0f;
  return settings;
}

struct ReSTIRPTSettings
{
  // Shared baseline knobs that future ReSTIR methods can inherit unchanged.
  ReSTIRSettings                           common = GetDefaultReSTIRPTSettings();
  // PT-specific receiver compatibility thresholds used before shifting a
  // reused candidate onto the current receiver.
  shaderio::ReSTIRPTReconnectionParameters reconnection = GetDefaultReSTIRPTReconnectionParameters();
};

}  // namespace nvsamples
