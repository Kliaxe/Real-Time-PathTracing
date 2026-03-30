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

#ifndef RESTIR_GI_PARAMETERS_H
#define RESTIR_GI_PARAMETERS_H

#include "ReSTIR/Common/ReSTIRParameters.h"

#ifdef __cplusplus
enum class ReSTIRGIBiasCorrectionMode : uint32_t
{
  Off       = RESTIR_BIAS_CORRECTION_OFF,
  Basic     = RESTIR_BIAS_CORRECTION_BASIC,
  Raytraced = RESTIR_BIAS_CORRECTION_RAY_TRACED
};
#else
#define ReSTIRGIBiasCorrectionMode uint32_t
#endif

struct ReSTIRGIBufferIndices
{
  uint32_t initialSamplingOutputBufferIndex;
  uint32_t temporalResamplingInputBufferIndex;
  uint32_t temporalResamplingOutputBufferIndex;
  uint32_t spatialResamplingInputBufferIndex;

  uint32_t spatialResamplingOutputBufferIndex;
  uint32_t shadingInputBufferIndex;
  uint32_t pad1;
  uint32_t pad2;
};

struct ReSTIRGITemporalResamplingParameters
{
  float    depthThreshold;
  float    normalThreshold;
  uint32_t maxHistoryLength;
  uint32_t enableFallbackSampling;

  ReSTIRGIBiasCorrectionMode biasCorrectionMode;
  uint32_t                   maxReservoirAge;
  uint32_t                   enablePermutationSampling;
  uint32_t                   uniformRandomNumber;
};

struct ReSTIRGISpatialResamplingParameters
{
  float    depthThreshold;
  float    normalThreshold;
  uint32_t numSamples;
  float    samplingRadius;

  ReSTIRGIBiasCorrectionMode biasCorrectionMode;
  uint32_t                   pad1;
  uint32_t                   pad2;
  uint32_t                   pad3;
};

struct ReSTIRGIShadingParameters
{
  uint32_t enableFinalVisibility;
  uint32_t pad1;
  uint32_t pad2;
  uint32_t pad3;
};

#endif
