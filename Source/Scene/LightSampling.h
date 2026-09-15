#pragma once

#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "Common/IoGltf.h"

namespace rtpt
{

// Emissive light selection weights
// The CPU half of emissive next-event estimation: a weight per emissive triangle and the normalized CDF built from those weights.
// Shaders pick a triangle by searching the CDF and recover its discrete probability from adjacent entries (EvaluateEmissiveTriangleDiscreteProbability in LightDistribution.hlsli), so any weight that is positive wherever a triangle can emit keeps the estimate unbiased. Better weights only lower the variance.
// Nothing here touches Vulkan, so the tables can be checked by CPU tests.

// Rec. 709 luminance of linear RGB, used to weight light sources by perceived brightness.
float ComputeLuminance(const glm::vec3& color);

// Mean Rec. 709 luminance of row-major RGBA8 texels after decoding RGB from sRGB to linear, which is what shaders read through an R8G8B8A8_SRGB image. Alpha is ignored.
// The result is exactly 0 only when every texel's RGB is 0, and it is 0 for an empty image.
float ComputeMeanLinearLuminanceSrgb(std::span<const unsigned char> rgbaPixels);

// Selection weight of each emissive triangle: area times the luminance of its material's emission factor, times the mean linear luminance of the material's emissive texture when it has one.
// Shaders emit the factor multiplied by the texture, so a texture whose mean is 0 emits nothing anywhere and weight 0 is correct for it.
// textureMeanLuminance is indexed by scene texture index; an emissive texture index outside it leaves the weight unscaled. A triangle whose material index is out of range gets weight 0.
std::vector<double> ComputeEmissiveTriangleWeights(std::span<const shaderio::EmissiveTriangleLight> lights, std::span<const shaderio::GltfMetallicRoughness> materials, std::span<const float> textureMeanLuminance);

// Replaces cdf with the normalized prefix sums of ComputeEmissiveTriangleWeights, one entry per light.
// When no weight is positive, lights is emptied and cdf left empty, which leaves the emissive triangle count at 0 and disables emissive light sampling in the shaders.
void BuildEmissiveTriangleCdf(std::vector<shaderio::EmissiveTriangleLight>& lights, std::vector<float>& cdf, std::span<const shaderio::GltfMetallicRoughness> materials, std::span<const float> textureMeanLuminance);

}  // namespace rtpt
