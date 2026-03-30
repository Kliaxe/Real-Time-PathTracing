/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#ifndef RESTIR_RESERVOIR_ADDRESSING_HLSLI
#define RESTIR_RESERVOIR_ADDRESSING_HLSLI

uint2 PixelPosToReservoirPos(uint2 pixelPosition, uint activeCheckerboardField)
{
    if (activeCheckerboardField == 0)
        return pixelPosition;

    return uint2(pixelPosition.x >> 1, pixelPosition.y);
}

uint2 ReservoirPosToPixelPos(uint2 reservoirIndex, uint activeCheckerboardField)
{
    if (activeCheckerboardField == 0)
        return reservoirIndex;

    uint2 pixelPosition = uint2(reservoirIndex.x << 1, reservoirIndex.y);
    pixelPosition.x += ((pixelPosition.y + activeCheckerboardField) & 1);
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

// Internal SDK function that permutes the pixels sampled from the previous frame.
void ApplyPermutationSampling(inout int2 prevPixelPos, uint uniformRandomNumber)
{
    int2 offset = int2(uniformRandomNumber & 3, (uniformRandomNumber >> 2) & 3);
    prevPixelPos += offset;
 
    prevPixelPos.x ^= 3;
    prevPixelPos.y ^= 3;
    
    prevPixelPos -= offset;
}

#endif // RESTIR_RESERVOIR_ADDRESSING_HLSLI
