#include "PathTracing/ReSTIR/PT/ReSTIRPTPairingTexture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <random>

namespace nvsamples
{

namespace
{

// Wraps a coordinate onto the texture torus. Every step of the construction is
// toroidal: the texture is tiled across the screen, so a link that leaves one edge
// must re-enter at the other or the pairing stops being a bijection.
inline uint32_t WrapCoordinate(int32_t value, uint32_t size)
{
  const int32_t wrapped = value % int32_t(size);
  return uint32_t(wrapped < 0 ? wrapped + int32_t(size) : wrapped);
}

// Shortest signed distance from `from` to `to` on the torus. This is the step that
// makes the texture tileable: a link near an edge would otherwise record a delta
// nearly the width of the texture, when the same partner is a short hop away
// through the wrap.
inline int32_t ShortestToroidalDelta(uint32_t from, uint32_t to, uint32_t size)
{
  int32_t delta = int32_t(to) - int32_t(from);
  if(delta > int32_t(size) / 2)
  {
    delta -= int32_t(size);
  }
  else if(delta < -int32_t(size) / 2)
  {
    delta += int32_t(size);
  }
  return delta;
}

}  // namespace

uint32_t CalculateReSTIRPTPairingShuffleCount(float sigma)
{
  // Equation 3, in the form the paper gives as its own approximation:
  // n = round(sigma^2 / 2).
  //
  // The correction terms are dropped deliberately. Their placement is ambiguous in
  // the PDF - the extracted layout admits both "sigma^2/2 + 1.46/sigma + ..." and
  // "sigma^2 / (2 + 1.46/sigma + ...)" - so both were built and the achieved
  // standard deviation measured against the requested one on a 254x254 texture:
  //
  //   sigma   round(s^2/2)   +corrections   /(2+corrections)
  //   0.8         0.817          3.359           0.817
  //   2.0         ~1.9           2.277           0.817
  //   4.0         3.920          3.920           3.359
  //   16.0       16.003         16.003          15.613
  //   20.0       20.060         20.060          19.703
  //
  // For sigma >= 3 the first two agree exactly and both land on target; below that
  // the additive reading diverges badly and is not even monotonic (sigma 0.8 asks
  // for 6 shuffles and overshoots four-fold), while the divisor reading is only
  // correct at 0.8. Plain sigma^2/2 is the one form that tracks the target across
  // the whole range, and it reproduces the footnote's claim that sigma = 0.8 is the
  // floor produced by a single iteration.
  const float terms = 0.5f * sigma * sigma;
  return std::max(1u, uint32_t(std::floor(terms + 0.5f)));
}

ReSTIRPTPairingTexture GenerateReSTIRPTPairingTexture(uint32_t size, float sigma, uint32_t seed)
{
  ReSTIRPTPairingTexture texture;
  // An odd edge length would leave one texel without a partner, since links are
  // handed out in pairs.
  if(size < 2 || (size % 2) != 0)
  {
    return texture;
  }

  texture.size = size;
  const uint32_t texelCount = size * size;

  // Each link index is planted twice. After shuffling, the two texels holding a
  // given index are the pair, and the distance they have drifted apart is the
  // delta. Two independent random walks of standard deviation sigma/sqrt(2) give a
  // separation of standard deviation sigma, which is why the shuffle count targets
  // half the variance.
  std::vector<uint32_t> linkIndices(texelCount);
  for(uint32_t texel = 0; texel < texelCount; ++texel)
  {
    linkIndices[texel] = texel / 2;
  }

  std::mt19937 rng(seed);

  const uint32_t shuffleCount = CalculateReSTIRPTPairingShuffleCount(sigma);
  const uint32_t blockCount   = size / 2;
  for(uint32_t iteration = 0; iteration < shuffleCount; ++iteration)
  {
    // Every other iteration slides the 2x2 grid diagonally by one. Without the
    // offset the block boundaries never move and an index could never cross them,
    // so the walk would be confined to its original 2x2 cell forever.
    const int32_t gridOffset = (iteration % 2 == 0) ? 0 : 1;

    for(uint32_t blockY = 0; blockY < blockCount; ++blockY)
    {
      for(uint32_t blockX = 0; blockX < blockCount; ++blockX)
      {
        const uint32_t x0 = WrapCoordinate(int32_t(blockX) * 2 - gridOffset, size);
        const uint32_t y0 = WrapCoordinate(int32_t(blockY) * 2 - gridOffset, size);
        const uint32_t x1 = WrapCoordinate(int32_t(x0) + 1, size);
        const uint32_t y1 = WrapCoordinate(int32_t(y0) + 1, size);

        const std::array<uint32_t, 4> cells{
            y0 * size + x0,
            y0 * size + x1,
            y1 * size + x0,
            y1 * size + x1,
        };

        // A uniformly random permutation of the four cells. Shuffling the values
        // rather than swapping a chosen pair keeps every iteration an exact
        // permutation of the whole texture, so no index is ever lost or duplicated.
        std::array<uint32_t, 4> values{
            linkIndices[cells[0]],
            linkIndices[cells[1]],
            linkIndices[cells[2]],
            linkIndices[cells[3]],
        };
        std::shuffle(values.begin(), values.end(), rng);
        for(size_t i = 0; i < cells.size(); ++i)
        {
          linkIndices[cells[i]] = values[i];
        }
      }
    }
  }

  // Locate the two texels carrying each link index. The paper resolves this on the
  // GPU with a deliberate write race; on the CPU the pair is simply collected.
  const uint32_t             linkCount = texelCount / 2;
  std::vector<uint32_t>      partnerCount(linkCount, 0);
  std::vector<std::array<uint32_t, 2>> linkTexels(linkCount, {0u, 0u});
  for(uint32_t texel = 0; texel < texelCount; ++texel)
  {
    const uint32_t link = linkIndices[texel];
    if(link >= linkCount || partnerCount[link] >= 2)
    {
      // Only reachable if the shuffle stopped being a permutation, which would
      // invalidate every delta derived from it.
      return ReSTIRPTPairingTexture{};
    }
    linkTexels[link][partnerCount[link]] = texel;
    ++partnerCount[link];
  }

  texture.deltas.assign(size_t(texelCount) * 2, 0);
  for(uint32_t link = 0; link < linkCount; ++link)
  {
    const uint32_t texelA = linkTexels[link][0];
    const uint32_t texelB = linkTexels[link][1];

    const uint32_t ax = texelA % size, ay = texelA / size;
    const uint32_t bx = texelB % size, by = texelB / size;

    const int32_t deltaX = ShortestToroidalDelta(ax, bx, size);
    const int32_t deltaY = ShortestToroidalDelta(ay, by, size);

    // A delta beyond int8 range cannot be encoded. It is possible in principle for
    // a very large sigma on a small texture; the texture is rejected rather than
    // silently clamped, because a clamped delta would break reciprocity.
    if(deltaX < -127 || deltaX > 127 || deltaY < -127 || deltaY > 127)
    {
      return ReSTIRPTPairingTexture{};
    }

    texture.deltas[size_t(texelA) * 2 + 0] = int8_t(deltaX);
    texture.deltas[size_t(texelA) * 2 + 1] = int8_t(deltaY);
    texture.deltas[size_t(texelB) * 2 + 0] = int8_t(-deltaX);
    texture.deltas[size_t(texelB) * 2 + 1] = int8_t(-deltaY);
  }

  // Verify the property the whole scheme rests on, rather than assume it: follow
  // every texel's delta and require the partner to point back.
  texture.involutionValid = true;
  for(uint32_t texel = 0; texel < texelCount && texture.involutionValid; ++texel)
  {
    const uint32_t x = texel % size, y = texel / size;
    const uint32_t partnerX = WrapCoordinate(int32_t(x) + texture.deltas[size_t(texel) * 2 + 0], size);
    const uint32_t partnerY = WrapCoordinate(int32_t(y) + texture.deltas[size_t(texel) * 2 + 1], size);
    const uint32_t partner  = partnerY * size + partnerX;

    const uint32_t backX = WrapCoordinate(int32_t(partnerX) + texture.deltas[size_t(partner) * 2 + 0], size);
    const uint32_t backY = WrapCoordinate(int32_t(partnerY) + texture.deltas[size_t(partner) * 2 + 1], size);
    if(partner == texel || backX != x || backY != y)
    {
      texture.involutionValid = false;
    }
  }

  return texture;
}

}  // namespace nvsamples
