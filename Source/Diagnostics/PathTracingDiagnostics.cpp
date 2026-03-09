#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{

struct Vec3
{
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

bool NearlyEqual(float a, float b, float epsilon = 1.0e-5f)
{
  return std::abs(a - b) <= epsilon;
}

float Dot(Vec3 a, Vec3 b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 operator-(Vec3 v)
{
  return Vec3{-v.x, -v.y, -v.z};
}

Vec3 operator+(Vec3 a, Vec3 b)
{
  return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator-(Vec3 a, Vec3 b)
{
  return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator*(Vec3 v, float s)
{
  return Vec3{v.x * s, v.y * s, v.z * s};
}

Vec3 Normalize(Vec3 v)
{
  const float lengthSquared = Dot(v, v);
  if(lengthSquared <= 1.0e-12f)
  {
    return Vec3{};
  }

  const float invLength = 1.0f / std::sqrt(lengthSquared);
  return v * invLength;
}

Vec3 Reflect(Vec3 incident, Vec3 normal)
{
  return incident - normal * (2.0f * Dot(normal, incident));
}

int RunShadingNormalReflectionDiagnostic()
{
  const Vec3 geometricNormal = Normalize(Vec3{0.0f, 0.0f, 1.0f});
  const Vec3 shadingNormal   = Normalize(Vec3{0.8660254f, 0.0f, 0.5f});
  const Vec3 viewDir         = Vec3{0.0f, 0.0f, 1.0f};
  const Vec3 bounceDir       = Normalize(Reflect(-viewDir, shadingNormal));

  const float shadingCos   = Dot(shadingNormal, bounceDir);
  const float geometricCos = Dot(geometricNormal, bounceDir);

  if(shadingCos <= 0.0f)
  {
    std::cerr << "Expected the sampled glossy reflection to remain valid in the shading frame.\n";
    return EXIT_FAILURE;
  }

  if(geometricCos >= 0.0f)
  {
    std::cerr << "Expected the same reflection to fall below the coarse triangle plane.\n";
    return EXIT_FAILURE;
  }

  std::cout << "Shading-normal reflection diagnostic: PASS\n";
  std::cout << "  shading cos   = " << shadingCos << "\n";
  std::cout << "  geometric cos = " << geometricCos << "\n";
  std::cout << "  This is the case that the old geometric-normal rejection discarded.\n";
  return EXIT_SUCCESS;
}

int RunOffsetNormalDiagnostic()
{
  const Vec3 geometricNormal = Vec3{0.0f, 0.0f, 1.0f};
  const Vec3 direction       = Vec3{0.0f, 0.0f, -1.0f};
  const Vec3 offsetNormal    = Dot(geometricNormal, direction) >= 0.0f ? geometricNormal : -geometricNormal;

  if(!NearlyEqual(offsetNormal.z, -1.0f))
  {
    std::cerr << "Expected the ray origin offset to flip to the sampled side of the triangle.\n";
    return EXIT_FAILURE;
  }

  std::cout << "Offset-side diagnostic: PASS\n";
  return EXIT_SUCCESS;
}

}  // namespace

int main()
{
  if(RunShadingNormalReflectionDiagnostic() != EXIT_SUCCESS)
  {
    return EXIT_FAILURE;
  }

  if(RunOffsetNormalDiagnostic() != EXIT_SUCCESS)
  {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
