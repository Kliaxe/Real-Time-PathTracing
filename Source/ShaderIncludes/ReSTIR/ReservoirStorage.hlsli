#ifndef RESTIR_DI_RESERVOIR_STORAGE_HLSLI
#define RESTIR_DI_RESERVOIR_STORAGE_HLSLI

#include "ReSTIR/Reservoir.hlsli"

#ifndef RESTIR_LIGHT_RESERVOIR_BUFFER
#error "RESTIR_LIGHT_RESERVOIR_BUFFER must be defined to point to a RWStructuredBuffer<ReSTIRPackedDIReservoir> type resource"
#endif

// Storage helpers for the three reservoir arrays owned by ReSTIRDIResources.
// The CPU builds the matching block-linear pitches in ReSTIRDIParameterContext.

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
    // Reservoirs currently use one storage entry per screen pixel.
    return pixelPosition;
}

uint ReservoirPositionToPointer(
    ReSTIRReservoirBufferParameters reservoirParams,
    uint2 reservoirPosition,
    uint reservoirArrayIndex)
{
    // Block-linear addressing keeps nearby pixels close in memory for spatial reuse.
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
    // Permutation sampling rotates 4x4 pixel groups to reduce temporal reuse correlation.
    int2 offset = int2(uniformRandomNumber & 3, (uniformRandomNumber >> 2) & 3);
    prevPixelPos += offset;

    prevPixelPos.x ^= 3;
    prevPixelPos.y ^= 3;

    prevPixelPos -= offset;
}

ReSTIRPackedDIReservoir PackDIReservoir(const ReSTIRDIReservoir reservoir)
{
    // Clamp side-band visibility metadata to the compact packed representation.
    int2 clampedSpatialDistance = clamp(reservoir.spatialDistance, -ReSTIRPackedDIReservoirMaxDistance, ReSTIRPackedDIReservoirMaxDistance);
    uint clampedAge = clamp(reservoir.age, 0, ReSTIRPackedDIReservoirMaxAge);

    ReSTIRPackedDIReservoir data;
    data.lightData = reservoir.lightData;
    data.uvData = reservoir.uvData;

    data.mVisibility = reservoir.packedVisibility
        | (min(uint(reservoir.M), ReSTIRPackedDIReservoirMaxM) << ReSTIRPackedDIReservoirMShift);

    // Distance is stored as signed 8-bit fields and age as an unsigned 8-bit field.
    data.distanceAge =
          ((clampedSpatialDistance.x & ReSTIRPackedDIReservoirDistanceMask) << ReSTIRPackedDIReservoirDistanceXShift)
        | ((clampedSpatialDistance.y & ReSTIRPackedDIReservoirDistanceMask) << ReSTIRPackedDIReservoirDistanceYShift)
        | (clampedAge << ReSTIRPackedDIReservoirAgeShift);

    data.targetPdf = reservoir.targetPdf;
    data.weight = reservoir.weightSum;

    return data;
}

void StoreDIReservoir(
    const ReSTIRDIReservoir reservoir,
    ReSTIRReservoirBufferParameters reservoirParams,
    uint2 reservoirPosition,
    uint reservoirArrayIndex)
{
    // Storage always goes through the same packed layout used by all ReSTIR passes.
    uint pointer = ReservoirPositionToPointer(reservoirParams, reservoirPosition, reservoirArrayIndex);
    RESTIR_LIGHT_RESERVOIR_BUFFER[pointer] = PackDIReservoir(reservoir);
}

ReSTIRDIReservoir UnpackDIReservoir(ReSTIRPackedDIReservoir data)
{
    // Unpack the compact GPU representation back into the algorithm-friendly form.
    ReSTIRDIReservoir res;
    res.lightData = data.lightData;
    res.uvData = data.uvData;
    res.targetPdf = data.targetPdf;
    res.weightSum = data.weight;
    res.M = (data.mVisibility >> ReSTIRPackedDIReservoirMShift) & ReSTIRPackedDIReservoirMaxM;
    res.packedVisibility = data.mVisibility & ReSTIRPackedDIReservoirVisibilityMask;
    // Sign extend the packed 8-bit spatial offsets back to int values.
    res.spatialDistance.x = int(data.distanceAge << (32 - ReSTIRPackedDIReservoirDistanceXShift - ReSTIRPackedDIReservoirDistanceChannelBits)) >> (32 - ReSTIRPackedDIReservoirDistanceChannelBits);
    res.spatialDistance.y = int(data.distanceAge << (32 - ReSTIRPackedDIReservoirDistanceYShift - ReSTIRPackedDIReservoirDistanceChannelBits)) >> (32 - ReSTIRPackedDIReservoirDistanceChannelBits);
    res.age = (data.distanceAge >> ReSTIRPackedDIReservoirAgeShift) & ReSTIRPackedDIReservoirMaxAge;

    // Discard reservoirs that have Inf/NaN before they can poison later reuse.
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
    // Every pass chooses which reservoir array to read through reservoirArrayIndex.
    uint pointer = ReservoirPositionToPointer(reservoirParams, reservoirPosition, reservoirArrayIndex);
    return UnpackDIReservoir(RESTIR_LIGHT_RESERVOIR_BUFFER[pointer]);
}

#endif // RESTIR_DI_RESERVOIR_STORAGE_HLSLI
