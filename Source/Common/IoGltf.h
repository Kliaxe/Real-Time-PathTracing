
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

// Minimal material model for the rasterizer sample.
enum GltfAlphaMode
{
  eOpaque = 0,
  eMask   = 1,
  eBlend  = 2
};

struct GltfMetallicRoughness
{
  float4 baseColorFactor;                    // Base color factor (RGBA)
  float3 emissionFactor      = float3(0.0f); // Emissive factor
  int    doubleSided         = 0;            // Whether the material is authored as double-sided
  float  metallicFactor;                     // Scalar metallic fallback when no texture is bound
  float  roughnessFactor;                    // Scalar roughness fallback when no texture is bound
  float  specularFactor      = 1.0f;         // Scalar dielectric specular strength
  float  specularTint        = 0.0f;         // Tint for the dielectric specular lobe
  float  subsurfaceFactor    = 0.0f;         // Diffuse-to-subsurface interpolation weight
  float  anisotropy          = 0.0f;         // Anisotropic roughness stretch
  float3 attenuationColor    = float3(1.0f); // Beer-Lambert attenuation color
  float  transmissionFactor = 0.0f;          // 0 = opaque, 1 = fully transmissive dielectric
  float  attenuationDistance = 1.0e30f;      // Distance at which attenuation reaches attenuationColor
  float  volumeThickness     = 0.0f;         // Minimum thickness used for volumetric absorption
  float  refractionIndex    = 1.5f;          // Index of refraction used by the path tracer
  float  clearcoatFactor    = 0.0f;          // Extra dielectric specular layer strength
  float  clearcoatRoughness = 0.0f;          // Roughness of the clearcoat layer
  float  sheenFactor        = 0.0f;          // Strength of the retro-reflective cloth-like lobe
  float  sheenTint          = 0.0f;          // 0 = white sheen, 1 = tint toward base color
  int    baseColorTextureIndex = -1;         // Base color texture index (optional, sampled as sRGB)
  int    metallicRoughnessTextureIndex = -1; // glTF metallic-roughness texture (B = metallic, G = roughness)
  int    emissiveTextureIndex = -1;          // Emissive texture index (optional, sampled as sRGB)
  int    normalTextureIndex = -1;            // Tangent-space normal texture index
  int    specularTextureIndex = -1;          // Scalar dielectric specular texture index
  int    specularColorTextureIndex = -1;     // Specular tint texture index (optional, sampled as sRGB)
  int    transmissionTextureIndex = -1;      // Transmission texture index
  int    thicknessTextureIndex = -1;         // Volume thickness texture index
  int    clearcoatTextureIndex = -1;         // Clearcoat strength texture index
  int    clearcoatRoughnessTextureIndex = -1; // Clearcoat roughness texture index
  int    sheenColorTextureIndex = -1;        // Sheen tint texture index (optional, sampled as sRGB)
  int    sheenRoughnessTextureIndex = -1;    // Sheen roughness/strength texture index
  float  alphaCutoff                 = 0.5f; // Alpha cutoff (used when alphaMode == eMask)
  int    alphaMode                   = GltfAlphaMode::eOpaque;  // Opaque / Mask / Blend
  int    pad0                        = 0;    // Explicit padding to keep the shared layout stable
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

// Emissive triangle used by the path tracer's direct-light sampler.
struct PathTraceEmissiveTriangle
{
  float3 position0;
  float  area;
  float3 position1;
  uint32_t materialIndex;
  float3 position2;
  uint32_t instanceIndex;
  float3 geometricNormal;
  uint32_t primitiveIndex;
  float2 texCoord0;
  float2 texCoord1;
  float2 texCoord2;
  float2 _pad0;
};
CHECK_STRUCT_ALIGNMENT(PathTraceEmissiveTriangle)

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
  PathTraceEmissiveTriangle* emissiveTriangles;   // Emissive triangle light list
  float*                 emissiveTriangleCdf;     // Normalized CDF for emissive triangle sampling
  float*                 environmentCdf;          // Normalized CDF for HDRI texel sampling
  float*                 environmentPdf;          // Normalized discrete HDRI texel probabilities
  uint32_t               emissiveTriangleCount;   // Number of emissive triangles in the light list
  uint32_t               environmentWidth;        // Width of the HDRI importance tables
  uint32_t               environmentHeight;       // Height of the HDRI importance tables
  uint32_t               _pad4;
  GltfPunctual           punctualLights[2];  // Punctual lights
  SkySimpleParameters    skySimpleParam;     // Sky parameters
};
CHECK_STRUCT_ALIGNMENT(GltfSceneInfo)

NAMESPACE_SHADERIO_END()

#endif  // IO_GLTF_H




