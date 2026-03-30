/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#ifndef RESTIR_RANDOM_SAMPLER_STATE_HLSLI
#define RESTIR_RANDOM_SAMPLER_STATE_HLSLI

#include <ReSTIR/Common/Utils/Math.hlsli>
#include <ReSTIR/Common/Utils/RandomSamplerPerPassSeeds.hlsli>

struct ReSTIRRandomSamplerState
{
    uint seed;
    uint index;
};

// Initialized the random sampler for a given pixel or tile index.
// The pass parameter is provided to help generate different RNG sequences
// for different resampling passes, which is important for image quality.
// In general, a high quality RNG is critical to get good results from ReSTIR.
// A table-based blue noise RNG dose not provide enough entropy, for example.
ReSTIRRandomSamplerState InitRandomSampler(uint2 pixelPos, uint frameIndex, uint pass)
{
    ReSTIRRandomSamplerState state;

    uint linearPixelIndex = ZCurveToLinearIndex(pixelPos);

    state.index = 1;
    state.seed = JenkinsHash(linearPixelIndex) + frameIndex + (pass * RESTIR_RANDOM_SAMPLER_PRIME_CONSTANT);

    return state;

}

uint NextMurmurHash(inout ReSTIRRandomSamplerState r)
{
#define ROT32(x, y) ((x << y) | (x >> (32 - y)))

    // https://en.wikipedia.org/wiki/MurmurHash
    uint c1 = 0xcc9e2d51;
    uint c2 = 0x1b873593;
    uint r1 = 15;
    uint r2 = 13;
    uint m = 5;
    uint n = 0xe6546b64;

    uint hash = r.seed;
    uint k = r.index++;
    k *= c1;
    k = ROT32(k, r1);
    k *= c2;

    hash ^= k;
    hash = ROT32(hash, r2) * m + n;

    hash ^= 4;
    hash ^= (hash >> 16);
    hash *= 0x85ebca6b;
    hash ^= (hash >> 13);
    hash *= 0xc2b2ae35;
    hash ^= (hash >> 16);

#undef ROT32

    return hash;
}

// Draws a random number X from the sampler, so that (0 <= X < 1).
float GetNextRandom(inout ReSTIRRandomSamplerState rng)
{
    uint v = NextMurmurHash(rng);
    const uint one = asuint(1.f);
    const uint mask = (1 << 23) - 1;
    return asfloat((mask & v) | one) - 1.f;
}

ReSTIRRandomSamplerState CreateRandomSamplerFromDirectSeed(uint seed, uint index)
{
	ReSTIRRandomSamplerState rng;
	rng.seed = seed;
	rng.index = index;
	return rng;
}

float SampleGaussian(float mean, float stddev, inout ReSTIRRandomSamplerState rng)
{
	// Box-Muller method for sampling from the normal distribution
	// http://en.wikipedia.org/wiki/Normal_distribution#Generating_values_from_normal_distribution
	// This method requires 2 uniform random inputs and produces 2
	// Gaussian random outputs.  We'll take a 3rd random variable and use it to
	// switch between the two outputs.

	// Add in the CPU-supplied random offsets to generate the 3 random values that we'll use.
	const float3 UVR = float3(GetNextRandom(rng), GetNextRandom(rng), GetNextRandom(rng));

	// Switch between the two random outputs.
	float Z = sqrt(-2.0 * log(UVR.x)) * ((UVR.z < 0.5) ? sin(2.0 * RESTIR_PI * UVR.y) : cos(2.0 * RESTIR_PI * UVR.y));

	// Apply the stddev and mean.
	Z = Z * stddev * mean + mean;

	return Z;
}

#endif // RESTIR_RANDOM_SAMPLER_STATE_HLSLI
