/*
 * Copyright (c) 2019-2026, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2019-2026, NVIDIA CORPORATION.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SHADERIO_H
#define SHADERIO_H

#include "Common/IoGltf.h"

NAMESPACE_SHADERIO_BEGIN()

enum BindingPoints
{
  eTextures          = 0,
  eTlas              = 1,
  eOutputImage       = 2,
  eAccumulationImage = 3,
};

enum ReSTIRBindingPoints
{
  eReSTIRTextures                      = 0,
  eReSTIRTlas                          = 1,
  eReSTIROutputImage                   = 2,
  eReSTIRAccumulationImage             = 3,
  eReSTIRInitialReservoirBuffer        = 4,
  eReSTIRScratchReservoirBuffer        = 5,
  eReSTIRCurrentHistoryReservoirBuffer = 6,
  eReSTIRPreviousHistoryReservoirBuffer = 7,
  eReSTIRCurrentSurfaceBuffer          = 8,
  eReSTIRPreviousSurfaceBuffer         = 9,
  eReSTIRNeighborOffsetBuffer          = 10,
  eReSTIRDebugBuffer                   = 11,
};

struct TutoPushConstant
{
  float3x3       normalMatrix;
  int            instanceIndex;
  GltfSceneInfo* sceneInfoAddress;
  float2         metallicRoughnessOverride;
};

enum PathTraceFlags
{
  ePathTraceFlagAccumulate = 0x1u,
};

struct PathTracePushConstant
{
  GltfSceneInfo* sceneInfoAddress;
  uint           rngFrameNumber;
  uint           accumulatedFrames;
  uint           maxBounces;
  uint           flags;
};

enum ReSTIRFlags
{
  eReSTIRFlagAccumulate            = 0x1u,
  eReSTIRFlagEnableTemporal        = 0x2u,
  eReSTIRFlagEnableSpatial         = 0x4u,
  eReSTIRFlagHasHistory            = 0x8u,
  eReSTIRFlagInitialWriteToHistory = 0x10u,
  eReSTIRFlagTemporalWriteToScratch = 0x20u,
  eReSTIRFlagSpatialReadFromScratch = 0x40u,
};

enum ReSTIRPTCandidateKind
{
  eReSTIRPTCandidateKindBroad = 0u,
  eReSTIRPTCandidateKindReflection = 1u,
  eReSTIRPTCandidateKindGlass = 2u,
  eReSTIRPTCandidateKindClearcoat = 3u,
};

enum ReSTIRPTPathTraceInvocationType
{
  eReSTIRPTPathTraceInvocationTypeNone = 0u,
  eReSTIRPTPathTraceInvocationTypeInitial = 1u,
  eReSTIRPTPathTraceInvocationTypeReplay = 2u,
};

enum ReSTIRDebugView
{
  eReSTIRDebugViewDisabled = 0u,
  eReSTIRDebugViewCandidateKind = 1u,
  eReSTIRDebugViewTargetPdf = 2u,
  eReSTIRDebugViewReservoirWeight = 3u,
  eReSTIRDebugViewReservoirAge = 4u,
  eReSTIRDebugViewTemporalStatus = 5u,
  eReSTIRDebugViewSpatialStatus = 6u,
  eReSTIRDebugViewShiftJacobian = 7u,
  eReSTIRDebugViewReuseCount = 8u,
};

enum ReSTIRShiftStatus
{
  eReSTIRShiftStatusNone = 0u,
  eReSTIRShiftStatusAccepted = 1u,
  eReSTIRShiftStatusRejectedNoHistory = 2u,
  eReSTIRShiftStatusRejectedSurface = 3u,
  eReSTIRShiftStatusRejectedDirection = 4u,
  eReSTIRShiftStatusRejectedVisibility = 5u,
  eReSTIRShiftStatusRejectedTargetPdf = 6u,
  eReSTIRShiftStatusRejectedHistoryAge = 7u,
};

struct ReSTIRInitialSamplingParameters
{
  uint numInitialSamples;
  uint maxBounceDepth;
  uint pad0;
  uint pad1;
};

struct ReSTIRPTReconnectionParameters
{
  float roughnessThreshold;
  float distanceThreshold;
  uint pad0;
  uint pad1;
};

struct ReSTIRTemporalResamplingParameters
{
  float depthThreshold;
  float normalThreshold;
  uint  maxHistoryLength;
  uint  maxReservoirAge;
  uint  pad0;
  uint  pad1;
  uint  pad2;
};

struct ReSTIRSpatialResamplingParameters
{
  uint  numSpatialSamples;
  uint  pad0;
  uint  pad1;
  uint  pad2;
  float samplingRadius;
  float normalThreshold;
  float depthThreshold;
  float pad3;
};

struct ReSTIRPTPushConstant
{
  GltfSceneInfo* sceneInfoAddress;
  uint           rngFrameNumber;
  uint           accumulatedFrames;
  uint           flags;
  ReSTIRInitialSamplingParameters     initialSampling;
  ReSTIRTemporalResamplingParameters  temporalResampling;
  ReSTIRSpatialResamplingParameters   spatialResampling;
  ReSTIRPTReconnectionParameters      reconnection;
  uint           enableVisibilityValidation;
  uint           neighborOffsetCount;
  uint           debugView;
  uint           pathTraceInvocationType;
  uint           pad0;
};

struct ReSTIRPTPrimarySurface
{
  float3 worldPosition;
  float  linearDepth;
  float3 shadingNormal;
  float  roughness;
  float3 geometricNormal;
  float  metallic;
  float3 tangent;
  float  specular;
  float3 bitangent;
  float  specularTint;
  float3 albedo;
  float  transmission;
  float3 emission;
  float  attenuationDistance;
  float3 attenuationColor;
  float  volumeThickness;
  float3 sheenColor;
  float  refractionIndex;
  float  clearcoat;
  float  clearcoatRoughness;
  float  sheenRoughness;
  float  subsurface;
  float  anisotropy;
  uint   materialIndex;
  uint   isFrontFace;
  uint   valid;
  uint   pad0;
  float3 baseRadiance;
  float  pad1;
};

struct ReSTIRPTReservoir
{
  float3 cachedIncidentRadiance;
  float  targetPdf;
  float3 localOutgoingDirection;
  float  weight;
  float3 reconnectionPoint;
  float  M;
  float3 reconnectionNormal;
  uint   age;
  uint   continuationSeed;
  uint   candidateKind;
  uint   valid;
  uint   reconnectionValid;
  uint   pad0;
  uint   pad1;
};

struct ReSTIRNeighborOffset
{
  float2 offset;
  float2 pad;
};

struct ReSTIRDebugPixel
{
  float targetPdf;
  float reservoirWeight;
  float shiftJacobian;
  float reuseCount;
  uint  candidateKind;
  uint  reservoirAge;
  uint  temporalStatus;
  uint  spatialStatus;
};

NAMESPACE_SHADERIO_END()

#endif  // SHADERIO_H








