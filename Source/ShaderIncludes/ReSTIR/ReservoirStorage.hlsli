#ifndef RESTIR_DI_RESERVOIR_STORAGE_HLSLI
#define RESTIR_DI_RESERVOIR_STORAGE_HLSLI

#include "ReSTIR/Reservoir.hlsli"

// Define this macro to 0 if your shader needs read-only access to the reservoirs,
// to avoid compile errors in the StoreDIReservoir function
#ifndef RESTIR_ENABLE_STORE_RESERVOIR
#define RESTIR_ENABLE_STORE_RESERVOIR 1
#endif

#ifndef RESTIR_LIGHT_RESERVOIR_BUFFER
#error "RESTIR_LIGHT_RESERVOIR_BUFFER must be defined to point to a RWStructuredBuffer<ReSTIRPackedDIReservoir> type resource"
#endif

// Encoding helper constants for ReSTIRPackedDIReservoir.distanceAge
static const uint ReSTIRPackedDIReservoirDistanceChannelBits = 8;
static const uint ReSTIRPackedDIReservoirDistanceXShift = 0;
static const uint ReSTIRPackedDIReservoirDistanceYShift = 8;
static const uint ReSTIRPackedDIReservoirAgeShift = 16;
static const uint ReSTIRPackedDIReservoirMaxAge = 0xff;
static const uint ReSTIRPackedDIReservoirDistanceMask = (1u << ReSTIRPackedDIReservoirDistanceChannelBits) - 1;
static const  int ReSTIRPackedDIReservoirMaxDistance = int((1u << (ReSTIRPackedDIReservoirDistanceChannelBits - 1)) - 1);

uint2 PixelPosToReservoirPos(uint2 pixelPosition)
{
    return pixelPosition;
}

uint ReservoirPositionToPointer(
    ReSTIRReservoirBufferParameters reservoirParams,
    uint2 reservoirPosition,
    uint reservoirArrayIndex)
{
    uint2 blockIdx = reservoirPosition / RESTIR_RESERVOIR_BLOCK_SIZE;
    uint2 positionInBlock = reservoirPosition % RESTIR_RESERVOIR_BLOCK_SIZE;

    return reservoirArrayIndex * reservoirParams.reservoirArrayPitch
        + blockIdx.y * reservoirParams.reservoirBlockRowPitch
        + blockIdx.x * (RESTIR_RESERVOIR_BLOCK_SIZE * RESTIR_RESERVOIR_BLOCK_SIZE)
        + positionInBlock.y * RESTIR_RESERVOIR_BLOCK_SIZE
        + positionInBlock.x;
}

void ApplyPermutationSampling(inout int2 prevPixelPos, uint uniformRandomNumber)
{
    int2 offset = int2(uniformRandomNumber & 3, (uniformRandomNumber >> 2) & 3);
    prevPixelPos += offset;

    prevPixelPos.x ^= 3;
    prevPixelPos.y ^= 3;

    prevPixelPos -= offset;
}

ReSTIRPackedDIReservoir PackDIReservoir(const ReSTIRDIReservoir reservoir)
{
    int2 clampedSpatialDistance = clamp(reservoir.spatialDistance, -ReSTIRPackedDIReservoirMaxDistance, ReSTIRPackedDIReservoirMaxDistance);
    uint clampedAge = clamp(reservoir.age, 0, ReSTIRPackedDIReservoirMaxAge);

    ReSTIRPackedDIReservoir data;
    data.lightData = reservoir.lightData;
    data.uvData = reservoir.uvData;

    data.mVisibility = reservoir.packedVisibility
        | (min(uint(reservoir.M), ReSTIRPackedDIReservoirMaxM) << ReSTIRPackedDIReservoirMShift);

    data.distanceAge =
          ((clampedSpatialDistance.x & ReSTIRPackedDIReservoirDistanceMask) << ReSTIRPackedDIReservoirDistanceXShift)
        | ((clampedSpatialDistance.y & ReSTIRPackedDIReservoirDistanceMask) << ReSTIRPackedDIReservoirDistanceYShift)
        | (clampedAge << ReSTIRPackedDIReservoirAgeShift);

    data.targetPdf = reservoir.targetPdf;
    data.weight = reservoir.weightSum;

    return data;
}

#if RESTIR_ENABLE_STORE_RESERVOIR
void StoreDIReservoir(
    const ReSTIRDIReservoir reservoir,
    ReSTIRReservoirBufferParameters reservoirParams,
    uint2 reservoirPosition,
    uint reservoirArrayIndex)
{
    uint pointer = ReservoirPositionToPointer(reservoirParams, reservoirPosition, reservoirArrayIndex);
    RESTIR_LIGHT_RESERVOIR_BUFFER[pointer] = PackDIReservoir(reservoir);
}
#endif // RESTIR_ENABLE_STORE_RESERVOIR

ReSTIRDIReservoir UnpackDIReservoir(ReSTIRPackedDIReservoir data)
{
    ReSTIRDIReservoir res;
    res.lightData = data.lightData;
    res.uvData = data.uvData;
    res.targetPdf = data.targetPdf;
    res.weightSum = data.weight;
    res.M = (data.mVisibility >> ReSTIRPackedDIReservoirMShift) & ReSTIRPackedDIReservoirMaxM;
    res.packedVisibility = data.mVisibility & ReSTIRPackedDIReservoirVisibilityMask;
    // Sign extend the shift values
    res.spatialDistance.x = int(data.distanceAge << (32 - ReSTIRPackedDIReservoirDistanceXShift - ReSTIRPackedDIReservoirDistanceChannelBits)) >> (32 - ReSTIRPackedDIReservoirDistanceChannelBits);
    res.spatialDistance.y = int(data.distanceAge << (32 - ReSTIRPackedDIReservoirDistanceYShift - ReSTIRPackedDIReservoirDistanceChannelBits)) >> (32 - ReSTIRPackedDIReservoirDistanceChannelBits);
    res.age = (data.distanceAge >> ReSTIRPackedDIReservoirAgeShift) & ReSTIRPackedDIReservoirMaxAge;

    // Discard reservoirs that have Inf/NaN
    if (isinf(res.weightSum) || isnan(res.weightSum)) {
        res = EmptyDIReservoir();
    }

    return res;
}

ReSTIRDIReservoir LoadDIReservoir(
    ReSTIRReservoirBufferParameters reservoirParams,
    uint2 reservoirPosition,
    uint reservoirArrayIndex)
{
    uint pointer = ReservoirPositionToPointer(reservoirParams, reservoirPosition, reservoirArrayIndex);
    return UnpackDIReservoir(RESTIR_LIGHT_RESERVOIR_BUFFER[pointer]);
}

#endif // RESTIR_DI_RESERVOIR_STORAGE_HLSLI
