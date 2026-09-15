// Presampled light tiles
// ReSTIR PT Enhanced, Section 6.1. NEE with many candidates is bound by memory behaviour, not by arithmetic.
// Each candidate walks the emissive CDF with a binary search and then reads a triangle from anywhere in the scene, so neighbouring threads touch unrelated cache lines and a warp serializes on them.
// Presampling breaks that: this pass draws the lights once into a small tile, and every pixel in an 8x8 screen tile then takes all of its candidates from that one tile, which fits in cache.
// The tiles are redrawn every frame. A stale tile would make the same handful of lights the only ones any pixel can pick, and the resulting correlation looks exactly like the light being in the wrong place rather than like noise.
// Sampling here is the SAME power-weighted CDF draw the single-sample path uses, so a tile entry is an ordinary light sample and the estimator built on it needs no correction for the tiling itself, only for the resampling applied later.

#include <Common/ShaderTypes.h>
#include "ShaderIo.h"
#include "ShaderIncludes/ReSTIR/PTGlobals.hlsli"

#include "ShaderIncludes/PathTracing/Common.hlsli"
#include "ShaderIncludes/PathTracing/Utility.hlsli"
#include "ShaderIncludes/PathTracing/MonteCarlo.hlsli"
#include "ShaderIncludes/PathTracing/LightDistribution.hlsli"

// Dispatched over the tile table rather than the viewport: x is the entry within a tile, y is the tile.
[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 threadID: SV_DispatchThreadID)
{
  const uint sampleIndex = threadID.x;
  const uint tileIndex   = threadID.y;

  // The dispatch rounds up to whole workgroups, so edge threads must drop out.
  if(sampleIndex >= RESTIR_PT_LIGHT_TILE_SIZE || tileIndex >= RESTIR_PT_LIGHT_TILE_COUNT)
  {
    return;
  }

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress.Get();
  const uint          slot      = tileIndex * RESTIR_PT_LIGHT_TILE_SIZE + sampleIndex;

  // An entry with zero invSourcePdf is skipped by the consumer, which is what a scene without emissive triangles gets.
  ReSTIRPTLightTileSample entry;

  entry.lightIndex   = 0u;
  entry.invSourcePdf = 0.0;

  if(sceneInfo.emissiveTriangleCount > 0)
  {
    // Every entry gets its own stream. Advancing one seed along the tile would tie an entry's light to its position in the tile, and pixels that share a tile read the same positions.
    uint       seed  = XxHash32(uint3(sampleIndex, tileIndex, pushConst.rngFrameNumber));
    const uint light = BinarySearchCdf(sceneInfo.emissiveTriangleCdf, sceneInfo.emissiveTriangleCount, NextRandom(seed));

    const float probability = EvaluateEmissiveTriangleDiscreteProbability(sceneInfo, light);

    entry.lightIndex = light;

    // Zero probability means the CDF handed back a light that cannot be chosen, which the consumer must skip rather than divide by.
    entry.invSourcePdf = probability > 0.0 ? (1.0 / probability) : 0.0;
  }

  ptLightTileBuffer[slot] = entry;
}

