#ifndef RESTIR_PT_CANDIDATE_H
#define RESTIR_PT_CANDIDATE_H

// ReSTIR PT contract:
// - The primary surface buffer stores the receiver state needed to reevaluate a shifted candidate.
// - Each reservoir stores one replayable first-bounce candidate:
//   the outgoing direction from the receiver, the proposal family that generated it,
//   the cached continuation radiance behind that direction, and an optional first-hit
//   reconnection point that temporal/spatial reuse can shift toward another receiver.

#include "ShaderIncludes/ReSTIR/PT/Common.h.slang"
#ifndef PATH_TRACING_CUSTOM_GLOBALS
#define PATH_TRACING_CUSTOM_GLOBALS
#endif
#include "ShaderIncludes/PathTracing/Utility.h.slang"
#include "ShaderIncludes/PathTracing/Disney.h.slang"

ReSTIRPTPrimarySurface EmptyPrimarySurface()
{
  return (ReSTIRPTPrimarySurface)0;
}

bool IsValidPrimarySurface(ReSTIRPTPrimarySurface surface)
{
  return surface.valid != 0;
}

ReSTIRPTPrimarySurface PackPrimarySurface(SurfaceData surface, uint materialIndex, float linearDepth, float3 baseRadiance)
{
  // This is the receiver record consumed later by temporal/spatial reevaluation
  // and by final replay after a reservoir sample has been selected.
  ReSTIRPTPrimarySurface packed = EmptyPrimarySurface();
  packed.worldPosition          = surface.worldPosition;
  packed.linearDepth            = linearDepth;
  packed.shadingNormal          = surface.shadingNormal;
  packed.roughness              = surface.roughness;
  packed.geometricNormal        = surface.geometricNormal;
  packed.metallic               = surface.metallic;
  packed.tangent                = surface.tangent;
  packed.specular               = surface.specular;
  packed.bitangent              = surface.bitangent;
  packed.specularTint           = surface.specularTint;
  packed.albedo                 = surface.albedo;
  packed.transmission           = surface.transmission;
  packed.emission               = surface.emission;
  packed.attenuationDistance    = surface.attenuationDistance;
  packed.attenuationColor       = surface.attenuationColor;
  packed.volumeThickness        = surface.volumeThickness;
  packed.refractionIndex        = surface.refractionIndex;
  packed.sheenColor             = surface.sheenColor;
  packed.clearcoat              = surface.clearcoat;
  packed.clearcoatRoughness     = surface.clearcoatRoughness;
  packed.sheenRoughness         = surface.sheenRoughness;
  packed.subsurface             = surface.subsurface;
  packed.anisotropy             = surface.anisotropy;
  packed.materialIndex          = materialIndex;
  packed.isFrontFace            = surface.isFrontFace;
  packed.valid                  = 1u;
  packed.baseRadiance           = SanitizeRadiance(baseRadiance);
  return packed;
}

SurfaceData UnpackPrimarySurface(ReSTIRPTPrimarySurface surface)
{
  // Reconstruct the subset of surface state needed after the primary hit.
  // Texture-space details are no longer needed once the receiver is packed.
  SurfaceData unpacked;
  unpacked.worldPosition       = surface.worldPosition;
  unpacked.shadingNormal       = surface.shadingNormal;
  unpacked.geometricNormal     = surface.geometricNormal;
  unpacked.tangent             = surface.tangent;
  unpacked.bitangent           = surface.bitangent;
  unpacked.texCoord            = float2(0.0);
  unpacked.albedo              = surface.albedo;
  unpacked.emission            = surface.emission;
  unpacked.metallic            = surface.metallic;
  unpacked.roughness           = surface.roughness;
  unpacked.specular            = surface.specular;
  unpacked.specularTint        = surface.specularTint;
  unpacked.subsurface          = surface.subsurface;
  unpacked.anisotropy          = surface.anisotropy;
  unpacked.attenuationColor    = surface.attenuationColor;
  unpacked.transmission        = surface.transmission;
  unpacked.attenuationDistance = surface.attenuationDistance;
  unpacked.volumeThickness     = surface.volumeThickness;
  unpacked.refractionIndex     = surface.refractionIndex;
  unpacked.clearcoat           = surface.clearcoat;
  unpacked.clearcoatRoughness  = surface.clearcoatRoughness;
  unpacked.sheenColor          = surface.sheenColor;
  unpacked.sheenRoughness      = surface.sheenRoughness;
  unpacked.isFrontFace         = surface.isFrontFace;
  return unpacked;
}

ReSTIRPTReservoir EmptyReservoir()
{
  return (ReSTIRPTReservoir)0;
}

bool IsValidReservoir(ReSTIRPTReservoir reservoir)
{
  return reservoir.valid != 0 && reservoir.targetPdf > 0.0 && reservoir.M > 0.0;
}

float3 GetReservoirWorldOutgoingDirection(ReSTIRPTPrimarySurface surface, ReSTIRPTReservoir reservoir)
{
  // Reservoir directions are stored in the receiver's local frame so they can
  // be shifted onto another receiver and reevaluated there.
  const float3 localDirection = normalize(reservoir.localOutgoingDirection);
  if(dot(localDirection, localDirection) <= 0.0)
  {
    return float3(0.0);
  }

  return normalize(ToWorld(surface.shadingNormal, surface.tangent, surface.bitangent, localDirection));
}

float3 EvaluateReservoirCandidateBsdfContribution(SurfaceData surface,
                                                  float3 viewDirection,
                                                  float3 worldDirection,
                                                  uint candidateKind,
                                                  out float samplePdf,
                                                  out float directLightPdf,
                                                  out bool isTransmissionEvent)
{
  // ReSTIR PT must reevaluate the candidate under the same proposal family that
  // originally generated it; using the full BSDF mixture here would bias
  // dielectric and glossy cases.
  samplePdf           = 0.0;
  directLightPdf      = 0.0;
  isTransmissionEvent = false;

  const DisneyMaterialData material = GetDisneyMaterialData(surface);
  const DisneyBrdfData     brdf     = PrepareBrdfData(surface, material, viewDirection, worldDirection);
  const DisneyLobeSetup    setup    = GetLobeSetup(material, brdf);

  float conditionalPdf = 0.0;
  float3 contribution  = float3(0.0);

  if(candidateKind == uint(ReSTIRPTCandidateKind::eReSTIRPTCandidateKindBroad))
  {
    contribution   = EvaluateBroadBsdfGroup(material, brdf, setup, conditionalPdf);
    samplePdf      = BroadGroupProbability(setup) * conditionalPdf;
    directLightPdf = samplePdf;
  }
  else if(candidateKind == uint(ReSTIRPTCandidateKind::eReSTIRPTCandidateKindReflection))
  {
    contribution = EvaluateReflectionBsdfGroup(material, brdf, setup, conditionalPdf);
    samplePdf    = ReflectionGroupProbability(setup) * conditionalPdf;
  }
  else if(candidateKind == uint(ReSTIRPTCandidateKind::eReSTIRPTCandidateKindGlass))
  {
    contribution        = EvaluateGlassBsdfGroup(material, brdf, setup, conditionalPdf);
    samplePdf           = setup.glassProbability * conditionalPdf;
    isTransmissionEvent = !brdf.reflection;
  }
  else
  {
    contribution = EvaluateClearcoatBsdfGroup(material, brdf, setup, conditionalPdf);
    samplePdf    = setup.clearcoatProbability * conditionalPdf;
  }

  if(samplePdf <= 0.0 || SafeMax3(contribution) <= 0.0)
  {
    samplePdf           = 0.0;
    directLightPdf      = 0.0;
    isTransmissionEvent = false;
    return float3(0.0);
  }

  return contribution;
}

float3 EvaluateReservoirCandidateContribution(ReSTIRPTPrimarySurface surface,
                                              ReSTIRPTReservoir reservoir,
                                              float3 cameraPosition,
                                              out float samplePdf)
{
  // The stored incident radiance is the suffix behind the sampled first-bounce
  // direction. Re-evaluation reconstructs the full candidate contribution for
  // the current receiver.
  samplePdf = 0.0;
  if(!IsValidPrimarySurface(surface) || reservoir.valid == 0)
  {
    return float3(0.0);
  }

  const float3 worldDirection = GetReservoirWorldOutgoingDirection(surface, reservoir);
  if(dot(worldDirection, worldDirection) <= 0.0)
  {
    return float3(0.0);
  }

  const SurfaceData shadingSurface = UnpackPrimarySurface(surface);
  const float3      viewDirection  = normalize(cameraPosition - shadingSurface.worldPosition);
  float             directLightPdf = 0.0;
  bool              isTransmissionEvent = false;
  const float3      bsdf = EvaluateReservoirCandidateBsdfContribution(shadingSurface, viewDirection, worldDirection,
                                                                      reservoir.candidateKind, samplePdf, directLightPdf,
                                                                      isTransmissionEvent);
  if(samplePdf <= 0.0 || SafeMax3(bsdf) <= 0.0)
  {
    samplePdf = 0.0;
    return float3(0.0);
  }

  return SanitizeRadiance((bsdf / samplePdf) * reservoir.cachedIncidentRadiance);
}

float EvaluateReservoirCandidateTarget(ReSTIRPTPrimarySurface surface, ReSTIRPTReservoir reservoir, float3 cameraPosition)
{
  float samplePdf       = 0.0;
  const float3 radiance = EvaluateReservoirCandidateContribution(surface, reservoir, cameraPosition, samplePdf);
  return samplePdf > 0.0 ? ReSTIRLuminance(radiance) : 0.0;
}

ReSTIRPTReservoir MakeReservoir(float3 localOutgoingDirection,
                                float3 cachedIncidentRadiance,
                                uint continuationSeed,
                                uint candidateKind,
                                float3 reconnectionPoint,
                                float3 reconnectionNormal,
                                bool reconnectionValid,
                                float targetPdf)
{
  // A reservoir stores one replayable first-bounce candidate plus the weights
  // needed by RIS combining.
  ReSTIRPTReservoir reservoir      = EmptyReservoir();
  reservoir.cachedIncidentRadiance = SanitizeRadiance(cachedIncidentRadiance);
  reservoir.localOutgoingDirection = normalize(localOutgoingDirection);
  reservoir.targetPdf              = targetPdf;
  reservoir.weight                 = targetPdf > 0.0 ? 1.0 : 0.0;
  reservoir.reconnectionPoint      = reconnectionPoint;
  reservoir.M                      = targetPdf > 0.0 ? 1.0 : 0.0;
  reservoir.reconnectionNormal     = normalize(reconnectionNormal);
  reservoir.age                    = 0;
  reservoir.continuationSeed       = continuationSeed;
  reservoir.candidateKind          = candidateKind;
  reservoir.valid                  = targetPdf > 0.0 ? 1u : 0u;
  reservoir.reconnectionValid      = reconnectionValid ? 1u : 0u;
  return reservoir;
}

#endif
