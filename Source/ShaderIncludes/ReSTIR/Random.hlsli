#ifndef RESTIR_RANDOM_HLSLI
#define RESTIR_RANDOM_HLSLI

static const uint ReSTIRRandomSamplerPrimeConstant = 31;

struct ReSTIRRandomSamplerState
{
  uint seed;
  uint index;
};

// Inserts a 0 between each bit. Takes inputs up to 16 bits wide.
uint IntegerExplode(uint x)
{
  x = (x | (x << 8)) & 0x00FF00FF;
  x = (x | (x << 4)) & 0x0F0F0F0F;
  x = (x | (x << 2)) & 0x33333333;
  x = (x | (x << 1)) & 0x55555555;
  return x;
}

uint ZCurveToLinearIndex(uint2 xy)
{
  return IntegerExplode(xy[0]) | (IntegerExplode(xy[1]) << 1);
}

uint JenkinsHash(uint a)
{
  a = (a + 0x7ed55d16) + (a << 12);
  a = (a ^ 0xc761c23c) ^ (a >> 19);
  a = (a + 0x165667b1) + (a << 5);
  a = (a + 0xd3a2646c) ^ (a << 9);
  a = (a + 0xfd7046c5) + (a << 3);
  a = (a ^ 0xb55a4f09) ^ (a >> 16);
  return a;
}

ReSTIRRandomSamplerState InitRandomSampler(uint2 pixelPos, uint frameIndex, uint pass)
{
  ReSTIRRandomSamplerState state;

  const uint linearPixelIndex = ZCurveToLinearIndex(pixelPos);

  state.index = 1;
  state.seed = JenkinsHash(linearPixelIndex) + frameIndex + pass * ReSTIRRandomSamplerPrimeConstant;

  return state;
}

uint NextMurmurHash(inout ReSTIRRandomSamplerState r)
{
#define ROT32(x, y) ((x << y) | (x >> (32 - y)))

  const uint c1 = 0xcc9e2d51;
  const uint c2 = 0x1b873593;
  const uint r1 = 15;
  const uint r2 = 13;
  const uint m = 5;
  const uint n = 0xe6546b64;

  uint hash = r.seed;
  uint k = r.index++;
  k *= c1;
  k = ROT32(k, r1);
  k *= c2;

  hash ^= k;
  hash = ROT32(hash, r2) * m + n;

  hash ^= 4;
  hash ^= hash >> 16;
  hash *= 0x85ebca6b;
  hash ^= hash >> 13;
  hash *= 0xc2b2ae35;
  hash ^= hash >> 16;

#undef ROT32

  return hash;
}

float GetNextRandom(inout ReSTIRRandomSamplerState rng)
{
  const uint v = NextMurmurHash(rng);
  const uint one = asuint(1.0f);
  const uint mask = (1 << 23) - 1;
  return asfloat((mask & v) | one) - 1.0f;
}

#endif // RESTIR_RANDOM_HLSLI
