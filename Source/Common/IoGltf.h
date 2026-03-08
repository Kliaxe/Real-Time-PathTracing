
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

#ifndef IO_GLTF_H
#define IO_GLTF_H

#ifdef __cplusplus
#define CHECK_STRUCT_ALIGNMENT(_s) static_assert(sizeof(_s) % 8 == 0);
#elif defined(__SLANG__)
#define CHECK_STRUCT_ALIGNMENT(_s)
#else
#define CHECK_STRUCT_ALIGNMENT(_s)

// This is a utility to define a buffer reference in GLSL.
// Usage: declare the buffer reference type with: BUFFER_REF_DECL(type)
// Then use the buffer reference in the shader with: BUFFER_REF(type, address)
#define BUFFER_REF_DECL(_type)                                                                                         \
  layout(buffer_reference, scalar) buffer _type##Buffer                                                                \
  {                                                                                                                    \
    _type o[];                                                                                                         \
  };

#define BUFFER_REF(_type, _addr) _type##Buffer(_addr).o

#endif

#include "nvshaders/sky_io.h.slang"

NAMESPACE_SHADERIO_BEGIN()

// BufferView: describes a typed view inside a raw GLTF buffer.
struct BufferView
{
  uint32_t offset;      // Offset in bytes
  uint32_t count;       // Element count
  uint32_t byteStride;  // Stride in bytes (0 if tightly packed)
};

// TriangleMesh: buffer views for the common vertex/index streams.
struct TriangleMesh
{
  BufferView indices;    // Index stream
  BufferView positions;  // Position stream (vec3)
  BufferView normals;    // Normal stream (vec3)
  BufferView colorVert;  // Vertex color stream (vec4, optional)
  BufferView texCoords;  // Texcoord stream (vec2, optional)
  BufferView tangents;   // Tangent stream (vec4, optional)
};

// Minimal material model for the foundation sample.
enum GltfAlphaMode
{
  eOpaque = 0,
  eMask   = 1,
  eBlend  = 2
};

struct GltfMetallicRoughness
{
  float4 baseColorFactor;             // Base color factor (RGBA)
  float  metallicFactor;              // 0 = dielectric, 1 = metallic
  float  roughnessFactor;             // 0 = smooth, 1 = rough
  int    baseColorTextureIndex = -1;  // Base color texture index (optional)
  float  alphaCutoff           = 0.5f;      // Alpha cutoff (used when alphaMode == eMask)
  int    alphaMode             = GltfAlphaMode::eOpaque;  // Opaque / Mask / Blend
  int    _pad0                 = 0;
};
CHECK_STRUCT_ALIGNMENT(GltfMetallicRoughness)

// GltfMesh: points into one uploaded GLTF binary buffer.
struct GltfMesh
{
  uint8_t*     gltfBuffer = nullptr;  // Pointer to raw data (indices, positions, normals, ...)
  TriangleMesh triMesh;               // Mesh streams
  int          indexType;             // VK_INDEX_TYPE_UINT16 or VK_INDEX_TYPE_UINT32

  // Workaround for an issue on a Radeon(TM) RX 7900 XT (see original nvpro sample).
  int padWorkaround;
};

// Simple light types for the sample.
enum GltfLightType
{
  ePoint       = 0,
  eSpot        = 1,
  eDirectional = 2
};

// Punctual light description.
struct GltfPunctual
{
  float3 position;   // World position
  float  intensity;  // Intensity
  float3 direction;  // Direction (spot/directional)
  int    type;       // GltfLightType
  float3 color;      // RGB color
  float  coneAngle;  // Cone angle (radians) for spot lights
};

// Instance in the scene.
struct GltfInstance
{
  float4x4 transform;      // Local-to-world transform
  uint32_t materialIndex;  // Material index
  uint32_t meshIndex;      // Mesh index
};
CHECK_STRUCT_ALIGNMENT(GltfInstance)

// Scene-wide parameters passed to shaders.
struct GltfSceneInfo
{
  float4x4               viewProjMatrix;     // View-projection matrix
  float4x4               projInvMatrix;      // Historical name: stores the inverse view-projection matrix for shared camera-ray reconstruction
  float4x4               viewInvMatrix;      // Inverse view matrix
  float3                 cameraPosition;     // Camera position
  int                    useSky;             // Sky toggle
  int                    useHdrEnv;          // HDRI background toggle
  int                    environmentTextureIndex;  // Texture index for selected HDRI
  int                    _pad1;
  float3                 backgroundColor;    // Background color if no sky
  int                    numLights;          // Punctual light count (up to 2)
  float2                 viewportSize;       // Viewport size in pixels
  int                    _pad2;
  int                    _pad3;
  GltfInstance*          instances;          // GPU address of instances
  GltfMesh*              meshes;             // GPU address of meshes
  GltfMetallicRoughness* materials;          // GPU address of materials
  GltfPunctual           punctualLights[2];  // Punctual lights
  SkySimpleParameters    skySimpleParam;     // Sky parameters
};
CHECK_STRUCT_ALIGNMENT(GltfSceneInfo)

NAMESPACE_SHADERIO_END()

#endif  // IO_GLTF_H


