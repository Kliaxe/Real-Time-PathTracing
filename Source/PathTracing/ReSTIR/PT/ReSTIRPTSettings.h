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

struct ReSTIRPTSettings
{
  // Shared baseline knobs that future ReSTIR methods can inherit unchanged.
  ReSTIRSettings                           common;
  // PT-specific receiver compatibility thresholds used before shifting a
  // reused candidate onto the current receiver.
  shaderio::ReSTIRPTReconnectionParameters reconnection = GetDefaultReSTIRPTReconnectionParameters();
};

}  // namespace nvsamples
