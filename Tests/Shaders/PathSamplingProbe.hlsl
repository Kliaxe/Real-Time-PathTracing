#include "Shaders/ShaderIo.h"

[[vk::binding(0)]] Texture2DArray<uint> blueNoiseTexture;
[[vk::binding(1)]] RWStructuredBuffer<uint4> results;
// ProbeParameters
// Separates writing reservoir storage from reading it in a later dispatch.

struct ProbeParameters
{
  // Zero writes, one validates the stored sample origin.
  uint phase;
};

[[vk::push_constant]] ConstantBuffer<ProbeParameters> parameters;

#include "ShaderIncludes/PathTracing/Sampling.hlsli"
#include "ReSTIR/PTReservoir.hlsli"

// Two dispatches exercise the production packed form through real GPU memory.
[[vk::binding(2)]] RWStructuredBuffer<ReSTIRPTPackedReservoir> storedReservoirs;
#define RESTIR_PT_RESERVOIR_BUFFER storedReservoirs
#include "ReSTIR/PTReservoirStorage.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
  const uint pixel = PackPathSamplePixel(id.xy);
  PathSampleStream stream = MakePathSampleStream(pixel, id.z, 0u, kPTStreamBsdf);

  const float x = NextRandom(stream);
  const float2 yz = NextRandom2(stream);

  uint failures = any(float3(x, yz) < 0.0) || any(float3(x, yz) >= 1.0) ? 1u : 0u;

  // Full-width frame identities must survive storage and all endpoint choices.
  ReSTIRPTReservoir reservoir = EmptyPTReservoir();

  reservoir.samplePixel = pixel;
  reservoir.sampleFrame = 0xf1234567u + id.z;
  reservoir.F = (float3)1.0;
  reservoir.ucw = 1.0;
  reservoir.M = 1.0;

  ReSTIRPTReconnectionSearch search = EmptyPTReconnectionSearch();

  ApplyPTReconnectionToReservoir(reservoir, search, 1u);

  search.found = 1u;
  search.length = 0u;
  search.suffixThroughput = (float3)1.0;

  ApplyPTReconnectionToReservoir(reservoir, search, 1u);

  search.pendingEndpointKind = RESTIR_PT_ENDPOINT_KIND_EMISSIVE_NEE;
  search.neeValid = 1u;
  search.neeShadingDepth = 1u;

  ApplyPTReconnectionToReservoir(reservoir, search, 1u);

  const uint storageIndex = (((id.z * 64u + id.y) * 64u + id.x) + 7919u) % (64u * 64u * 64u);

  if(parameters.phase == 0u)
  {
    storedReservoirs[storageIndex] = PackPTReservoir(reservoir);
    return;
  }

  const ReSTIRPTReservoir restored = UnpackPTReservoir(storedReservoirs[storageIndex]);

  failures |= restored.samplePixel != pixel || restored.sampleFrame != 0xf1234567u + id.z ? 2u : 0u;

  // Relocation cannot change the original path's dimensions. Replay starts from
  // the packed origin, including at frame values above float's integer precision.
  PathSampleStream original = MakePathSampleStream(pixel, 0xf1234567u + id.z, id.z % 8u, kPTStreamBsdf);
  PathSampleStream replay = MakePathSampleStream(restored.samplePixel, restored.sampleFrame, id.z % 8u, kPTStreamBsdf);

  for(uint dimension = 0u; dimension < 8u; ++dimension)
  {
    const float initialValue = NextRandom(original);
    const float replayValue = NextRandom(replay);

    failures |= asuint(initialValue) != asuint(replayValue) ? 4u : 0u;
  }

  // Optional NEE draws cannot advance any BSDF dimensions.
  PathSampleStream nee = MakePathSampleStream(pixel, id.z, 0u, kPTStreamNee);

  for(uint candidate = 0u; candidate < id.x; ++candidate)
  {
    NextRandom(nee);
  }

  PathSampleStream independent = MakePathSampleStream(pixel, id.z, 0u, kPTStreamBsdf);
  const float independentX = NextRandom(independent);

  failures |= asuint(independentX) != asuint(x) ? 8u : 0u;
  results[(id.z * 64u + id.y) * 64u + id.x] = uint4(asuint(x), asuint(yz.x), asuint(yz.y), failures);
}
