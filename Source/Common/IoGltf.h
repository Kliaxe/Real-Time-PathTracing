#ifndef IO_GLTF_H
#define IO_GLTF_H

#include "Common/ShaderTypes.h"

// Size check
// Shared structs must be a multiple of 8 bytes in C++. LoadDeviceArrayElement reads element i at base + i * sizeof(T) and tells vk::RawBufferLoad the data is 8-byte aligned, so every element must start on an 8-byte boundary.
// Matching strides on both sides come from -fvk-use-scalar-layout, not from this check. HLSL has no static_assert, so the macro expands to nothing there.

#ifdef __cplusplus
#define CHECK_STRUCT_ALIGNMENT(_s) static_assert(sizeof(_s) % 8 == 0);
#else
#define CHECK_STRUCT_ALIGNMENT(_s)
#endif

#include "Common/SkyIo.h"

NAMESPACE_SHADERIO_BEGIN()

// BufferView
// Describes a typed view inside a raw glTF buffer, so shaders can read vertex and index streams directly from the uploaded blob.

struct BufferView
{
  // Offset in bytes from the start of the glTF buffer. 0xFFFFFFFF marks an absent attribute.
  uint32_t offset;

  // Element count.
  uint32_t count;

  // Stride in bytes between elements.
  uint32_t byteStride;
};

// TriangleMesh
// Buffer views for the common vertex and index streams of one triangle primitive.

struct TriangleMesh
{
  // Index stream.
  BufferView indices;

  // Position stream (vec3).
  BufferView positions;

  // Normal stream (vec3).
  BufferView normals;

  // Vertex color stream (vec4, optional).
  BufferView colorVert;

  // Texcoord stream (vec2, optional).
  BufferView texCoords;

  // Tangent stream (vec4, optional).
  BufferView tangents;
};

// glTF alpha modes, stored as ints in GltfMetallicRoughness::alphaMode.
enum GltfAlphaMode
{
  eOpaque = 0,
  eMask   = 1,
  eBlend  = 2
};

// GltfMetallicRoughness
// The material record shared by the rasterizer and the path tracers: glTF metallic-roughness factors plus the KHR material extensions the importer reads.
// Texture indices point into the scene texture descriptor arrays, with -1 meaning no texture.

struct GltfMetallicRoughness
{
  // Base color factor (RGBA).
  float4 baseColorFactor;

  // Emissive factor.
  float3 emissionFactor RTPT_DEFAULT(float3(0.0f, 0.0f, 0.0f));

  // Whether the material is authored as double-sided.
  int doubleSided RTPT_DEFAULT(0);

  // Scalar metallic fallback when no texture is bound.
  float metallicFactor;

  // Scalar roughness fallback when no texture is bound.
  float roughnessFactor;

  // Scalar dielectric specular strength.
  float specularFactor RTPT_DEFAULT(1.0f);

  // Tint for the dielectric specular lobe.
  float specularTint RTPT_DEFAULT(0.0f);

  // Diffuse-to-subsurface interpolation weight.
  float subsurfaceFactor RTPT_DEFAULT(0.0f);

  // Anisotropic roughness stretch.
  float anisotropy RTPT_DEFAULT(0.0f);

  // Beer-Lambert attenuation color.
  float3 attenuationColor RTPT_DEFAULT(float3(1.0f, 1.0f, 1.0f));

  // 0 = opaque, 1 = fully transmissive dielectric.
  float transmissionFactor RTPT_DEFAULT(0.0f);

  // Distance at which attenuation reaches attenuationColor.
  float attenuationDistance RTPT_DEFAULT(1.0e30f);

  // Minimum thickness used for volumetric absorption.
  float volumeThickness RTPT_DEFAULT(0.0f);

  // Index of refraction used by the path tracer.
  float refractionIndex RTPT_DEFAULT(1.5f);

  // Extra dielectric specular layer strength.
  float clearcoatFactor RTPT_DEFAULT(0.0f);

  // Roughness of the clearcoat layer.
  float clearcoatRoughness RTPT_DEFAULT(0.0f);

  // RGB sheen reflectance.
  float3 sheenColorFactor RTPT_DEFAULT(float3(0.0f, 0.0f, 0.0f));

  // Roughness of the sheen lobe.
  float sheenRoughnessFactor RTPT_DEFAULT(0.0f);

  // Base color texture index (sRGB).
  int baseColorTextureIndex RTPT_DEFAULT(-1);

  // glTF metallic-roughness texture index (linear).
  int metallicRoughnessTextureIndex RTPT_DEFAULT(-1);

  // Emissive texture index (sRGB).
  int emissiveTextureIndex RTPT_DEFAULT(-1);

  // Tangent-space normal texture index.
  int normalTextureIndex RTPT_DEFAULT(-1);

  // Scalar dielectric specular texture index.
  int specularTextureIndex RTPT_DEFAULT(-1);

  // Specular tint texture index.
  int specularColorTextureIndex RTPT_DEFAULT(-1);

  // Transmission texture index.
  int transmissionTextureIndex RTPT_DEFAULT(-1);

  // Volume thickness texture index.
  int thicknessTextureIndex RTPT_DEFAULT(-1);

  // Clearcoat strength texture index.
  int clearcoatTextureIndex RTPT_DEFAULT(-1);

  // Clearcoat roughness texture index.
  int clearcoatRoughnessTextureIndex RTPT_DEFAULT(-1);

  // Sheen color texture index.
  int sheenColorTextureIndex RTPT_DEFAULT(-1);

  // Sheen roughness texture index.
  int sheenRoughnessTextureIndex RTPT_DEFAULT(-1);

  // Alpha cutoff, used when alphaMode == eMask.
  float alphaCutoff RTPT_DEFAULT(0.5f);

  // Opaque, Mask, or Blend, as GltfAlphaMode.
  int alphaMode RTPT_DEFAULT(GltfAlphaMode::eOpaque);

  // Explicit padding to keep the shared layout stable.
  int pad0 RTPT_DEFAULT(0);
};
CHECK_STRUCT_ALIGNMENT(GltfMetallicRoughness)

// GltfMesh
// One triangle primitive, pointing into one uploaded glTF binary buffer.

struct GltfMesh
{
  // Device address of the raw glTF data. Typed as a pointer on the CPU but never dereferenced there.
  RTPT_SCENE_ADDRESS(uint8_t) gltfBuffer RTPT_DEFAULT(nullptr);

  // Mesh streams.
  TriangleMesh triMesh;

  // VK_INDEX_TYPE_UINT16 or VK_INDEX_TYPE_UINT32.
  int indexType;

  // Workaround for an issue on a Radeon(TM) RX 7900 XT (see original nvpro sample).
  int padWorkaround;
};

// Punctual light types.
enum GltfLightType
{
  ePoint       = 0,
  eSpot        = 1,
  eDirectional = 2
};

// GltfPunctual
// Punctual light description, stored inline in GltfSceneInfo.

struct GltfPunctual
{
  // World position.
  float3 position;

  // Intensity.
  float intensity;

  // Direction, for spot and directional lights.
  float3 direction;

  // GltfLightType.
  int type;

  // RGB color.
  float3 color;

  // Cone angle in radians, for spot lights.
  float coneAngle;
};

// GltfInstance
// One placed mesh in the scene. Its array position is the index shaders use to look it up.

struct GltfInstance
{
  // Local-to-world transform.
  float4x4 transform;

  // Inverse of the transform's upper 3x3 as the shader reads it, so a normal reaches world space through mul(normalTransform, objectNormal), which applies the inverse transpose.
  // Carried per instance rather than inverted at each hit: every resolved surface needs it, and the passes that resolve one per pixel paid for the inversion on every hit. SetInstanceTransform derives it, so it cannot drift from transform.
  float3x3 normalTransform;

  // Index into the scene material array.
  uint32_t materialIndex;

  // Index into the scene mesh array.
  uint32_t meshIndex;

  // Pads the record to a multiple of 8 bytes; see CHECK_STRUCT_ALIGNMENT above.
  uint32_t _pad0;
};
CHECK_STRUCT_ALIGNMENT(GltfInstance)

// EmissiveTriangleLight
// A world-space emissive triangle shared by the ray-traced renderers for explicit light sampling.
// Each vec3 is paired with a scalar so the struct packs into 16-byte rows.

struct EmissiveTriangleLight
{
  // First vertex, world space.
  float3 position0;

  // Triangle area in world units squared.
  float area;

  // Second vertex, world space.
  float3 position1;

  // Index into the scene material array.
  uint32_t materialIndex;

  // Third vertex, world space.
  float3 position2;

  // Scene instance the triangle belongs to, compared against the hit instance index.
  uint32_t instanceIndex;

  // Unit normal from the triangle winding, world space.
  float3 geometricNormal;

  // Triangle index within its mesh, compared against the hit primitive index.
  uint32_t primitiveIndex;

  // TEXCOORD_0 at the first vertex.
  float2 texCoord0;

  // TEXCOORD_0 at the second vertex.
  float2 texCoord1;

  // TEXCOORD_0 at the third vertex.
  float2 texCoord2;

  // Explicit padding to keep the shared layout stable.
  float2 _pad0;
};
CHECK_STRUCT_ALIGNMENT(EmissiveTriangleLight)

// GltfSceneInfo
// Scene-wide parameters passed to shaders: camera matrices for the current and two previous frames, background and light settings, and device addresses of every scene array.
// The CPU rewrites it every frame; shaders reach it through a buffer pointer in their push constants.

struct GltfSceneInfo
{
  // View-projection matrix.
  float4x4 viewProjMatrix;

  // View matrix used for camera-space guide-buffer reconstruction.
  float4x4 viewMatrix;

  // Inverse view-projection matrix for shared camera-ray reconstruction.
  float4x4 viewProjInvMatrix;

  // Inverse view matrix.
  float4x4 viewInvMatrix;

  // Previous frame view-projection matrix for temporal reprojection.
  float4x4 prevViewProjMatrix;

  // Previous frame view matrix used for screen-space motion reprojection.
  float4x4 prevViewMatrix;

  // Frame N-2 view-projection matrix for history-aware reprojection and debugging.
  float4x4 prevPrevViewProjMatrix;

  // Camera position, world space.
  float3 cameraPosition;

  // Explicit padding to keep the shared layout stable.
  int _padPrevCamera0;

  // Previous frame camera position.
  float3 prevCameraPosition;

  // Explicit padding to keep the shared layout stable.
  int _padPrevCamera1;

  // Camera position from frame N-2.
  float3 prevPrevCameraPosition;

  // Explicit padding to keep the shared layout stable.
  int _padPrevCamera2;

  // Procedural sky toggle.
  int useSky;

  // HDRI environment toggle.
  int useHdrEnv;

  // Texture index of the selected HDRI, or -1 when none is loaded.
  int environmentTextureIndex;

  // Explicit padding to keep the shared layout stable.
  int _pad1;

  // Background color when neither sky nor HDRI is used.
  float3 backgroundColor;

  // Punctual light count (up to 2).
  int numLights;

  // Viewport size in pixels.
  float2 viewportSize;

  // Debug override for metallic (x) and roughness (y), applied to every material as it is resolved. A negative component keeps the material's own value.
  // It lives in the scene uniform rather than a push constant because the rasterizer and both path tracers must show the same material to be comparable.
  float2 metallicRoughnessOverride;

  // GPU address of instances.
  RTPT_SCENE_ADDRESS(GltfInstance) instances;

  // GPU address of meshes.
  RTPT_SCENE_ADDRESS(GltfMesh) meshes;

  // GPU address of materials.
  RTPT_SCENE_ADDRESS(GltfMetallicRoughness) materials;

  // GPU address of the emissive triangle light list.
  RTPT_SCENE_ADDRESS(EmissiveTriangleLight) emissiveTriangles;

  // Normalized CDF for emissive triangle sampling.
  RTPT_SCENE_ADDRESS(float) emissiveTriangleCdf;

  // Normalized CDF for HDRI texel sampling.
  RTPT_SCENE_ADDRESS(float) environmentCdf;

  // Normalized discrete HDRI texel probabilities.
  RTPT_SCENE_ADDRESS(float) environmentPdf;

  // Number of emissive triangles in the light list.
  uint32_t emissiveTriangleCount;

  // Width of the HDRI importance tables.
  uint32_t environmentWidth;

  // Height of the HDRI importance tables.
  uint32_t environmentHeight;

  // Explicit padding to keep the shared layout stable.
  uint32_t _pad4;

  // Punctual lights.
  GltfPunctual punctualLights[2];

  // Procedural sky parameters.
  SkySimpleParameters skySimpleParam;
};
CHECK_STRUCT_ALIGNMENT(GltfSceneInfo)

NAMESPACE_SHADERIO_END()

#endif  // IO_GLTF_H
