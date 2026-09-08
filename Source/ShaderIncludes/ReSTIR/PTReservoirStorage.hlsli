#ifndef RESTIR_PT_RESERVOIR_STORAGE_HLSLI
#define RESTIR_PT_RESERVOIR_STORAGE_HLSLI

#include "ReSTIR/PTReservoir.hlsli"

#ifndef RESTIR_PT_RESERVOIR_BUFFER
#error "RESTIR_PT_RESERVOIR_BUFFER must be defined to point to a RWStructuredBuffer<ReSTIRPTPackedReservoir> resource"
#endif

// Storage helpers for the rotating reservoir arrays owned by ReSTIRPTResources.
// The CPU builds the matching block-linear pitches in ReSTIRPTParameterContext;
// the two must agree or passes will silently address the wrong pixels.

uint2 PTPixelPosToReservoirPos(uint2 pixelPosition)
{
    // One storage entry per screen pixel.
    return pixelPosition;
}

uint PTReservoirPositionToPointer(
    ReSTIRPTReservoirBufferParameters reservoirParams,
    uint2 reservoirPosition,
    uint reservoirArrayIndex)
{
    // Block-linear addressing keeps a pixel's spatial neighbors close in memory.
    // This matters more for PT than DI: a PT reservoir is 64 bytes against DI's
    // 24, and spatial reuse gathers a random neighborhood of them per pixel.
    const uint2 blockIdx        = reservoirPosition / RESTIR_PT_RESERVOIR_BLOCK_SIZE;
    const uint2 positionInBlock = reservoirPosition % RESTIR_PT_RESERVOIR_BLOCK_SIZE;

    return reservoirArrayIndex * reservoirParams.reservoirArrayPitch
        + blockIdx.y * reservoirParams.reservoirBlockRowPitch
        + blockIdx.x * (RESTIR_PT_RESERVOIR_BLOCK_SIZE * RESTIR_PT_RESERVOIR_BLOCK_SIZE)
        + positionInBlock.y * RESTIR_PT_RESERVOIR_BLOCK_SIZE
        + positionInBlock.x;
}

// Octahedral encoding for the reconnection vertex direction. Two 16-bit unorms
// hold a unit vector to well under a degree of error, which is far below what the
// shift's reconnection tolerance cares about.
uint PackPTOctahedralDirection(float3 direction)
{
    const float3 n = direction / max(abs(direction.x) + abs(direction.y) + abs(direction.z), 1.0e-8);
    float2 p = n.xy;
    if(n.z < 0.0)
    {
        // Fold the lower hemisphere outward across the octahedron's diagonals.
        p = (1.0 - abs(float2(n.y, n.x))) * select(n.xy >= 0.0, float2(1.0), float2(-1.0));
    }

    const uint2 quantized = uint2(saturate(p * 0.5 + 0.5) * 65535.0 + 0.5);
    return quantized.x | (quantized.y << 16);
}

float3 UnpackPTOctahedralDirection(uint packed)
{
    const float2 p = float2(uint2(packed & 0xffff, packed >> 16)) / 65535.0 * 2.0 - 1.0;
    float3 n = float3(p, 1.0 - abs(p.x) - abs(p.y));
    if(n.z < 0.0)
    {
        n.xy = (1.0 - abs(float2(n.y, n.x))) * select(n.xy >= 0.0, float2(1.0), float2(-1.0));
    }

    return normalize(n);
}

uint PackPTBarycentrics(float2 barycentrics)
{
    // The third barycentric coordinate is implied by 1 - u - v.
    const uint2 quantized = uint2(saturate(barycentrics) * 65535.0 + 0.5);
    return quantized.x | (quantized.y << 16);
}

float2 UnpackPTBarycentrics(uint packed)
{
    return float2(uint2(packed & 0xffff, packed >> 16)) / 65535.0;
}

ReSTIRPTPackedReservoir PackPTReservoir(const ReSTIRPTReservoir reservoir)
{
    ReSTIRPTPackedReservoir data;

    data.ucw        = reservoir.ucw;
    data.integrandR = reservoir.F.x;
    data.integrandG = reservoir.F.y;
    data.integrandB = reservoir.F.z;

    data.initRandomSeed     = reservoir.initRandomSeed;
    data.rcVertexRandomSeed = reservoir.rcVertexRandomSeed;

    // M shares a word with the path flags. Clamping rather than wrapping keeps an
    // over-long history from aliasing into the flag bits above it.
    const uint packedM = min(uint(reservoir.M + 0.5), RESTIR_PT_PATH_FLAGS_M_MASK);
    data.pathFlags = (reservoir.pathFlags & ~RESTIR_PT_PATH_FLAGS_M_MASK)
                   | (packedM << RESTIR_PT_PATH_FLAGS_M_SHIFT);

    data.rcVertexInstanceId     = reservoir.rcVertexInstanceId;
    data.rcVertexPrimitiveIndex = reservoir.rcVertexPrimitiveIndex;
    data.rcVertexBarycentrics   = PackPTBarycentrics(reservoir.rcVertexBarycentrics);
    data.rcVertexWi             = PackPTOctahedralDirection(reservoir.rcVertexWi);

    data.rcVertexRadianceR = reservoir.rcVertexRadiance.x;
    data.rcVertexRadianceG = reservoir.rcVertexRadiance.y;
    data.rcVertexRadianceB = reservoir.rcVertexRadiance.z;

    data.rcVertexJacobianTerms = reservoir.rcVertexJacobianTerms;
    data.rcVertexNeeLightPdf   = reservoir.rcVertexNeeLightPdf;

    return data;
}

ReSTIRPTReservoir UnpackPTReservoir(ReSTIRPTPackedReservoir data)
{
    ReSTIRPTReservoir reservoir;

    reservoir.ucw = data.ucw;
    reservoir.F   = float3(data.integrandR, data.integrandG, data.integrandB);
    // Streaming state is not stored: it has no meaning once a reservoir is
    // finalized, and rebuilding it from the stored values keeps loads consistent.
    reservoir.targetPdf = ReSTIRLuminance(reservoir.F);
    reservoir.weightSum = reservoir.targetPdf * reservoir.ucw;

    reservoir.initRandomSeed     = data.initRandomSeed;
    reservoir.rcVertexRandomSeed = data.rcVertexRandomSeed;
    reservoir.pathFlags          = data.pathFlags;
    reservoir.M = float((data.pathFlags & RESTIR_PT_PATH_FLAGS_M_MASK) >> RESTIR_PT_PATH_FLAGS_M_SHIFT);

    reservoir.rcVertexInstanceId     = data.rcVertexInstanceId;
    reservoir.rcVertexPrimitiveIndex = data.rcVertexPrimitiveIndex;
    reservoir.rcVertexBarycentrics   = UnpackPTBarycentrics(data.rcVertexBarycentrics);
    reservoir.rcVertexWi             = UnpackPTOctahedralDirection(data.rcVertexWi);

    reservoir.rcVertexRadiance      = float3(data.rcVertexRadianceR, data.rcVertexRadianceG, data.rcVertexRadianceB);
    reservoir.rcVertexJacobianTerms = data.rcVertexJacobianTerms;
    reservoir.rcVertexNeeLightPdf   = data.rcVertexNeeLightPdf;

    // Drop non-finite reservoirs before they can poison later reuse. One NaN
    // spread through spatiotemporal resampling contaminates a growing region over
    // subsequent frames, so it is worth rejecting at every load.
    if(any(isnan(reservoir.F)) || any(isinf(reservoir.F)) || isnan(reservoir.ucw) || isinf(reservoir.ucw))
    {
        reservoir = EmptyPTReservoir();
    }

    return reservoir;
}

void StorePTReservoir(
    const ReSTIRPTReservoir reservoir,
    ReSTIRPTReservoirBufferParameters reservoirParams,
    uint2 reservoirPosition,
    uint reservoirArrayIndex)
{
    const uint pointer = PTReservoirPositionToPointer(reservoirParams, reservoirPosition, reservoirArrayIndex);
    RESTIR_PT_RESERVOIR_BUFFER[pointer] = PackPTReservoir(reservoir);
}

ReSTIRPTReservoir LoadPTReservoir(
    ReSTIRPTReservoirBufferParameters reservoirParams,
    uint2 reservoirPosition,
    uint reservoirArrayIndex)
{
    const uint pointer = PTReservoirPositionToPointer(reservoirParams, reservoirPosition, reservoirArrayIndex);
    return UnpackPTReservoir(RESTIR_PT_RESERVOIR_BUFFER[pointer]);
}

#endif // RESTIR_PT_RESERVOIR_STORAGE_HLSLI
