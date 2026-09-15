#ifndef RTPT_SHADER_MATH_HLSLI
#define RTPT_SHADER_MATH_HLSLI

static const float RTPT_PI         = 3.14159265358979323846f;
static const float RTPT_INV_PI     = 0.31830988618379067154f;
static const float RTPT_TWO_PI     = 6.28318530717958647692f;

float Square(float value)
{
  return value * value;
}

// Relative luminance of a linear color with Rec. 709 / sRGB primaries.
float Luminance(float3 color)
{
  return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float ClampedDot(float3 left, float3 right)
{
  return clamp(dot(left, right), 0.0f, 1.0f);
}

// Schlick's Fresnel approximation between normal-incidence and grazing reflectance.
float3 SchlickFresnel(float3 reflectance0, float3 reflectance90, float cosineTheta)
{
  const float weight = pow(1.0f - cosineTheta, 5.0f);

  return lerp(reflectance0, reflectance90, weight);
}

// Builds a tangent frame around a unit normal without normalization or branching on the common path.
// This is Frisvad's construction; the -0.99998796 singularity threshold is Max's float-precision refinement of it.
void BuildOrthonormalBasis(float3 normal, out float3 tangent, out float3 bitangent)
{
  // Near -Z the 1 / (1 + z) term blows up, so that pole gets a fixed frame.
  if(normal.z < -0.99998796f)
  {
    tangent   = float3(0.0f, -1.0f, 0.0f);
    bitangent = float3(-1.0f, 0.0f, 0.0f);
    return;
  }

  const float a = 1.0f / (1.0f + normal.z);
  const float b = -normal.x * normal.y * a;

  tangent       = float3(1.0f - normal.x * normal.x * a, b, -normal.x);
  bitangent     = float3(b, 1.0f - normal.y * normal.y * a, -normal.y);
}

// Cosine-weighted direction around local +Z: a uniform disk sample lifted onto the hemisphere.
float3 CosineSampleHemisphere(float random0, float random1)
{
  const float radius = sqrt(random0);
  const float phi    = RTPT_TWO_PI * random1;
  const float x      = radius * cos(phi);
  const float y      = radius * sin(phi);

  return float3(x, y, sqrt(max(0.0f, 1.0f - x * x - y * y)));
}

// Power heuristic (exponent 2) MIS weight for the left strategy against the right one.
float PowerHeuristic(float leftPdf, float rightPdf)
{
  const float leftSquared  = leftPdf * leftPdf;
  const float rightSquared = rightPdf * rightPdf;

  return leftSquared / (leftSquared + rightSquared);
}

#endif  // RTPT_SHADER_MATH_HLSLI
