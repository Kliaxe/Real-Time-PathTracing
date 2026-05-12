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

#pragma once

#include <stdint.h>

#include <ReSTIR/Parameters.h>

namespace restir
{

ReSTIRReservoirBufferParameters CalculateReservoirBufferParameters(uint32_t renderWidth, uint32_t renderHeight);

// 32 bit Jenkins hash
uint32_t JenkinsHash(uint32_t a);

}
