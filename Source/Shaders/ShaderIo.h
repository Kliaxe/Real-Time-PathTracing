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
#include "ReSTIR/Parameters.h"

NAMESPACE_SHADERIO_BEGIN()

// Descriptor binding numbers are part of the CPU/Slang ABI. Keep these values
// synchronized with descriptor-set creation in C++ and resource declarations in
// the corresponding shaders.
enum BindingPoints
{
  eTextures                         = 0,
  eTlas                             = 1,
  eOutputImage                      = 2,
  eAccumulationImage                = 3,
  eMotionVectorsImage               = 4,
  eNormalRoughnessImage             = 5,
  eBaseColorMetalnessImage          = 6,
  eViewZImage                       = 7,
  eDiffuseRadianceHitDistanceImage  = 8,
  eSpecularRadianceHitDistanceImage = 9,
};

// ReSTIR uses a separate descriptor layout because the reuse passes need
// reservoir buffers, current/previous surface buffers, and denoiser signal
// targets in addition to the normal path-tracing resources.
enum ReSTIRDIBindingPoints
{
  eReSTIRDITextures            = 0,
  eReSTIRDITlas                = 1,
  eReSTIRDIOutputImage         = 2,
  eReSTIRDIAccumulationImage   = 3,
  eReSTIRDILightReservoirBuffer = 4,
  eReSTIRDICurrentSurfaceBuffer = 5,
  eReSTIRDIPreviousSurfaceBuffer = 6,
  eReSTIRDINeighborOffsetBuffer = 7,
  eReSTIRDIParamsBuffer        = 8,
  eReSTIRDIDebugBuffer         = 9,
  eReSTIRDIMotionVectorsImage             = 10,
  eReSTIRDINormalRoughnessImage           = 11,
  eReSTIRDIBaseColorMetalnessImage        = 12,
  eReSTIRDIViewZImage                     = 13,
  eReSTIRDIDiffuseRadianceHitDistanceImage = 14,
  eReSTIRDISpecularRadianceHitDistanceImage = 15,
};

struct RasterPushConstant
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
  eReSTIRFlagAccumulate           = 0x1u,
  eReSTIRFlagWriteDenoiserSignals = 0x2u,
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
  eReSTIRDebugViewDepthDisocclusion = 9u,
};

enum ReSTIRShiftStatus
{
  eReSTIRShiftStatusNone = 0u,
  eReSTIRShiftStatusAccepted = 1u,
  eReSTIRShiftStatusRejectedNoHistory = 2u,
  eReSTIRShiftStatusRejectedSurface = 3u,
  eReSTIRShiftStatusRejectedTargetPdf = 4u,
};

struct ReSTIRDIParameters
{
  // Packed uniform block consumed by every ReSTIR pass. The nested structs come
  // from the shared ReSTIR include code, so C++ only fills values and keeps the
  // layout stable.
  ReSTIRRuntimeParameters                runtimeParams;
  ReSTIRReservoirBufferParameters        reservoirBufferParams;
  ReSTIRDIBufferIndices                  bufferIndices;
  ReSTIRDIInitialSamplingParameters      initialSampling;
  ReSTIRDITemporalResamplingParameters   temporalResampling;
  ReSTIRDISpatialResamplingParameters    spatialResampling;
  ReSTIRDIShadingParameters              shading;
};

struct ReSTIRDIPushConstant
{
  GltfSceneInfo* sceneInfoAddress;
  uint           accumulatedFrames;
  uint           flags;
  uint           secondaryPathMaxBounces;
  uint           debugView;
};

struct ReSTIRDISurface
{
  // G-buffer record stored between ReSTIR passes. It intentionally carries the
  // material values needed for direct-light evaluation so later passes do not
  // have to re-run the closest-hit material path.
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
  uint   instanceIndex;
  uint   primitiveIndex;
  uint   isFrontFace;
  uint   valid;
  uint   pad2;
  uint   pad3;
  uint   pad4;
  uint   pad5;
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
  uint  lightKind;
  uint  reservoirAge;
  uint  temporalStatus;
  uint  spatialStatus;
};

CHECK_STRUCT_ALIGNMENT(ReSTIRDISurface)
CHECK_STRUCT_ALIGNMENT(ReSTIRDIParameters)
CHECK_STRUCT_ALIGNMENT(ReSTIRDIPushConstant)
#ifdef __cplusplus
static_assert(sizeof(ReSTIRPackedDIReservoir) == 24, "Packed ReSTIR DI reservoir layout must match the shader storage buffer.");
#endif

NAMESPACE_SHADERIO_END()

#endif  // SHADERIO_H
