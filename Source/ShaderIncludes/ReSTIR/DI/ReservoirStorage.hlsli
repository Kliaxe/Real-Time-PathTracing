/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#ifndef RESTIR_DI_RESERVOIR_STORAGE_HLSLI
#define RESTIR_DI_RESERVOIR_STORAGE_HLSLI

#include "ReSTIR/DI/Reservoir.hlsli"
#include "ReSTIR/Common/Utils/ReservoirAddressing.hlsli"

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
    res.canonicalWeight = 0.0f;

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
