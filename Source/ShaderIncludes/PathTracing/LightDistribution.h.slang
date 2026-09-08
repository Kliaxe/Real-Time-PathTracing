#ifndef PATH_TRACING_LIGHT_DISTRIBUTION_H
#define PATH_TRACING_LIGHT_DISTRIBUTION_H

// The emissive light distribution on its own, with no ray tracing dependency.
//
// Split out of Lights.h.slang so a compute pass can presample lights: that header
// pulls in the intersection and BSDF code, whose TraceRay calls are not valid in a
// compute stage, and the only thing a presampling pass needs from it is the
// discrete distribution the CDF describes.

#include <nvshaders/slang_types.h>
#include "ShaderIo.h"
#include "ShaderIncludes/PathTracing/Utility.h.slang"

float EvaluateEmissiveTriangleDiscreteProbability(GltfSceneInfo sceneInfo, uint lightIndex)
{
  // Reconstructs one light's discrete probability mass from the normalized prefix CDF built on the CPU.
  if(sceneInfo.emissiveTriangleCount == 0 || lightIndex >= sceneInfo.emissiveTriangleCount)
  {
    return 0.0;
  }

  const float previous = lightIndex > 0 ? sceneInfo.emissiveTriangleCdf[lightIndex - 1] : 0.0;
  return max(sceneInfo.emissiveTriangleCdf[lightIndex] - previous, 0.0);
}

#endif
