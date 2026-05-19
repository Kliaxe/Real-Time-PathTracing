#pragma once

#include <cstdint>

#include <ReSTIR/Parameters.h>

namespace nvsamples
{

ReSTIRReservoirBufferParameters CalculateReservoirBufferParameters(uint32_t renderWidth, uint32_t renderHeight);

// Stable integer hash used to decorrelate per-frame ReSTIR sampling.
uint32_t JenkinsHash(uint32_t a);

}  // namespace nvsamples
