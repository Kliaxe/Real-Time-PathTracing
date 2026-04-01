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

#ifndef RESTIR_PARAMETERS_H
#define RESTIR_PARAMETERS_H

#include "ReSTIRTypes.h"

// Set this to 1 to enable debug functionality, such as
//   temporal and spatial path retrace for ReSTIR PT
#define RESTIR_DEBUG 0

// Flag that is used in the RIS buffer to identify that a light is 
// stored in a compact form.
#define RESTIR_LIGHT_COMPACT_BIT 0x80000000u

// Light index mask for the RIS buffer.
#define RESTIR_LIGHT_INDEX_MASK 0x7fffffff

// Reservoirs are stored in a structured buffer in a block-linear layout.
// This constant defines the size of that block, measured in pixels.
#define RESTIR_RESERVOIR_BLOCK_SIZE 16

// Bias correction modes for temporal and spatial resampling:
// Use (1/M) normalization, which is very biased but also very fast.
#define RESTIR_BIAS_CORRECTION_OFF 0
// Use MIS-like normalization but assume that every sample is visible.
#define RESTIR_BIAS_CORRECTION_BASIC 1
// Use pairwise MIS normalization (assuming every sample is visible).  Better perf & specular quality
#define RESTIR_BIAS_CORRECTION_PAIRWISE 2
// Use MIS-like normalization with visibility rays. Unbiased.
#define RESTIR_BIAS_CORRECTION_RAY_TRACED 3

// Uses fixed roughness and distance thresholds to determine reconnectibility
#define RESTIR_PT_RECONNECTION_MODE_FIXED_THRESHOLD 0
// Uses a more dynamic ray + brdf calculation to determine reconnectibility
#define RESTIR_PT_RECONNECTION_MODE_FOOTPRINT 1

// When neighboring samples have less than the naive sampling M threshold, they are ignored during spatial resampling
#define RESTIR_NAIVE_SAMPLING_M_THRESHOLD 2

#define RESTIR_INVALID_LIGHT_INDEX (0xffffffffu)

// FLT_MAX
#define RESTIR_MAX_FLOAT32 3.402823466e+38F 

#ifndef __cplusplus
static const uint ReSTIRInvalidLightIndex = RESTIR_INVALID_LIGHT_INDEX;
#endif

struct ReSTIRLightBufferRegion
{
    uint32_t firstLightIndex;
    uint32_t numLights;
    uint32_t pad1;
    uint32_t pad2;
};

struct ReSTIREnvironmentLightBufferParameters
{
    uint32_t lightPresent;
    uint32_t lightIndex;
    uint32_t pad1;
    uint32_t pad2;
};

struct ReSTIRRuntimeParameters
{
    uint32_t neighborOffsetMask; // Spatial
    uint32_t activeCheckerboardField; // 0 - no checkerboard, 1 - odd pixels, 2 - even pixels
    uint32_t frameIndex;
    uint32_t pad2;
};

struct ReSTIRLightBufferParameters
{
    ReSTIRLightBufferRegion localLightBufferRegion;
    ReSTIRLightBufferRegion infiniteLightBufferRegion;
    ReSTIREnvironmentLightBufferParameters environmentLightParams;
};

struct ReSTIRReservoirBufferParameters
{
    uint32_t reservoirBlockRowPitch;
    uint32_t reservoirArrayPitch;
    uint32_t pad1;
    uint32_t pad2;
};

struct ReSTIRBoilingFilterParameters
{
    uint32_t enableBoilingFilter;
    float boilingFilterStrength;
    uint32_t pad1;
    uint32_t pad2;
};

struct ReSTIRPackedDIReservoir
{
    uint32_t lightData;
    uint32_t uvData;
    uint32_t mVisibility;
    uint32_t distanceAge;
    float targetPdf;
    float weight;
};

#endif // RESTIR_PARAMETERS_H
