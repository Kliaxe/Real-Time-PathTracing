#include "Shaders/ShaderIo.h"
#include "ShaderIncludes/Camera.hlsli"
#include "ShaderIncludes/Math.hlsli"
#include "ShaderIncludes/SceneAccess.hlsli"
#include "ShaderIncludes/Sky.hlsli"

// Raster preview
// Vertex and fragment entry points for the rasterized scene view drawn by RasterRenderer.
// Shading is a cheap environment-lit approximation, not the path tracer's BSDF.

[[vk::push_constant]] ConstantBuffer<RasterPushConstant> pushConstants;
[[vk::binding(BindingPoints::eHlslTextures)]] Texture2D<float4> textures[];
[[vk::binding(BindingPoints::eHlslTextureSamplers)]] SamplerState textureSamplers[];

// VertexOutput
// Interpolants passed from vertexMain to fragmentMain.

struct VertexOutput
{
  // Clip-space position.
  float4 position : SV_Position;

  // World-space position, used to build the view direction.
  float3 worldPosition : POSITION;

  // World-space normal, renormalized per fragment.
  float3 worldNormal : NORMAL;

  // Material texture coordinate.
  float2 textureCoordinate : TEXCOORD0;
};

float4 SampleSceneTexture(int textureIndex, float2 textureCoordinate)
{
  // Texture and sampler arrays share one index, and the index varies per draw, so it must be marked non-uniform.
  const uint index = NonUniformResourceIndex(uint(textureIndex));

  return textures[index].Sample(textureSamplers[index], textureCoordinate);
}

float2 DirectionToLatLongUv(float3 direction)
{
  const float3 normalizedDirection = normalize(direction);

  // Equirectangular mapping: the constants are 1 / (2 pi) for longitude and 1 / pi for latitude.
  return float2(0.5f - atan2(normalizedDirection.z, normalizedDirection.x) * 0.15915494309f, 0.5f - asin(clamp(normalizedDirection.y, -1.0f, 1.0f)) * 0.31830988618f);
}

float3 SampleEnvironment(GltfSceneInfo sceneInfo, float3 direction)
{
  // The HDR environment takes priority, then the procedural sky, then the flat background colour: the order the path tracers' SampleEnvironment uses and RasterRenderer's background choice follows.
  if(sceneInfo.useHdrEnv != 0 && sceneInfo.environmentTextureIndex >= 0)
  {
    return SampleSceneTexture(sceneInfo.environmentTextureIndex, DirectionToLatLongUv(direction)).xyz;
  }

  if(sceneInfo.useSky != 0)
  {
    return EvaluateSimpleSky(sceneInfo.skySimpleParam, direction);
  }

  return sceneInfo.backgroundColor;
}

float3 EvaluateEnvironmentMaterial(GltfSceneInfo sceneInfo, float3 albedo, float metallic, float roughness, float3 normal, float3 viewDirection)
{
  // Diffuse
  // A constant ambient term tinted toward the background colour stands in for irradiance; metals get no diffuse.

  const float3 ambientColor = lerp(float3(0.08f, 0.08f, 0.08f), sceneInfo.backgroundColor, 0.35f);
  const float3 diffuse      = albedo * ambientColor * (1.0f - metallic);

  // Glossy reflection
  // One environment lookup along the mirror direction, bent toward the normal as roughness grows, weighted by Schlick Fresnel and faded for rough surfaces.

  const float clampedRoughness = clamp(roughness, 0.0f, 1.0f);
  const float3 reflectionDirection = reflect(-viewDirection, normal);
  const float3 glossyDirection = normalize(lerp(reflectionDirection, normal, clampedRoughness * clampedRoughness));
  const float3 environmentReflection = SampleEnvironment(sceneInfo, glossyDirection);
  const float3 reflectance0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
  const float3 fresnel = SchlickFresnel(reflectance0, float3(1.0f, 1.0f, 1.0f), ClampedDot(normal, viewDirection));
  const float reflectionStrength = lerp(1.0f, 0.08f, clampedRoughness * clampedRoughness);

  return diffuse + environmentReflection * fresnel * reflectionStrength;
}

float2 ResolveMetallicRoughness(GltfMetallicRoughness material, float2 textureCoordinate)
{
  float metallic  = material.metallicFactor;
  float roughness = material.roughnessFactor;

  // glTF packs roughness in green and metalness in blue.
  if(material.metallicRoughnessTextureIndex >= 0)
  {
    const float4 sample = SampleSceneTexture(material.metallicRoughnessTextureIndex, textureCoordinate);
    roughness *= sample.g;
    metallic *= sample.b;
  }

  return clamp(float2(metallic, roughness), 0.0f, 1.0f);
}

VertexOutput vertexMain(uint vertexIndex : SV_VertexID)
{
  const GltfSceneInfo sceneInfo = pushConstants.sceneInfoAddress.Get();

  VertexOutput output;

  // Background triangle
  // A negative instance index marks RasterRenderer's HDRI background draw: three vertices forming one triangle that covers the screen, with no mesh data.

  if(pushConstants.instanceIndex < 0)
  {
    const float2 fullscreenPositions[3] = { float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f) };
    output.position          = float4(fullscreenPositions[vertexIndex], 0.0f, 1.0f);
    output.worldPosition     = float3(0.0f, 0.0f, 0.0f);
    output.worldNormal       = float3(0.0f, 1.0f, 0.0f);
    output.textureCoordinate = float2(0.0f, 0.0f);
    return output;
  }

  // Mesh vertex
  // Attributes are read from the scene's device buffers by vertex index; RasterRenderer binds only an index buffer for mesh draws.

  const GltfInstance instance = LoadDeviceArrayElement<GltfInstance>(sceneInfo.instances, uint(pushConstants.instanceIndex));
  const GltfMesh mesh = LoadDeviceArrayElement<GltfMesh>(sceneInfo.meshes, instance.meshIndex);
  const float3 localPosition = LoadAttribute<float3>(mesh.gltfBuffer, mesh.triMesh.positions, vertexIndex);
  const float3 localNormal   = LoadAttribute<float3>(mesh.gltfBuffer, mesh.triMesh.normals, vertexIndex);
  const float2 textureCoordinate = LoadAttribute<float2>(mesh.gltfBuffer, mesh.triMesh.texCoords, vertexIndex);
  const float4 worldPosition = mul(float4(localPosition, 1.0f), instance.transform);

  // Normals use the inverse-transpose matrix from the CPU so non-uniform scale does not skew them.
  output.position          = mul(worldPosition, sceneInfo.viewProjMatrix);
  output.worldPosition     = worldPosition.xyz;
  output.worldNormal       = normalize(mul(localNormal, pushConstants.normalMatrix));
  output.textureCoordinate = textureCoordinate;

  return output;
}

float4 fragmentMain(VertexOutput input) : SV_Target
{
  const GltfSceneInfo sceneInfo = pushConstants.sceneInfoAddress.Get();

  // The background triangle has no surface; it shows the environment along each pixel's view ray.
  if(pushConstants.instanceIndex < 0)
  {
    const float3 direction = ReconstructWorldDirectionFromPixel(input.position.xy, sceneInfo);
    return float4(SampleEnvironment(sceneInfo, direction), 1.0f);
  }

  // Material

  const GltfInstance instance = LoadDeviceArrayElement<GltfInstance>(sceneInfo.instances, uint(pushConstants.instanceIndex));
  const GltfMetallicRoughness material = LoadDeviceArrayElement<GltfMetallicRoughness>(sceneInfo.materials, instance.materialIndex);

  float4 baseColor = material.baseColorFactor;

  if(material.baseColorTextureIndex >= 0)
  {
    baseColor *= SampleSceneTexture(material.baseColorTextureIndex, input.textureCoordinate);
  }

  // Alpha-masked materials cut out below their cutoff; blend mode is not handled here.
  if(material.alphaMode == GltfAlphaMode::eMask && baseColor.w < material.alphaCutoff)
  {
    discard;
  }

  // Debug override
  // A component of metallicRoughnessOverride at or above zero replaces the material value; the UI default of -0.01 leaves the material alone.

  float2 metallicRoughness = ResolveMetallicRoughness(material, input.textureCoordinate);

  if(pushConstants.metallicRoughnessOverride.x >= 0.0f)
    metallicRoughness.x = pushConstants.metallicRoughnessOverride.x;
  if(pushConstants.metallicRoughnessOverride.y >= 0.0f)
    metallicRoughness.y = pushConstants.metallicRoughnessOverride.y;

  // Shade

  const float3 normal = normalize(input.worldNormal);
  const float3 viewDirection = normalize(sceneInfo.cameraPosition - input.worldPosition);
  const float3 color = EvaluateEnvironmentMaterial(sceneInfo, baseColor.xyz, metallicRoughness.x, metallicRoughness.y, normal, viewDirection);

  return float4(max(color, float3(0.0f, 0.0f, 0.0f)), 1.0f);
}
