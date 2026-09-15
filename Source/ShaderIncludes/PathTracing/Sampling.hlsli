#ifndef RTPT_PATH_SAMPLING_HLSLI
#define RTPT_PATH_SAMPLING_HLSLI

#include "ShaderIncludes/Random.hlsli"

// PathSampleStream
// An addressable stream shared by reference tracing, ReSTIR initial sampling, and replay. The origin stays with the sampled path when a reservoir moves to another pixel or frame.

struct PathSampleStream
{
  // Original pixel packed as two 16-bit coordinates; Vulkan image dimensions fit this representation.
  uint pixel;
  // Original RNG frame, independent of accumulation and reuse history resets.
  uint frame;
  // Hash of the vertex and estimator purpose, so optional NEE work cannot move the BSDF samples.
  uint domain;
  // Next scalar dimension within this estimator's stream.
  uint dimension;
};

static const uint kPTStreamNee      = 0u;
static const uint kPTStreamBsdf     = 1u;
static const uint kPTStreamEmissive = 2u;
static const uint kPTStreamRoulette = 3u;

uint PackPathSamplePixel(uint2 pixel)
{
  return pixel.x | (pixel.y << 16u);
}

PathSampleStream MakePathSampleStream(uint pixel, uint frame, uint vertex, uint purpose)
{
  PathSampleStream stream;

  stream.pixel     = pixel;
  stream.frame     = frame;
  stream.domain    = XxHash32(uint3(vertex, purpose, 0x5354424eu));
  stream.dimension = 0u;

  return stream;
}

// One actual random coordinate, rather than a seed subsequently hashed into white noise.
float SamplePathDimension(Texture2DArray<uint> volume, PathSampleStream stream)
{
  const uint2 pixel      = uint2(stream.pixel & 0xffffu, stream.pixel >> 16u);
  const uint2 tile       = pixel / uint2(uint(BlueNoiseDimensions::eBlueNoiseWidth), uint(BlueNoiseDimensions::eBlueNoiseHeight));
  const uint epoch       = stream.frame / uint(BlueNoiseDimensions::eBlueNoiseLayers);
  const uint dimension   = XxHash32(uint3(stream.domain, stream.dimension, 0x44494d53u));
  const uint tileKey     = XxHash32(uint3(tile, dimension));
  const uint scramble    = XxHash32(uint3(tileKey, epoch, dimension));

  // Fixed within a temporal block
  // Toroidal translations give dimensions and tiles different lookups while preserving each tile's spatial and temporal ordering. The original frame and tile are used during replay, never the receiving pixel's coordinates.

  const uint2 coordinate = (pixel + uint2(scramble, scramble >> 8u)) % uint2(uint(BlueNoiseDimensions::eBlueNoiseWidth), uint(BlueNoiseDimensions::eBlueNoiseHeight));
  const uint layer       = (stream.frame + (scramble >> 16u)) % uint(BlueNoiseDimensions::eBlueNoiseLayers);
  const uint rank        = volume.Load(int4(int2(coordinate), int(layer), 0)).x;

  // Full-domain randomization
  // A dimension-specific Cranley-Patterson rotation changes on each temporal block, so a pixel does not revisit the same 32 high-byte strata forever. Fine jitter avoids quantized directions. Unsigned addition implements the rotation modulo one without a rounded float wrapping to 1.

  const uint rotation = XxHash32(uint3(tileKey, epoch, dimension ^ 0x4350524fu));
  const uint fineBits = XxHash32(uint3(stream.pixel, stream.frame, dimension)) & 0x00ffffffu;
  const uint sample   = ((rank << 24u) | fineBits) + rotation;

  return float(sample >> 8u) * (1.0 / 16777216.0);
}

float NextRandom(inout PathSampleStream stream)
{
  const float sample = SamplePathDimension(blueNoiseTexture, stream);

  stream.dimension += 1u;

  return sample;
}

// Explicit sequencing prevents two inout draws in one expression from aliasing through argument copy-in/copy-out rules.
float2 NextRandom2(inout PathSampleStream stream)
{
  const float x = NextRandom(stream);
  const float y = NextRandom(stream);

  return float2(x, y);
}

#endif
