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

#ifndef RESTIR_UNIFORM_SAMPLING_HLSLI
#define RESTIR_UNIFORM_SAMPLING_HLSLI

void SelectLightUniformly(
    float rnd,
    ReSTIRLightBufferRegion region,
    out DILightInfo lightInfo,
    out uint lightIndex,
    out float invSourcePdf)
{
    invSourcePdf = float(region.numLights);
    lightIndex = region.firstLightIndex + min(uint(floor(rnd * region.numLights)), region.numLights - 1);
    lightInfo = DILoadLightInfo(lightIndex, false);
}

#endif // RESTIR_UNIFORM_SAMPLING_HLSLI
