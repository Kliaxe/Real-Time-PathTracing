#ifndef PATH_TRACING_LIGHT_DISTRIBUTION_H
#define PATH_TRACING_LIGHT_DISTRIBUTION_H

// Emissive light distribution
// The emissive light distribution on its own, with no ray tracing dependency.
// Split out of Lights.hlsli so a compute pass can presample lights. That header pulls in the intersection and BSDF code, whose TraceRay calls are not valid in a compute stage, and all a presampling pass needs from it is the discrete distribution the CDF describes.

#include <Common/ShaderTypes.h>
#include "ShaderIo.h"
#include "ShaderIncludes/PathTracing/Utility.hlsli"

// Reconstructs one light's discrete probability mass from the normalized prefix CDF built on the CPU.
// The CPU (Scene/LightSampling.cpp) weights each triangle by area times emission-factor luminance times its emissive texture's mean luminance. Selection and this probability come from the same CDF, so any weight that is positive wherever a triangle emits keeps the estimate unbiased.
float EvaluateEmissiveTriangleDiscreteProbability(GltfSceneInfo sceneInfo, uint lightIndex)
{
  if(sceneInfo.emissiveTriangleCount == 0 || lightIndex >= sceneInfo.emissiveTriangleCount)
  {
    return 0.0;
  }

  const float current  = vk::RawBufferLoad<float>(sceneInfo.emissiveTriangleCdf + uint64_t(lightIndex) * 4u, 4);
  const float previous = lightIndex > 0 ? vk::RawBufferLoad<float>(sceneInfo.emissiveTriangleCdf + uint64_t(lightIndex - 1u) * 4u, 4) : 0.0;

  // Float rounding in the prefix sums can make adjacent entries decrease slightly, so the difference is clamped.
  return max(current - previous, 0.0);
}

#endif
