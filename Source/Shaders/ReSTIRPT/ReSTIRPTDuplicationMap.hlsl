// Sample duplication map
// ReSTIR PT Enhanced, Section 5. Measures how correlated the frame's reservoirs are, so the next frame's temporal pass can throttle its confidence cap where correlation is building up.
// Why this works at all: after resampling, two pixels hold "the same sample" when their reservoirs descend from one initial candidate. Reservoirs already store initRandomSeed to make random replay possible, and that seed is copied verbatim whenever a sample is passed between pixels, so seed equality IS descent from a common candidate.
// The measure therefore costs no extra state, which is the whole reason the paper picks it over tracking sample provenance explicitly.

// What the score is used for
// A firefly is only a local blemish until reuse spreads it. Temporal confidence controls how strongly a pixel clings to its history, so lowering the cap exactly where duplication is high shortens the lifetime of the blobs and streaks that correlation produces, and leaves uncorrelated regions at full confidence.
// This is deliberately a trade, not a free win: see the temporal pass, where the bias is documented at the point it is actually introduced.

// A known limit of this signal
// Worth reading before trusting it as the damping for correlated reuse. The score counts how many of the 288 neighbours in a 17x17 window carry the SAME initRandomSeed, so it detects one path literally copied into several pixels.
// It does not detect two pixels that hold DIFFERENT paths but whose estimates are correlated because they keep reusing from each other.
// Paired spatial reuse (Section 3) plausibly produces the second kind. Pairing is reciprocal, so within any one frame A reuses from B and B reuses from A from the same shared shift records: the reuse relation is SYMMETRIC, where unpaired reuse draws independent random neighbours and is almost never mutual.
// Note what this is NOT: the pairing is re-randomised every frame (see the conjugation in ReSTIRPTRenderer.cpp), so a pair does not persist and this is not a standing two-cycle in the reuse graph. An earlier version of this comment claimed it was; that was wrong, and the persistent-correlation explanation is ruled out by that re-randomisation.
// Measured consequence: with paired reuse a high-weight reservoir replicates across the image until it saturates the population of pixels that reuse at all, and the image diverges; the unpaired path is stable even at 8 neighbours, because it recomputes both shifts inline and never consumes a shared record. See maxHistoryLength in ReSTIRPTParameterContext.cpp for the numbers.
// That is this map's job, and it is where it falls short. The brake engages as score^gamma, so with a gamma of 0.5 (the value that was tuned when this was measured; the current default is the paper's 0.1) it is still nearly off while the outbreak is small and only bites once duplication is already widespread, by which point the spread has saturated.
// The paper's 0.1 engages on the faintest duplication and does hold the loop flat, which is the whole point of a small exponent, but it caps history so broadly that it costs 5.7% bias on Cornell Box.
// So neither setting is right: 0.5 is accurate and does not contain an outbreak, 0.1 contains one and is not accurate. That gap, a containment mechanism whose only effective setting is unusable, is the open problem, and it is more likely to be in how aggressively the cap is applied than in this score.
// That is why Section 5 damps this instability only when its gamma is small enough to cap history almost everywhere: it is being used as a global brake because its signal cannot see the specific correlation that needs braking.
// Treated as a hypothesis rather than a conclusion: it explains every measurement taken so far, but nothing here has yet measured the score distribution against the actual pairwise correlation, which is what would confirm it.

#include <Common/ShaderTypes.h>
#include "ShaderIo.h"
#include "ShaderIncludes/ReSTIR/PTGlobals.hlsli"
#include "ReSTIR/PTReservoir.hlsli"
#include "ReSTIR/PTReservoirStorage.hlsli"

// 17x17 counting window, so radius 8 around the centre pixel.
static const int kDuplicationRadius = RESTIR_PT_DUPLICATION_NEIGHBORHOOD / 2;

// Shared-memory staging
// One workgroup covers an 8x8 output tile. Every thread reads a 17x17 window, and neighbouring windows overlap almost entirely, so the seeds are staged in shared memory once per tile instead of being re-read 289 times per pixel.
// The staged region is the output tile grown by the radius on all sides: 24 texels across, 576 in total.

static const int kTileSize     = 8;
static const int kSharedExtent = kTileSize + 2 * kDuplicationRadius;
static const int kSharedCount  = kSharedExtent * kSharedExtent;

// Seeds of the staged region.
groupshared uint gSeeds[kSharedCount];
// Whether each staged reservoir actually holds a sample. Tracked separately rather than folded into a sentinel seed: initRandomSeed is an unrestricted 32-bit value, so no reserved value exists that a legitimate seed could not collide with.
groupshared uint gValid[kSharedCount];

[shader("compute")]
[numthreads(kTileSize, kTileSize, 1)]
void main(uint3 threadId: SV_DispatchThreadID, uint3 groupId: SV_GroupID, uint groupIndex: SV_GroupIndex)
{
  const GltfSceneInfo sceneInfo  = pushConst.sceneInfoAddress.Get();
  const uint2         viewport   = (uint2)sceneInfo.viewportSize;
  const int2          tileOrigin = (int2)groupId.xy * kTileSize - kDuplicationRadius;

  // Stage the window
  // kSharedCount (576) exceeds the 64 threads in the group, so each thread walks the staging array with a stride of 64 starting at its own group index, and together the threads fill every entry.
  // Off-screen taps stay invalid, which keeps them out of both the numerator and the implicit denominator: the window is not renormalised at the border. Under-counting at the edge only makes the cap less aggressive there, which is the safe direction: it can leave correlation in, never add bias.

  for(int stagedIndex = int(groupIndex); stagedIndex < kSharedCount; stagedIndex += kTileSize * kTileSize)
  {
    const int2 samplePixel = tileOrigin + int2(stagedIndex % kSharedExtent, stagedIndex / kSharedExtent);

    uint seed  = 0u;
    uint valid = 0u;

    if(all(samplePixel >= int2(0, 0)) && all(samplePixel < (int2)viewport))
    {
      const ReSTIRPTReservoir reservoir = LoadPTReservoir(ptParams.reservoirBufferParams, PTPixelPosToReservoirPos((uint2)samplePixel), ptParams.bufferIndices.shadingInputBufferIndex);

      if(IsValidPTReservoir(reservoir))
      {
        seed  = reservoir.initRandomSeed;
        valid = 1u;
      }
    }

    gSeeds[stagedIndex] = seed;
    gValid[stagedIndex] = valid;
  }

  GroupMemoryBarrierWithGroupSync();

  // Count duplicates
  // Every thread must reach the barrier above, so the out-of-viewport check only happens after it.

  const uint2 pixel = threadId.xy;

  if(pixel.x >= viewport.x || pixel.y >= viewport.y)
  {
    return;
  }

  // Centre of this thread's window inside the staged region.
  const int2 centre      = int2(int(groupIndex) % kTileSize, int(groupIndex) / kTileSize) + kDuplicationRadius;
  const int  centreIndex = centre.y * kSharedExtent + centre.x;

  float score = 0.0;

  // A pixel with no sample has nothing for neighbours to duplicate, and reporting duplication there would throttle a pixel that is not correlated with anything.
  if(gValid[centreIndex] != 0u)
  {
    const uint centreSeed = gSeeds[centreIndex];

    uint duplicates = 0u;

    for(int dy = -kDuplicationRadius; dy <= kDuplicationRadius; ++dy)
    {
      for(int dx = -kDuplicationRadius; dx <= kDuplicationRadius; ++dx)
      {
        const int neighbourIndex = (centre.y + dy) * kSharedExtent + (centre.x + dx);

        // The centre trivially matches itself and is excluded, which is why the paper's divisor is 288 = 17*17 - 1 rather than 289.
        if((dx != 0 || dy != 0) && gValid[neighbourIndex] != 0u && gSeeds[neighbourIndex] == centreSeed)
        {
          ++duplicates;
        }
      }
    }

    score = saturate(float(duplicates) / RESTIR_PT_DUPLICATION_DIVISOR);
  }

  ptDuplicationBuffer[pixel.y * viewport.x + pixel.x] = score;
}

