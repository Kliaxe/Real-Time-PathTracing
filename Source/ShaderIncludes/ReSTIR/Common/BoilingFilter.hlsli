#ifndef RESTIR_BOILING_FILTER_HLSLI
#define RESTIR_BOILING_FILTER_HLSLI

#include "ReSTIR/Common/ReSTIRTypes.h"

#ifdef RESTIR_ENABLE_BOILING_FILTER

#ifndef RESTIR_BOILING_FILTER_GROUP_SIZE
#error "RESTIR_BOILING_FILTER_GROUP_SIZE must be defined before including BoilingFilter.hlsli"
#endif

#define RESTIR_BOILING_FILTER_MIN_LANE_COUNT 32

groupshared float s_BoilingFilterWeights[(RESTIR_BOILING_FILTER_GROUP_SIZE * RESTIR_BOILING_FILTER_GROUP_SIZE \
                                          + RESTIR_BOILING_FILTER_MIN_LANE_COUNT - 1) / RESTIR_BOILING_FILTER_MIN_LANE_COUNT];
groupshared uint  s_BoilingFilterCounts[(RESTIR_BOILING_FILTER_GROUP_SIZE * RESTIR_BOILING_FILTER_GROUP_SIZE \
                                         + RESTIR_BOILING_FILTER_MIN_LANE_COUNT - 1) / RESTIR_BOILING_FILTER_MIN_LANE_COUNT];

bool ReSTIRBoilingFilterInternal(uint2 localIndex, float filterStrength, float reservoirWeight)
{
  const float boilingFilterMultiplier = 10.0 / clamp(filterStrength, 1.0e-6, 1.0) - 9.0;

  float waveWeight = WaveActiveSum(reservoirWeight);
  uint  waveCount  = WaveActiveCountBits(reservoirWeight > 0.0);

  const uint linearThreadIndex = localIndex.x + localIndex.y * RESTIR_BOILING_FILTER_GROUP_SIZE;
  const uint waveIndex         = linearThreadIndex / WaveGetLaneCount();

  if(WaveIsFirstLane())
  {
    s_BoilingFilterWeights[waveIndex] = waveWeight;
    s_BoilingFilterCounts[waveIndex]  = waveCount;
  }

  GroupMemoryBarrierWithGroupSync();

  const uint reductionLaneCount =
      (RESTIR_BOILING_FILTER_GROUP_SIZE * RESTIR_BOILING_FILTER_GROUP_SIZE + WaveGetLaneCount() - 1) / WaveGetLaneCount();
  if(linearThreadIndex < reductionLaneCount)
  {
    waveWeight = s_BoilingFilterWeights[linearThreadIndex];
    waveCount  = s_BoilingFilterCounts[linearThreadIndex];

    waveWeight = WaveActiveSum(waveWeight);
    waveCount  = WaveActiveSum(waveCount);

    if(linearThreadIndex == 0u)
    {
      s_BoilingFilterWeights[0] = (waveCount > 0u) ? (waveWeight / float(waveCount)) : 0.0;
    }
  }

  GroupMemoryBarrierWithGroupSync();

  const float averageNonzeroWeight = s_BoilingFilterWeights[0];
  return reservoirWeight > averageNonzeroWeight * boilingFilterMultiplier;
}

#endif  // RESTIR_ENABLE_BOILING_FILTER

#endif  // RESTIR_BOILING_FILTER_HLSLI
