#include "LightSampling.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace rtpt
{

float ComputeLuminance(const glm::vec3& color)
{
  return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

float ComputeMeanLinearLuminanceSrgb(std::span<const unsigned char> rgbaPixels)
{
  const size_t texelCount = rgbaPixels.size() / 4;

  if(texelCount == 0)
  {
    return 0.0f;
  }

  // sRGB decode table
  // Each 8-bit code is decoded once with the IEC 61966-2-1 transfer function, the same decode an SRGB format applies when shaders sample it. Code 0 maps to exactly 0 and every other code to a positive value.

  std::array<double, 256> linearFromCode {};

  for(size_t code = 0; code < linearFromCode.size(); ++code)
  {
    const double encoded = static_cast<double>(code) / 255.0;

    linearFromCode[code] = encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
  }

  // Mean
  // Accumulated in double precision so a large texture does not lose its dim texels to rounding.

  double luminanceSum = 0.0;

  for(size_t texel = 0; texel < texelCount; ++texel)
  {
    const size_t offset = texel * 4;

    luminanceSum += 0.2126 * linearFromCode[rgbaPixels[offset]] + 0.7152 * linearFromCode[rgbaPixels[offset + 1]] + 0.0722 * linearFromCode[rgbaPixels[offset + 2]];
  }

  return static_cast<float>(luminanceSum / static_cast<double>(texelCount));
}

std::vector<double> ComputeEmissiveTriangleWeights(std::span<const shaderio::EmissiveTriangleLight> lights, std::span<const shaderio::GltfMetallicRoughness> materials, std::span<const float> textureMeanLuminance)
{
  std::vector<double> weights(lights.size(), 0.0);

  for(size_t i = 0; i < lights.size(); ++i)
  {
    const shaderio::EmissiveTriangleLight& light = lights[i];

    // A triangle whose material index is out of range keeps weight 0, so it can never be selected.
    if(light.materialIndex >= materials.size())
    {
      continue;
    }

    const shaderio::GltfMetallicRoughness& material = materials[light.materialIndex];

    // Area times emission-factor luminance is the triangle's power when the material has no emissive texture.
    double weight = static_cast<double>(light.area) * static_cast<double>(std::max(ComputeLuminance(glm::vec3(material.emissionFactor)), 0.0f));

    // The emissive texture multiplies the emission across the whole triangle, so its mean luminance scales the weight.
    if(material.emissiveTextureIndex >= 0 && static_cast<size_t>(material.emissiveTextureIndex) < textureMeanLuminance.size())
    {
      weight *= static_cast<double>(textureMeanLuminance[static_cast<size_t>(material.emissiveTextureIndex)]);
    }

    weights[i] = weight;
  }

  return weights;
}

void BuildEmissiveTriangleCdf(std::vector<shaderio::EmissiveTriangleLight>& lights, std::vector<float>& cdf, std::span<const shaderio::GltfMetallicRoughness> materials, std::span<const float> textureMeanLuminance)
{
  cdf.clear();

  if(lights.empty())
  {
    return;
  }

  const std::vector<double> weights = ComputeEmissiveTriangleWeights(lights, materials, textureMeanLuminance);

  double totalWeight = 0.0;

  for(const double weight : weights)
  {
    totalWeight += weight;
  }

  // With no usable weight there is nothing to sample, so the light list is emptied rather than left behind a CDF that cannot be normalized.
  if(totalWeight <= 0.0)
  {
    lights.clear();
    return;
  }

  // CDF
  // Accumulated in double precision and clamped to 1, so floating-point accumulation cannot push the last entries past the end of the range.

  cdf.resize(lights.size());

  double cumulative = 0.0;

  for(size_t i = 0; i < lights.size(); ++i)
  {
    cumulative += weights[i] / totalWeight;

    cdf[i] = static_cast<float>(std::min(cumulative, 1.0));
  }
}

}  // namespace rtpt
