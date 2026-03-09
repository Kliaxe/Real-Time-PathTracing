#ifndef PATH_TRACING_COMMON_H
#define PATH_TRACING_COMMON_H

#include "ShaderIncludes/PathTracing/Globals.h.slang"

// -------------------------------------------------------------------------
//    Constants
// -------------------------------------------------------------------------

static const float kRayTMax               = 1.0e32;        // Practical "infinite" ray distance for scene traversal.
static const float kInvPi                 = 0.31830988618; // Shared constant used by cosine-weighted and Lambert-like terms.
static const float kRayOriginFloor        = 1.0 / 32.0;    // Small-position threshold for the floating-point ray offset path.
static const float kRayOriginFloatEps     = 1.0 / 65536.0; // Scalar epsilon used when the integer bit trick is unstable near zero.
static const float kRayOriginIntScale     = 256.0;         // Integer offset scale from the Ray Tracing Gems origin bias recipe.
static const uint  kPathTraceFlagAccumulate = 0x1u;

// -------------------------------------------------------------------------
//    Path tracing payloads
// -------------------------------------------------------------------------

struct PathPayload
{
  float3 radiance;             // Running radiance sum for the whole path.
  float3 throughput;           // Multiplicative weight carried from bounce to bounce.
  float3 mediumAttenuation;    // Current medium extinction coefficient used between this hit and the next.
  float  mediumThickness;      // Minimum segment length for thin-volume absorption when geometry thickness is tiny.
  float3 lastSurfacePosition;  // Previous shading point used when evaluating finite-light MIS on emissive hits.
  float3 lastSurfaceNormal;    // Previous surface normal used when evaluating environment MIS on escape rays.
  float  lastBsdfPdf;          // BSDF PDF of the event that launched the current ray.
  uint   seed;                 // Per-path RNG state advanced in shaders only.
  uint   depth;                // Number of continuation rays already spawned by this path.
  uint   mediumActive;         // Non-zero while the next ray segment travels inside an absorbing dielectric.
  uint   hasLastBsdfSample;    // Non-zero when miss / emissive-hit code should apply MIS against the previous BSDF sample.
};

struct ShadowPayload
{
  uint visible;  // 1 when the shadow ray reaches the miss shader
};

// Resolved shading data at one triangle hit.
struct SurfaceData
{
  float3 worldPosition;       // Hit position in world space.
  float3 shadingNormal;       // Shading-frame normal after normal mapping and face-forwarding.
  float3 geometricNormal;     // Geometric triangle normal used for offsets and hemisphere acceptance.
  float3 tangent;             // World-space tangent for anisotropy and tangent-space normal mapping.
  float3 bitangent;           // World-space bitangent paired with tangent / shadingNormal.
  float2 texCoord;            // Interpolated primary UV set.
  float3 albedo;              // Resolved base color after texture modulation.
  float3 emission;            // Resolved emissive radiance after texture modulation.
  float  metallic;            // Resolved metallic factor in [0, 1].
  float  roughness;           // Resolved perceptual roughness in [0.045, 1].
  float  specular;            // Resolved dielectric specular scale.
  float  specularTint;        // Resolved tint amount for the dielectric lobe.
  float  subsurface;          // Diffuse-to-subsurface interpolation amount.
  float  anisotropy;          // Anisotropy strength used to shape the GGX alpha pair.
  float3 attenuationColor;    // Volume attenuation color used by Beer-Lambert absorption.
  float  transmission;        // Surface transmission strength in [0, 1].
  float  attenuationDistance; // Distance at which attenuationColor is reached.
  float  volumeThickness;     // Authoring thickness used for thin volume absorption.
  float  refractionIndex;     // Index of refraction for transmissive lobes.
  float  clearcoat;           // Secondary dielectric top-coat strength.
  float  clearcoatRoughness;  // Perceptual roughness of the clearcoat lobe.
  float  sheen;               // Sheen lobe strength / roughness-style control.
  float  sheenTint;           // Sheen tint factor from white to base-color tint.
  uint   isFrontFace;  // 1 when the incoming ray hit the front side of the surface
};

bool IsAccumulationEnabled()
{
  // The ray generation shader uses this to decide whether to blend with the history image.
  return (pushConst.flags & kPathTraceFlagAccumulate) != 0;
}

#endif
