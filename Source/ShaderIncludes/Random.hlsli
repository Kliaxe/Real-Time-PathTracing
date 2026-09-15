#ifndef RTPT_SHADER_RANDOM_HLSLI
#define RTPT_SHADER_RANDOM_HLSLI

// 32-bit xxHash of a 3D integer key, used to derive seeds from values like pixel and frame.
// The constants are xxHash32's primes.
uint XxHash32(uint3 value)
{
  const uint4 primes = uint4(2246822519u, 3266489917u, 668265263u, 374761393u);

  uint hash = value.z + primes.w + value.x * primes.y;
  hash      = primes.z * ((hash << 17) | (hash >> 15));
  hash     += value.y * primes.y;
  hash      = primes.z * ((hash << 17) | (hash >> 15));
  hash      = primes.x * (hash ^ (hash >> 15));
  hash      = primes.y * (hash ^ (hash >> 13));

  return hash ^ (hash >> 16);
}

// 32-bit PCG: an LCG state step followed by the RXS-M-XS output permutation.
uint Pcg(inout uint state)
{
  const uint previous = state * 747796405u + 2891336453u;
  const uint word     = ((previous >> ((previous >> 28u) + 4u)) ^ previous) * 277803737u;
  state               = previous;

  return (word >> 22u) ^ word;
}

// Maps the next PCG output to [0, 1).
// The scale rounds to 2^-32 and the top 128 outputs round up to exactly 1.0, where streaming selection's `u * weightSum < weight` rejects even a candidate that holds the entire weight sum.
// Clamping those outputs to the largest float below one, rather than remapping the bits, leaves every other value, and so every other resampling decision, unchanged.
float RandomFloat(inout uint state)
{
  const float oneMinusEpsilon = 0.99999994;

  return min(float(Pcg(state)) * (1.0f / float(0xffffffffu)), oneMinusEpsilon);
}

#endif  // RTPT_SHADER_RANDOM_HLSLI
