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

#ifndef RESTIR_LOCAL_LIGHT_SELECTION_HLSLI
#define RESTIR_LOCAL_LIGHT_SELECTION_HLSLI

#include "ReSTIR/Common/LightSampling/UniformSampling.hlsli"

struct LocalLightSelectionContext
{
    ReSTIRLightBufferRegion lightBufferRegion;
};

LocalLightSelectionContext InitializeLocalLightSelectionContextUniform(ReSTIRLightBufferRegion lightBufferRegion)
{
    LocalLightSelectionContext ctx;
    ctx.lightBufferRegion = lightBufferRegion;
    return ctx;
}

void SelectNextLocalLight(
    LocalLightSelectionContext ctx,
    float rnd,
    out DILightInfo lightInfo,
    out uint lightIndex,
    out float invSourcePdf)
{
    SelectLightUniformly(rnd, ctx.lightBufferRegion, lightInfo, lightIndex, invSourcePdf);
}

#endif // RESTIR_LOCAL_LIGHT_SELECTION_HLSLI
