#include "Scene/LightSampling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{

// Returns an emissive triangle with the given area and material. Selection weights ignore the geometry, so the vertices stay zero.
shaderio::EmissiveTriangleLight MakeLight(float area, uint32_t materialIndex)
{
  shaderio::EmissiveTriangleLight light {};

  light.area          = area;
  light.materialIndex = materialIndex;

  return light;
}

// Returns a material with the given emission factor and emissive texture index.
shaderio::GltfMetallicRoughness MakeMaterial(const glm::vec3& emission, int emissiveTextureIndex = -1)
{
  shaderio::GltfMetallicRoughness material {};

  material.emissionFactor       = emission;
  material.emissiveTextureIndex = emissiveTextureIndex;

  return material;
}

// Mirrors BinarySearchCdf in Utility.hlsli: the first CDF entry >= value, or the last index for a value past the end.
size_t SearchCdf(const std::vector<float>& cdf, float value)
{
  size_t low  = 0;
  size_t high = cdf.empty() ? 0 : cdf.size() - 1;

  while(low < high)
  {
    const size_t mid = (low + high) >> 1;

    if(value <= cdf[mid])
    {
      high = mid;
    }
    else
    {
      low = mid + 1;
    }
  }

  return low;
}

// Mirrors EvaluateEmissiveTriangleDiscreteProbability in LightDistribution.hlsli.
float DiscreteProbability(const std::vector<float>& cdf, size_t index)
{
  const float previous = index > 0 ? cdf[index - 1] : 0.0f;

  return std::max(cdf[index] - previous, 0.0f);
}

}  // namespace

int main()
{
  // CDF shape
  // With one material, every triangle's probability must be its share of the total area, and the prefix sums must rise monotonically to 1.

  {
    const std::vector<shaderio::GltfMetallicRoughness> materials { MakeMaterial(glm::vec3(1.0f)) };
    const std::array<float, 5>                          areas { 0.5f, 2.0f, 0.25f, 1.25f, 3.0f };

    std::vector<shaderio::EmissiveTriangleLight> lights;

    for(const float area : areas)
    {
      lights.push_back(MakeLight(area, 0));
    }

    std::vector<float> cdf;

    rtpt::BuildEmissiveTriangleCdf(lights, cdf, materials, {});

    if(lights.size() != areas.size() || cdf.size() != areas.size())
    {
      std::cerr << "CDF must hold one entry per light\n";
      return 1;
    }

    for(size_t i = 1; i < cdf.size(); ++i)
    {
      if(cdf[i] < cdf[i - 1])
      {
        std::cerr << "CDF must be monotonically non-decreasing\n";
        return 1;
      }
    }

    if(std::abs(cdf.back() - 1.0f) > 1.0e-6f)
    {
      std::cerr << "CDF must end at 1\n";
      return 1;
    }

    for(size_t i = 0; i < areas.size(); ++i)
    {
      if(std::abs(DiscreteProbability(cdf, i) - areas[i] / 7.0f) > 1.0e-6f)
      {
        std::cerr << "light probability must be proportional to area for equal emission\n";
        return 1;
      }
    }
  }

  // Zero-weight triangles
  // A black emission factor and an emissive texture whose mean is 0 both give weight 0. Such a triangle has zero probability, and no CDF search may land on it.
  // The zero-weight triangles sit between and after positive ones. The two positive triangles have equal weight, so the prefix sums are exact.

  {
    const std::vector<shaderio::GltfMetallicRoughness> materials { MakeMaterial(glm::vec3(4.0f)), MakeMaterial(glm::vec3(0.0f)), MakeMaterial(glm::vec3(4.0f), 0) };
    const std::vector<float>                            textureMeans { 0.0f };

    std::vector<shaderio::EmissiveTriangleLight> lights { MakeLight(1.0f, 0), MakeLight(1.0f, 1), MakeLight(1.0f, 0), MakeLight(1.0f, 2) };

    const std::vector<double> weights = rtpt::ComputeEmissiveTriangleWeights(lights, materials, textureMeans);

    if(weights[1] != 0.0 || weights[3] != 0.0 || weights[0] <= 0.0 || weights[2] <= 0.0)
    {
      std::cerr << "black emission and zero-mean emissive textures must give weight 0, and only those\n";
      return 1;
    }

    std::vector<float> cdf;

    rtpt::BuildEmissiveTriangleCdf(lights, cdf, materials, textureMeans);

    if(lights.size() != 4 || DiscreteProbability(cdf, 1) != 0.0f || DiscreteProbability(cdf, 3) != 0.0f)
    {
      std::cerr << "zero-weight triangles must stay in the list with zero probability\n";
      return 1;
    }

    for(int sample = 0; sample < 1024; ++sample)
    {
      const size_t selected = SearchCdf(cdf, (static_cast<float>(sample) + 0.5f) / 1024.0f);

      if(selected == 1 || selected == 3)
      {
        std::cerr << "a CDF search selected a zero-weight triangle\n";
        return 1;
      }
    }
  }

  // Emissive texture mean
  // The texture multiplies emission everywhere on the triangle, so its mean scales the weight exactly. An index with no recorded mean leaves the weight unscaled.

  {
    const std::vector<shaderio::GltfMetallicRoughness> materials { MakeMaterial(glm::vec3(2.0f)), MakeMaterial(glm::vec3(2.0f), 1), MakeMaterial(glm::vec3(2.0f), 5) };
    const std::vector<float>                            textureMeans { 1.0f, 0.25f };

    std::vector<shaderio::EmissiveTriangleLight> lights { MakeLight(1.0f, 0), MakeLight(1.0f, 1), MakeLight(1.0f, 2) };

    const std::vector<double> weights = rtpt::ComputeEmissiveTriangleWeights(lights, materials, textureMeans);

    if(weights[0] <= 0.0 || weights[1] != weights[0] * 0.25 || weights[2] != weights[0])
    {
      std::cerr << "emissive texture mean must scale the triangle weight\n";
      return 1;
    }

    std::vector<float> cdf;

    rtpt::BuildEmissiveTriangleCdf(lights, cdf, materials, textureMeans);

    if(std::abs(DiscreteProbability(cdf, 1) - 0.25f * DiscreteProbability(cdf, 0)) > 1.0e-6f)
    {
      std::cerr << "emissive texture mean must scale the triangle probability\n";
      return 1;
    }
  }

  // Out-of-range material
  // A triangle whose material index points past the material array cannot be shaded, so it gets weight 0.

  {
    const std::vector<shaderio::GltfMetallicRoughness> materials { MakeMaterial(glm::vec3(1.0f)) };

    std::vector<shaderio::EmissiveTriangleLight> lights { MakeLight(1.0f, 0), MakeLight(1.0f, 7) };

    const std::vector<double> weights = rtpt::ComputeEmissiveTriangleWeights(lights, materials, {});

    if(weights[0] <= 0.0 || weights[1] != 0.0)
    {
      std::cerr << "an out-of-range material index must give weight 0\n";
      return 1;
    }

    std::vector<float> cdf;

    rtpt::BuildEmissiveTriangleCdf(lights, cdf, materials, {});

    if(DiscreteProbability(cdf, 1) != 0.0f)
    {
      std::cerr << "an out-of-range material index must have zero probability\n";
      return 1;
    }
  }

  // All weights zero
  // With nothing to sample, the light list and the CDF must both end up empty, even when the CDF held stale entries.

  {
    const std::vector<shaderio::GltfMetallicRoughness> materials { MakeMaterial(glm::vec3(0.0f)), MakeMaterial(glm::vec3(1.0f), 0) };
    const std::vector<float>                            textureMeans { 0.0f };

    std::vector<shaderio::EmissiveTriangleLight> lights { MakeLight(1.0f, 0), MakeLight(2.0f, 1), MakeLight(3.0f, 9) };
    std::vector<float>                           cdf { 0.5f, 1.0f };

    rtpt::BuildEmissiveTriangleCdf(lights, cdf, materials, textureMeans);

    if(!lights.empty() || !cdf.empty())
    {
      std::cerr << "all-zero weights must empty the light list and the CDF\n";
      return 1;
    }
  }

  // sRGB texture mean
  // A 2x2 texture of white, black, red, and mid-gray. Their linear luminances are 1, 0, 0.2126, and 0.2158605 (sRGB code 128), and alpha differs per texel to show it is ignored.

  {
    constexpr std::array<unsigned char, 16> rgba8 {
      255, 255, 255, 0,
      0, 0, 0, 255,
      255, 0, 0, 255,
      128, 128, 128, 17,
    };

    const double expected = (1.0 + 0.0 + 0.2126 + 0.21586050011389923) / 4.0;
    const float  mean     = rtpt::ComputeMeanLinearLuminanceSrgb(rgba8);

    if(std::abs(static_cast<double>(mean) - expected) > 1.0e-6)
    {
      std::cerr << "sRGB texture mean must be taken over linear luminance\n";
      return 1;
    }

    constexpr std::array<unsigned char, 8> black { 0, 0, 0, 255, 0, 0, 0, 128 };

    if(rtpt::ComputeMeanLinearLuminanceSrgb(black) != 0.0f)
    {
      std::cerr << "a black texture must have a mean of exactly 0\n";
      return 1;
    }
  }

  return 0;
}
