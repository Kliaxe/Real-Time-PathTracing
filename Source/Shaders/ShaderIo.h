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

// Binding points shared by raster and ray tracing shaders.
// Raster currently uses only eTextures, while the path tracer also binds the
// TLAS and the storage image it writes into.
enum BindingPoints
{
  eTextures    = 0,  // Combined image sampler array
  eTlas        = 1,  // Top-level acceleration structure
  eOutputImage = 2,  // Storage image for ray tracing output
};

// Push constants used by the raster foundation pass.
struct TutoPushConstant
{
  float3x3       normalMatrix;
  int            instanceIndex;              // Instance index for the current draw call
  GltfSceneInfo* sceneInfoAddress;           // Address of the scene information buffer
  float2         metallicRoughnessOverride;  // Metallic and roughness override values
};

// Push constants used by the path tracing pass.
// This is intentionally small: it just points the shaders at the shared scene
// info buffer and provides a tiny bit of per-frame control data.
struct PathTracePushConstant
{
  GltfSceneInfo* sceneInfoAddress;  // Address of the shared scene information buffer
  uint           frameNumber;       // Frame index used to vary random seeds
  uint           maxBounces;        // Number of bounce continuations after the primary hit
  uint           _pad0;
};

NAMESPACE_SHADERIO_END()

#endif  // SHADERIO_H
