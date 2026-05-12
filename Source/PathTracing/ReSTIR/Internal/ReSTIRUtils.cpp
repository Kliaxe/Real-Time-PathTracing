/*
 * SPDX-FileCopyrightText: Copyright (c) 2020-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#include "PathTracing/ReSTIR/ReSTIRUtils.h"

namespace restir
{

ReSTIRReservoirBufferParameters CalculateReservoirBufferParameters(uint32_t renderWidth, uint32_t renderHeight)
{
    uint32_t renderWidthBlocks = (renderWidth + RESTIR_RESERVOIR_BLOCK_SIZE - 1) / RESTIR_RESERVOIR_BLOCK_SIZE;
    uint32_t renderHeightBlocks = (renderHeight + RESTIR_RESERVOIR_BLOCK_SIZE - 1) / RESTIR_RESERVOIR_BLOCK_SIZE;
    ReSTIRReservoirBufferParameters params;
    params.reservoirBlockRowPitch = renderWidthBlocks * (RESTIR_RESERVOIR_BLOCK_SIZE * RESTIR_RESERVOIR_BLOCK_SIZE);
    params.reservoirArrayPitch = params.reservoirBlockRowPitch * renderHeightBlocks;
    return params;
}

uint32_t JenkinsHash(uint32_t a)
{
    // http://burtleburtle.net/bob/hash/integer.html
    a = (a + 0x7ed55d16) + (a << 12);
    a = (a ^ 0xc761c23c) ^ (a >> 19);
    a = (a + 0x165667b1) + (a << 5);
    a = (a + 0xd3a2646c) ^ (a << 9);
    a = (a + 0xfd7046c5) + (a << 3);
    a = (a ^ 0xb55a4f09) ^ (a >> 16);
    return a;
}

}
