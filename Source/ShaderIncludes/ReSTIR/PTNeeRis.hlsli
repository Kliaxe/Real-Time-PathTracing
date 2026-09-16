#ifndef RESTIR_PT_NEE_RIS_H
#define RESTIR_PT_NEE_RIS_H

// RIS-based next event estimation
// ReSTIR PT Enhanced, Section 6.1. Single-sample NEE picks a light from the scene-wide power CDF and lives with it.
// That is fine when one light dominates and poor when many do: the chosen light is often facing away, distant, or outside the BSDF lobe, and the sample is wasted.
// RIS fixes it by drawing several candidates and resampling them against a target that already knows the receiving surface (radiance times BSDF times the MIS weight), so the surviving light is one that can actually light this point.
// Visibility stays out of the target and is traced once, for the winner only. That is what makes the extra candidates affordable: a candidate costs a light read and a BSDF evaluation, not a ray.
// The primary sample space contract (supplemental Section 5) is what keeps the shift mapping unchanged. The stored integrand F is still the SINGLE-sample NEE estimate for the light that won, MIS-weighted and divided by that light's own solid angle density, exactly what the hybrid shift knows how to rebuild at another pixel.
// Everything the resampling contributed is folded into the candidate's unbiased contribution weight instead, where the shift never looks.
// Writing the resampling into F would be the tempting shortcut, and it would break every shift of an NEE endpoint: no other pixel can reproduce the candidate set this pixel happened to draw.

#include "ShaderIncludes/PathTracing/Lights.hlsli"

// Solid angle density of one tile entry, rebuilt from the stored discrete probability rather than from another CDF probe: the probe is what the tile exists to avoid.
float PTLightTileSolidAnglePdf(EmissiveTriangleLight light, float invDiscreteProbability, float lightCos, float distanceSquared)
{
  if(invDiscreteProbability <= 0.0 || light.area <= 1.0e-6 || lightCos <= 0.0 || distanceSquared <= 1.0e-8)
  {
    return 0.0;
  }

  const float areaPdf = 1.0 / (invDiscreteProbability * light.area);

  return areaPdf * distanceSquared / max(lightCos, 1.0e-6);
}

// Candidates for one vertex, on the paper's schedule: the primary hit gets the full count and deeper bounces decay by an inverse square, because their contribution falls off while their divergence cost does not.
uint PTNeeCandidateCount(uint vertexDepth)
{
  const uint scale = (vertexDepth + 1u) * (vertexDepth + 1u);

  return max(1u, ptParams.nee.primaryCandidates / scale);
}

// The light tile this pixel draws from.
// Tied to an 8x8 screen tile so a warp's candidates come from one small region, and re-rolled every frame so a tile's contents never become a fixed subset of the scene's lights.
uint PTSelectLightTile(uint2 launchID)
{
  const uint2 screenTile = launchID / uint(RESTIR_PT_LIGHT_TILE_SCREEN_EXTENT);

  return XxHash32(uint3(screenTile, ptParams.runtimeParams.frameIndex)) % uint(RESTIR_PT_LIGHT_TILE_COUNT);
}

// instanceIndex and primitiveIndex name the receiving vertex's own triangle.
void AccumulatePTEmissiveDirectLightRis(inout PathState path, SurfaceData surface, uint instanceIndex, uint primitiveIndex, GltfSceneInfo sceneInfo, float3 viewDir, uint2 launchID, uint vertexDepth)
{
  // Same gate as the single-sample path, which is also what the next hit's emissive MIS weight checks.
  if(!CanSampleEmissiveDirectLight(surface, sceneInfo))
  {
    return;
  }

  // Candidate window
  // A random entry per pixel, then consecutive entries from there. Starting every pixel at zero would make a whole screen tile resample the same few lights.

  const uint candidateCount = PTNeeCandidateCount(vertexDepth);
  const uint tileBase       = PTSelectLightTile(launchID) * uint(RESTIR_PT_LIGHT_TILE_SIZE);
  const uint tileCursor     = XxHash32(uint3(launchID, ptParams.runtimeParams.frameIndex + vertexDepth * 0x9E3779B9u)) % uint(RESTIR_PT_LIGHT_TILE_SIZE);

  // Resampling state
  // targetSum accumulates the RIS weights w_i = pHat_i / p_i over candidates, with p_i the candidate's solid angle density. The selected* fields hold what the winner needs for its single-sample estimate.

  float  targetSum        = 0.0;
  float  selectedTarget   = 0.0;
  uint   selectedLight    = 0u;
  float3 selectedBary     = (float3)0.0;
  float3 selectedRadiance = (float3)0.0;
  float3 selectedBsdf     = (float3)0.0;
  float  selectedLightPdf = 0.0;
  float  selectedMis      = 0.0;
  float3 selectedPosition = (float3)0.0;
  bool   haveSelection    = false;

  // RIS selection stays independent of the STBN coordinates that position candidates on their lights.
  uint selectionSeed = XxHash32(uint3(path.seed.pixel, path.seed.frame, path.seed.domain ^ 0x524953u));

  // Candidate loop
  // Every candidate that is skipped still counts toward candidateCount, which is the M in the unbiased contribution weight below.

  for(uint candidate = 0; candidate < candidateCount; ++candidate)
  {
    const uint                    slot  = tileBase + ((tileCursor + candidate) % uint(RESTIR_PT_LIGHT_TILE_SIZE));
    const ReSTIRPTLightTileSample entry = ptLightTileBuffer[slot];

    if(entry.invSourcePdf <= 0.0)
    {
      continue;
    }

    const EmissiveTriangleLight light = LoadDeviceArrayElement<EmissiveTriangleLight>(sceneInfo.emissiveTriangles, entry.lightIndex);

    // A surface must not explicitly sample itself. Skipping the candidate rather than the whole estimate is the difference from the single-sample path, which has nothing else to fall back on.
    if(light.instanceIndex == instanceIndex && light.primitiveIndex == primitiveIndex)
    {
      continue;
    }

    // Point on the light
    // Barycentrics come from the path's own stream, not from the tile: the tile fixes WHICH triangle, and every pixel sharing it still needs its own point on that triangle or the samples correlate across the screen tile.

    // Rejected earlier candidates keep their own dimensions, so a branch in one pixel cannot renumber later light-point samples.
    path.seed.dimension = candidate * 2u;

    const float  sqrtXi0 = sqrt(NextRandom(path.seed));
    const float  xi1     = NextRandom(path.seed);
    const float3 bary    = float3(1.0 - sqrtXi0, sqrtXi0 * (1.0 - xi1), sqrtXi0 * xi1);

    const float3 position = light.position0 * bary.x + light.position1 * bary.y + light.position2 * bary.z;
    const float2 texCoord = light.texCoord0 * bary.x + light.texCoord1 * bary.y + light.texCoord2 * bary.z;
    const float3 radiance = EvaluateEmissiveTriangleRadiance(sceneInfo, light, texCoord);

    if(SafeMax3(radiance) <= 0.0)
    {
      continue;
    }

    const float3 unoffsetToLight = position - surface.worldPosition;

    if(dot(unoffsetToLight, unoffsetToLight) <= 1.0e-8)
    {
      continue;
    }

    // Geometry and density
    // The shadow origin must be the one every later evaluation uses, or the density, the geometry and the visibility test disagree with each other.

    const float3 shadowOrigin    = OffsetRay(surface.worldPosition, SelectOffsetNormal(surface.geometricNormal, unoffsetToLight));
    const float3 toLight         = position - shadowOrigin;
    const float  distanceSquared = dot(toLight, toLight);

    if(distanceSquared <= 1.0e-8)
    {
      continue;
    }

    const float  distance = sqrt(distanceSquared);
    const float3 lightDir = toLight / distance;

    const GltfMetallicRoughness material = LoadDeviceArrayElement<GltfMetallicRoughness>(sceneInfo.materials, light.materialIndex);
    const float                 lightCos = EvaluateEmissiveTriangleCosine(material, light.geometricNormal, -lightDir);

    if(lightCos <= 0.0)
    {
      continue;
    }

    const float lightPdf = PTLightTileSolidAnglePdf(light, entry.invSourcePdf, lightCos, distanceSquared);

    if(lightPdf <= 0.0)
    {
      continue;
    }

    float        bsdfPdf = 0.0;
    const float3 bsdf    = EvaluateDirectLightBsdf(surface, viewDir, lightDir, bsdfPdf);

    if(bsdfPdf <= 0.0 || SafeMax3(bsdf) <= 0.0)
    {
      continue;
    }

    // Target function
    // The unshadowed contribution this candidate would make.
    // Visibility is deliberately absent: including it would cost a ray per candidate, which is the whole expense RIS is here to avoid.

    const float  misWeight  = MisMixWeight(lightPdf, bsdfPdf);
    const float3 unshadowed = radiance * bsdf * misWeight;
    const float  target     = ReSTIRLuminance(unshadowed);

    if(!(target > 0.0))
    {
      continue;
    }

    // Streaming selection

    const float weight = target / lightPdf;

    targetSum += weight;

    if(NextRandom(selectionSeed) * targetSum < weight)
    {
      selectedTarget   = target;
      selectedLight    = entry.lightIndex;
      selectedBary     = bary;
      selectedRadiance = radiance;
      selectedBsdf     = bsdf;
      selectedLightPdf = lightPdf;
      selectedMis      = misWeight;
      selectedPosition = position;
      haveSelection    = true;
    }
  }

  if(!haveSelection || selectedTarget <= 0.0 || targetSum <= 0.0)
  {
    return;
  }

  // Winner
  // risUcw = targetSum / (M * pHat(selected)) is the unbiased contribution weight of the resampling, in solid angle measure.
  // The shadow origin is rebuilt exactly as it was for the candidate, so the visibility ray matches the density it was evaluated with.

  const float risUcw = targetSum / (float(candidateCount) * selectedTarget);

  const float3 unoffsetToLight = selectedPosition - surface.worldPosition;
  const float3 shadowOrigin    = OffsetRay(surface.worldPosition, SelectOffsetNormal(surface.geometricNormal, unoffsetToLight));
  const float3 toLight         = selectedPosition - shadowOrigin;
  const float  distance        = length(toLight);

  if(distance <= 1.0e-4)
  {
    return;
  }

  // The one ray. Everything above was arithmetic.
  if(!TraceVisibilityFromOrigin(shadowOrigin, toLight / distance, max(distance - 0.001, 0.001)))
  {
    return;
  }

  // Publish
  // The contribution keeps the single-sample form, so the hybrid shift can rebuild it elsewhere (see the header comment); the resampling lives in the candidate weight.
  // risUcw * lightPdf is the factor between the two parameterizations: the single-sample NEE this integrand pretends to be would have carried a weight of exactly 1, and lightPdf is what converts back to it.

  const float3 contribution = path.throughput * selectedRadiance * selectedBsdf * (selectedMis / selectedLightPdf);

  PATH_TRACING_ON_NEE_EMISSIVE(path, selectedLight, selectedBary, selectedRadiance, selectedLightPdf, distance);

  AccumulatePathContributionWithWeight(path, contribution, risUcw * selectedLightPdf);
  StoreDiffuseDenoiserHitDistanceIfMissing(path, distance);
}

#endif
