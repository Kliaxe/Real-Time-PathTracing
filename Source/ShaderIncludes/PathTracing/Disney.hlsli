#ifndef PATH_TRACING_DISNEY_H
#define PATH_TRACING_DISNEY_H

#include "ShaderIncludes/PathTracing/Common.hlsli"
#include "ShaderIncludes/PathTracing/Utility.hlsli"
#include "ShaderIncludes/PathTracing/MonteCarlo.hlsli"

// BRDF data
// Disney / principled BSDF built from lobes: Burley diffuse with a Hanrahan-Krueger subsurface blend, anisotropic GGX dielectric and metallic reflection, rough dielectric transmission, Charlie sheen, and an isotropic GGX clearcoat.
// Lobes are grouped by the proposal that samples them (broad cosine, glossy VNDF, glass, clearcoat), and a group index doubles as the lobe kind stored with a sampled path.

// DisneyMaterialData
// The surface's material parameters clamped and remapped into what the lobe formulas consume.
// Built once per evaluation so every lobe reads the same derived values, such as the GGX alphas and the tinted F0.

struct DisneyMaterialData
{
  // Base diffuse / metal color after texturing.
  float3 albedo;
  // Dielectric F0 color after specular + tint remapping.
  float3 specularColor;
  // RGB sheen reflectance resolved from the material.
  float3 sheenColor;
  // Metalness selector for base lobe partitioning.
  float  metallic;
  // Perceptual roughness kept for diffuse terms.
  float  roughness;
  // Squared roughness used by GGX.
  float  alpha;
  // Tangent-axis GGX alpha.
  float  alphaAnisotropicX;
  // Bitangent-axis GGX alpha.
  float  alphaAnisotropicY;
  // Transmission strength for glass behavior.
  float  transmission;
  // IOR shared by reflection / refraction Fresnel.
  float  refractionIndex;
  // Scalar dielectric specular scale.
  float  specular;
  // Amount of albedo tint applied to dielectric F0.
  float  specularTint;
  // Blend factor between Burley diffuse and HK-style subsurface diffuse.
  float  subsurface;
  // Authoring anisotropy input used to derive alphaX / alphaY.
  float  anisotropy;
  // Extra top-layer lobe strength.
  float  clearcoat;
  // Perceptual clearcoat roughness.
  float  clearcoatRoughness;
  // Squared clearcoat roughness.
  float  clearcoatAlpha;
  // Charlie sheen roughness parameter.
  float  sheenRoughness;
  // Untinted dielectric F0 derived only from IOR.
  float  baseF0;
};

// DisneyBrdfData
// The direction pair being evaluated plus every dot product and tangent projection the lobes need.
// Computing them in one place keeps evaluation and sampling working from identical geometry.

struct DisneyBrdfData
{
  // View direction in world space, pointing away from the surface.
  float3 V;
  // Shading normal in world space.
  float3 N;
  // Light / outgoing direction in world space.
  float3 L;
  // Reflection or refraction half vector, depending on event type.
  float3 H;
  // Tangent axis.
  float3 X;
  // Bitangent axis.
  float3 Y;
  // Cosine between shading normal and view.
  float  NdotV;
  // Cosine between shading normal and light.
  float  NdotL;
  // Cosine between shading normal and half vector.
  float  NdotH;
  // Cosine between view and half vector.
  float  VdotH;
  // Cosine between light and half vector.
  float  LdotH;
  // Tangent projection of view.
  float  VdotX;
  // Bitangent projection of view.
  float  VdotY;
  // Tangent projection of light.
  float  LdotX;
  // Bitangent projection of light.
  float  LdotY;
  // Tangent projection of half vector.
  float  HdotX;
  // Bitangent projection of half vector.
  float  HdotY;
  // Incident IOR for the current interface orientation.
  float  etaI;
  // Transmitted IOR for the current interface orientation.
  float  etaT;
  // Relative IOR (etaI / etaT) used by transmission formulas.
  float  eta;
  // True when view and light stay on the same side of the shading normal.
  bool   reflection;
};

// DisneyLobeSetup
// Separates how much energy each lobe gets from how often each lobe is sampled.
// Evaluation multiplies by the weights; the sampler and the mixture PDF use the probabilities, which are normalized to sum to one.

struct DisneyLobeSetup
{
  // Energy weight used when evaluating the diffuse/subsurface lobe.
  float diffuseWeight;
  // Energy weight used when evaluating dielectric reflection.
  float dielectricWeight;
  // Energy weight used when evaluating metallic reflection.
  float metalWeight;
  // Energy weight used when evaluating transmission/refraction.
  float glassWeight;
  // Energy weight used when evaluating sheen.
  float sheenWeight;
  // Energy weight used when evaluating clearcoat.
  float clearcoatWeight;
  // Sampling probability of the diffuse/subsurface lobe.
  float diffuseProbability;
  // Sampling probability of the dielectric reflection lobe.
  float dielectricProbability;
  // Sampling probability of the metallic reflection lobe.
  float metalProbability;
  // Sampling probability of the glass branch before Fresnel splitting.
  float glassProbability;
  // Sampling probability of the sheen lobe.
  float sheenProbability;
  // Sampling probability of the clearcoat lobe.
  float clearcoatProbability;
};

// BRDF utilities
// Microfacet distributions, masking and visibility terms, Fresnel, and diffuse models. The epsilons in the denominators only keep grazing configurations finite.

// Isotropic GGX normal distribution function, written in a form that avoids cancellation near NdotH = 1.
float D_GGX(float alpha, float NdotH)
{
  const float oneMinusNoHSquared = 1.0 - NdotH * NdotH;
  const float a                  = NdotH * alpha;
  const float k                  = alpha / (oneMinusNoHSquared + a * a);

  return k * k * kInvPi;
}

// Anisotropic GGX NDF in the local tangent frame.
float D_GGX_Anisotropic(float alphaX, float alphaY, float HdotX, float HdotY, float NdotH)
{
  const float alphaSquared = alphaX * alphaY;
  const float3 d           = float3(alphaY * HdotX, alphaX * HdotY, alphaSquared * NdotH);
  const float  dSquared    = dot(d, d);
  const float  bSquared    = alphaSquared / max(dSquared, 1.0e-6);

  return alphaSquared * bSquared * bSquared * kInvPi;
}

// Charlie distribution used for the broad retro-reflective sheen lobe: (2 + 1/r) * sin(theta_h)^(1/r) / (2 * pi).
// The roughness floor keeps the 1/r exponent bounded.
float D_Charlie(float roughness, float NdotH)
{
  const float clampedRoughness = max(roughness, 0.045);
  const float invR             = 1.0 / clampedRoughness;
  const float cos2h            = NdotH * NdotH;
  const float sin2h            = max(0.0, 1.0 - cos2h);

  return (2.0 + invR) * pow(sin2h, invR * 0.5) * (0.5 * kInvPi);
}

// Single-direction GGX masking term G1, despite the V_ prefix.
float V_Smith_G1_GGX(float alpha, float NdotV)
{
  const float a = alpha * alpha;
  const float b = NdotV * NdotV;

  return (2.0 * NdotV) / max(NdotV + sqrt(max(a + b - a * b, 0.0)), 1.0e-5);
}

// Single-direction anisotropic GGX masking term G1. EvaluateMicrofacetReflection and EvaluateMicrofacetRefraction multiply two of these as a separable G2.
float V_Smith_G1_GGX_Anisotropic(float alphaX, float alphaY, float VdotX, float VdotY, float NdotV)
{
  const float a = VdotX * alphaX;
  const float b = VdotY * alphaY;

  return (2.0 * NdotV) / max(NdotV + sqrt(a * a + b * b + NdotV * NdotV), 1.0e-5);
}

// Correlated isotropic Smith visibility term, G2 / (4 * NdotV * NdotL).
float V_Smith_G2_Correlated_GGX(float alpha, float NdotV, float NdotL)
{
  const float a2      = alpha * alpha;
  const float lambdaV = NdotL * sqrt(max((NdotV - a2 * NdotV) * NdotV + a2, 0.0));
  const float lambdaL = NdotV * sqrt(max((NdotL - a2 * NdotL) * NdotL + a2, 0.0));

  return 0.5 / max(lambdaV + lambdaL, 1.0e-5);
}

// Correlated anisotropic Smith visibility term, G2 / (4 * NdotV * NdotL).
float V_Smith_G2_Correlated_GGX_Anisotropic(float alphaX, float alphaY, float VdotX, float VdotY, float LdotX, float LdotY, float NdotV, float NdotL)
{
  const float lambdaV = NdotL * length(float3(alphaX * VdotX, alphaY * VdotY, NdotV));
  const float lambdaL = NdotV * length(float3(alphaX * LdotX, alphaY * LdotY, NdotL));

  return 0.5 / max(lambdaV + lambdaL, 1.0e-5);
}

// Cheap visibility approximation traditionally used for clearcoat.
float V_Kelemen(float LdotH)
{
  return 0.25 / max(LdotH * LdotH, 1.0e-5);
}

// Visibility approximation paired here with the Charlie sheen distribution.
float V_Neubelt(float NdotV, float NdotL)
{
  return 1.0 / max(4.0 * (NdotL + NdotV - NdotL * NdotV), 1.0e-5);
}

// Shared fifth-power Schlick helper, (1 - u)^5.
float F_SchlickWeight(float u)
{
  const float m  = clamp(1.0 - u, 0.0, 1.0);
  const float m2 = m * m;

  return m * m2 * m2;
}

float F_Schlick(float f0, float f90, float u)
{
  const float w = F_SchlickWeight(u);

  return f0 + (f90 - f0) * w;
}

float3 F_Schlick(float3 f0, float3 f90, float u)
{
  const float w = F_SchlickWeight(u);

  return f0 + (f90 - f0) * w;
}

// Exact unpolarized Fresnel reflectance for dielectric interfaces, including total internal reflection.
float DielectricFresnel(float cosThetaI, float etaI, float etaT)
{
  // Snell's law gives sin^2 of the transmitted angle. Above one, no transmitted ray exists and everything reflects.
  const float sinThetaTSq = (etaI / etaT) * (etaI / etaT) * max(0.0, 1.0 - cosThetaI * cosThetaI);

  if(sinThetaTSq > 1.0)
  {
    return 1.0;
  }

  const float cosThetaT = sqrt(max(0.0, 1.0 - sinThetaTSq));
  const float rs        = (etaT * cosThetaI - etaI * cosThetaT) / max(etaT * cosThetaI + etaI * cosThetaT, 1.0e-5);
  const float rp        = (etaI * cosThetaI - etaT * cosThetaT) / max(etaI * cosThetaI + etaT * cosThetaT, 1.0e-5);

  return 0.5 * (rs * rs + rp * rp);
}

// Disney (Burley) diffuse term used for the default opaque base, with retro-reflection grazing response f90 = 0.5 + 2 * roughness * LdotH^2.
float Fd_Burley(float roughness, float NdotV, float NdotL, float LdotH)
{
  const float f90          = 0.5 + 2.0 * roughness * LdotH * LdotH;
  const float lightScatter = F_Schlick(1.0, f90, NdotL);
  const float viewScatter  = F_Schlick(1.0, f90, NdotV);

  return lightScatter * viewScatter * kInvPi;
}

// Burley's Hanrahan-Krueger-inspired approximation for a softer subsurface-like diffuse response.
float Fd_HanrahanKrueger(float roughness, float NdotV, float NdotL, float LdotH)
{
  const float fss90      = roughness * LdotH * LdotH;
  const float lightFss   = F_SchlickWeight(NdotL);
  const float viewFss    = F_SchlickWeight(NdotV);
  const float fss        = lerp(1.0, fss90, lightFss) * lerp(1.0, fss90, viewFss);

  return 1.25 * (fss * (1.0 / max(NdotL + NdotV, 1.0e-5) - 0.5) + 0.5) * kInvPi;
}

// Converts IOR into scalar normal-incidence reflectance, ((n - 1) / (n + 1))^2.
float DielectricF0(float refractionIndex)
{
  const float r = (refractionIndex - 1.0) / (refractionIndex + 1.0);

  return r * r;
}

// Produces a chroma-only tint so specular tint does not unintentionally change energy with luminance.
float3 CalculateTint(float3 albedo)
{
  const float luminanceValue = Luminance(albedo);

  return luminanceValue > 0.0 ? (albedo / luminanceValue) : (float3)1.0;
}

// Converts the resolved surface into the compact BSDF parameter set used by the Disney helper code below.
DisneyMaterialData GetDisneyMaterialData(SurfaceData surface)
{
  // Clamped parameters
  // The 0.045 roughness floor matches the one applied in LoadSurfaceData. The IOR floor of 1.01 keeps the interface from being index-matched: at eta = 1 a straight-through refraction gives L = -V and the transmission half vector L + V * eta degenerates to zero.

  DisneyMaterialData material;
  material.albedo             = surface.albedo;
  material.metallic           = clamp(surface.metallic, 0.0, 1.0);
  material.roughness          = clamp(surface.roughness, 0.045, 1.0);
  material.alpha              = material.roughness * material.roughness;
  material.transmission       = clamp(surface.transmission, 0.0, 1.0);
  material.refractionIndex    = max(surface.refractionIndex, 1.01);
  material.specular           = clamp(surface.specular, 0.0, 1.0);
  material.specularTint       = clamp(surface.specularTint, 0.0, 1.0);
  material.subsurface         = clamp(surface.subsurface, 0.0, 1.0);
  material.anisotropy         = clamp(surface.anisotropy, 0.0, 1.0);
  material.clearcoat          = clamp(surface.clearcoat, 0.0, 1.0);
  material.clearcoatRoughness = clamp(surface.clearcoatRoughness, 0.045, 1.0);
  material.clearcoatAlpha     = material.clearcoatRoughness * material.clearcoatRoughness;
  material.sheenColor         = clamp(surface.sheenColor, (float3)0.0, (float3)1.0);
  material.sheenRoughness     = clamp(surface.sheenRoughness, 0.0, 1.0);
  material.baseF0             = DielectricF0(material.refractionIndex);

  // Dielectric F0 is the IOR's reflectance, tinted toward the albedo's chroma and scaled by the specular strength.
  const float3 tint           = CalculateTint(material.albedo);
  material.specularColor      = (float3)material.baseF0 * lerp((float3)1.0, tint, material.specularTint) * material.specular;

  // Anisotropy
  // Maps the single anisotropy control onto separate tangent/bitangent roughnesses with the Disney aspect ratio sqrt(1 - 0.9 * anisotropy).
  // Both alphas are floored at 0.002025, which is 0.045 squared, the same floor as the isotropic alpha.

  const float aspect          = sqrt(max(1.0 - 0.9 * material.anisotropy, 0.001));
  material.alphaAnisotropicX  = max(0.002025, material.alpha / aspect);
  material.alphaAnisotropicY  = max(0.002025, material.alpha * aspect);

  return material;
}

// Precomputes the projections used by every lobe so evaluation and sampling stay consistent.
DisneyBrdfData PrepareBrdfData(SurfaceData surface, DisneyMaterialData material, float3 viewDir, float3 lightDir)
{
  // Frame and interface
  // A back-face hit means the ray is inside the material, so the incident and transmitted IORs swap.

  DisneyBrdfData data;
  data.V          = normalize(viewDir);
  data.N          = normalize(surface.shadingNormal);
  data.L          = normalize(lightDir);
  data.X          = normalize(surface.tangent);
  data.Y          = normalize(surface.bitangent);
  data.NdotV      = dot(data.N, data.V);
  data.NdotL      = dot(data.N, data.L);
  data.reflection = (data.NdotV * data.NdotL) > 0.0;
  data.etaI       = surface.isFrontFace != 0 ? 1.0 : material.refractionIndex;
  data.etaT       = surface.isFrontFace != 0 ? material.refractionIndex : 1.0;
  data.eta        = data.etaI / data.etaT;

  // Half vector
  // Transmission uses the generalized dielectric half vector consistent with refract(-V, H, etaI / etaT): wm ∝ wt + wo * eta, where eta = etaI / etaT.
  // Putting eta on the transmitted direction instead flips the transmission-side half-vector geometry and breaks Fresnel / D / G / PDF coherence for the BTDF.
  // The half vector is then flipped into the normal's hemisphere, where the microfacet distribution is defined.

  data.H          = data.NdotL <= 0.0 ? normalize(data.L + data.V * data.eta) : normalize(data.L + data.V);

  if(dot(data.N, data.H) < 0.0)
  {
    data.H = -data.H;
  }

  data.NdotH = dot(data.N, data.H);
  data.VdotH = dot(data.V, data.H);
  data.LdotH = dot(data.L, data.H);
  data.VdotX = dot(data.V, data.X);
  data.VdotY = dot(data.V, data.Y);
  data.LdotX = dot(data.L, data.X);
  data.LdotY = dot(data.L, data.Y);
  data.HdotX = dot(data.H, data.X);
  data.HdotY = dot(data.H, data.Y);

  return data;
}

// Separates "how much energy each lobe gets" from "how often each lobe is sampled".
DisneyLobeSetup GetLobeSetup(DisneyMaterialData material, DisneyBrdfData brdf)
{
  DisneyLobeSetup setup;

  // Energy weights
  // Lobe setup must not depend on the sampled light direction; continuation sampling builds it before L is known. Only NdotV is read from brdf.
  // The clearcoat layer reflects a Schlick share of the light at the view angle, and that share is removed from the base lobes beneath it. Glass and clearcoat are not scaled by it.
  // Diffuse and dielectric reflection share the non-metallic, non-transmissive share of the base.

  const float clearcoatEnergyLoss = 1.0 - (F_Schlick(0.04, 1.0, abs(brdf.NdotV)) * material.clearcoat);
  setup.diffuseWeight             = (1.0 - material.metallic) * (1.0 - material.transmission) * clearcoatEnergyLoss;
  setup.dielectricWeight          = setup.diffuseWeight;
  setup.metalWeight               = material.metallic * clearcoatEnergyLoss;
  setup.glassWeight               = (1.0 - material.metallic) * material.transmission;
  setup.sheenWeight               = clearcoatEnergyLoss;
  setup.clearcoatWeight           = material.clearcoat;

  // Sampling probabilities
  // The probabilities are luminance-weighted so bright lobes get sampled more often even when their energy weight is similar. The specular lobes use their Schlick reflectance at the view angle.
  // The total is floored so a material with no lobes normalizes to all-zero probabilities instead of NaN.

  const float schlickWeight         = F_SchlickWeight(abs(brdf.NdotV));
  const float diffuseProbability    = setup.diffuseWeight * Luminance(material.albedo);
  const float dielectricProbability = setup.dielectricWeight * Luminance(lerp(material.specularColor, (float3)1.0, schlickWeight));
  const float metalProbability      = setup.metalWeight * Luminance(lerp(material.albedo, (float3)1.0, schlickWeight));
  const float glassProbability      = setup.glassWeight;
  const float sheenProbability      = setup.sheenWeight * Luminance(material.sheenColor);
  const float clearcoatProbability  = setup.clearcoatWeight;

  const float total = max(diffuseProbability + dielectricProbability + metalProbability + glassProbability + sheenProbability + clearcoatProbability, 1.0e-5);

  setup.diffuseProbability    = diffuseProbability / total;
  setup.dielectricProbability = dielectricProbability / total;
  setup.metalProbability      = metalProbability / total;
  setup.glassProbability      = glassProbability / total;
  setup.sheenProbability      = sheenProbability / total;
  setup.clearcoatProbability  = clearcoatProbability / total;

  return setup;
}

// Lobe evaluation
// Each lobe returns its BSDF value f (without the cosine) and the solid-angle PDF of the proposal that samples it. The group functions further down combine lobes, apply the energy weights, and multiply by |NdotL|.

// Returns f for the diffuse family and reports the matching cosine-hemisphere PDF.
float3 EvaluateDisneyDiffuse(DisneyMaterialData material, DisneyBrdfData brdf, out float pdf)
{
  pdf = 0.0;

  if(brdf.NdotL <= 0.0)
  {
    return (float3)0.0;
  }

  const float diffuse     = Fd_Burley(material.roughness, abs(brdf.NdotV), abs(brdf.NdotL), abs(brdf.LdotH));
  const float subsurface  = Fd_HanrahanKrueger(material.roughness, abs(brdf.NdotV), abs(brdf.NdotL), abs(brdf.LdotH));
  pdf                     = abs(brdf.NdotL) * kInvPi;

  return lerp(diffuse, subsurface, material.subsurface) * material.albedo;
}

// Returns f for anisotropic microfacet reflection and the corresponding visible-normal PDF.
float3 EvaluateMicrofacetReflection(DisneyMaterialData material, DisneyBrdfData brdf, float3 fresnel, out float pdf)
{
  pdf = 0.0;

  if(brdf.NdotL <= 0.0)
  {
    return (float3)0.0;
  }

  const float D   = D_GGX_Anisotropic(material.alphaAnisotropicX, material.alphaAnisotropicY, brdf.HdotX, brdf.HdotY, abs(brdf.NdotH));
  const float G1V = V_Smith_G1_GGX_Anisotropic(material.alphaAnisotropicX, material.alphaAnisotropicY, brdf.VdotX, brdf.VdotY, abs(brdf.NdotV));
  const float G1L = V_Smith_G1_GGX_Anisotropic(material.alphaAnisotropicX, material.alphaAnisotropicY, brdf.LdotX, brdf.LdotY, abs(brdf.NdotL));

  // The VNDF half-vector density G1V * VdotH * D / NdotV times the reflection Jacobian 1 / (4 * VdotH) leaves D * G1V / (4 * NdotV).
  pdf = D * G1V / max(4.0 * abs(brdf.NdotV), 1.0e-5);

  // Cook-Torrance: F * D * G / (4 * NdotV * NdotL), with separable G = G1V * G1L.
  const float G = G1V * G1L;

  return fresnel * D * G / max(4.0 * abs(brdf.NdotV) * abs(brdf.NdotL), 1.0e-5);
}

// Returns f for anisotropic microfacet transmission and the corresponding refracted-direction PDF.
float3 EvaluateMicrofacetRefraction(DisneyMaterialData material, DisneyBrdfData brdf, float fresnel, out float pdf)
{
  pdf = 0.0;

  if(brdf.NdotL >= 0.0)
  {
    return (float3)0.0;
  }

  // V and L must each lie on the same side of the microfacet as of the macro surface, or the configuration is unreachable.
  if(brdf.VdotH * brdf.NdotV <= 0.0 || brdf.LdotH * brdf.NdotL <= 0.0)
  {
    return (float3)0.0;
  }

  const float D   = D_GGX_Anisotropic(material.alphaAnisotropicX, material.alphaAnisotropicY, brdf.HdotX, brdf.HdotY, abs(brdf.NdotH));
  const float G1V = V_Smith_G1_GGX_Anisotropic(material.alphaAnisotropicX, material.alphaAnisotropicY, brdf.VdotX, brdf.VdotY, abs(brdf.NdotV));
  const float G1L = V_Smith_G1_GGX_Anisotropic(material.alphaAnisotropicX, material.alphaAnisotropicY, brdf.LdotX, brdf.LdotY, abs(brdf.NdotL));

  // Refraction Jacobian
  // With wm ∝ wt + wo * eta, the transmission change-of-variables uses dwm / dwt = |wt . wm| / (wt . wm + eta * wo . wm)^2.
  // Matching this convention in both PDF and BSDF evaluation is what keeps bsdf/pdf coherent for glass.

  float denom  = brdf.LdotH + brdf.VdotH * brdf.eta;
  denom       *= denom;

  const float jacobian = abs(brdf.LdotH) / max(denom, 1.0e-5);

  pdf = D * G1V * abs(brdf.VdotH) * jacobian / max(abs(brdf.NdotV), 1.0e-5);

  // Walter BTDF
  // (1 - F) * D * G * |LdotH * VdotH| / (|NdotV * NdotL| * denom), the Walter et al. rough dielectric BTDF normalized by etaT^2.
  // The 1 / eta^2 factor accounts for radiance compressing or expanding across the interface.
  // The square root of albedo tints each crossing, so entering and leaving an object together tint by the albedo once.

  const float G             = G1V * G1L;
  const float transmission  = (1.0 - fresnel) * D * G * abs(brdf.LdotH * brdf.VdotH) / max(abs(brdf.NdotV * brdf.NdotL) * denom, 1.0e-5);
  const float radianceScale = 1.0 / max(brdf.eta * brdf.eta, 1.0e-5);

  return sqrt(material.albedo) * transmission * radianceScale;
}

// Sheen is evaluated as a broad reflection lobe with cosine-hemisphere sampling.
float3 EvaluateSheen(DisneyMaterialData material, DisneyBrdfData brdf, out float pdf)
{
  pdf = 0.0;

  if(brdf.NdotL <= 0.0)
  {
    return (float3)0.0;
  }

  pdf = abs(brdf.NdotL) * kInvPi;

  return D_Charlie(material.sheenRoughness, clamp(brdf.NdotH, 0.0, 1.0)) * V_Neubelt(abs(brdf.NdotV), abs(brdf.NdotL)) * material.sheenColor;
}

// Clearcoat is a narrow dielectric top layer with its own GGX / Kelemen model and a fixed F0 of 0.04 (IOR 1.5).
float3 EvaluateClearcoat(DisneyMaterialData material, DisneyBrdfData brdf, out float pdf)
{
  pdf = 0.0;

  if(brdf.NdotL <= 0.0)
  {
    return (float3)0.0;
  }

  const float D = D_GGX(material.clearcoatAlpha, abs(brdf.NdotH));
  const float V = V_Kelemen(abs(brdf.LdotH));
  const float F = F_Schlick(0.04, 1.0, abs(brdf.LdotH));

  // Half-vector density D * NdotH from ImportanceSampleGGX, times the reflection Jacobian 1 / (4 * LdotH).
  pdf = D * abs(brdf.NdotH) / max(4.0 * abs(brdf.LdotH), 1.0e-5);

  return (float3)(D * V * F);
}

// Broad lobes share the same cosine-weighted proposal, so direct lighting can treat them as one estimator family.
float BroadGroupProbability(DisneyLobeSetup setup)
{
  return setup.diffuseProbability + setup.sheenProbability;
}

// Dielectric and metallic reflection both use the same visible-GGX proposal, so continuation rays can sample them as one glossy-reflection family.
float ReflectionGroupProbability(DisneyLobeSetup setup)
{
  return setup.dielectricProbability + setup.metalProbability;
}

// Diffuse plus sheen, times |NdotL|, with the group's conditional PDF.
// This group is the "area-light / environment-light friendly" part of the BSDF. Everything here can be sampled by a cosine hemisphere proposal.
float3 EvaluateBroadBsdfGroup(DisneyMaterialData material, DisneyBrdfData brdf, DisneyLobeSetup setup, out float pdf)
{
  pdf = 0.0;

  if(!brdf.reflection)
  {
    return (float3)0.0;
  }

  float3 result = (float3)0.0;
  float  tmpPdf = 0.0;

  if(setup.diffuseProbability > 0.0)
  {
    result += EvaluateDisneyDiffuse(material, brdf, tmpPdf) * setup.diffuseWeight;
  }

  if(setup.sheenProbability > 0.0)
  {
    result += EvaluateSheen(material, brdf, tmpPdf) * setup.sheenWeight;
  }

  if(SafeMax3(result) <= 0.0)
  {
    return (float3)0.0;
  }

  // Diffuse and sheen both live on the same cosine proposal, so the group PDF is just the cosine-hemisphere PDF once, not one value per sub-lobe.
  pdf = abs(brdf.NdotL) * kInvPi;

  return result * abs(brdf.NdotL);
}

// Dielectric plus metallic glossy reflection, times |NdotL|, with the group's conditional PDF.
// This group is continuation-ray only. Direct lighting never samples it explicitly, so its PDF only needs to be coherent with the BSDF sampler.
float3 EvaluateReflectionBsdfGroup(DisneyMaterialData material, DisneyBrdfData brdf, DisneyLobeSetup setup, out float pdf)
{
  pdf = 0.0;

  if(!brdf.reflection)
  {
    return (float3)0.0;
  }

  float3 result = (float3)0.0;
  float  tmpPdf = 0.0;

  // Dielectric reflection
  // The exact dielectric Fresnel is rescaled from [baseF0, 1] into a [0, 1] weight, which then blends the tinted specularColor toward white. The tinted F0 thereby follows the exact Fresnel curve's shape.

  if(setup.dielectricProbability > 0.0)
  {
    const float fresnelWeight = (DielectricFresnel(abs(brdf.VdotH), brdf.etaI, brdf.etaT) - material.baseF0) / max(1.0 - material.baseF0, 1.0e-5);

    result += EvaluateMicrofacetReflection(material, brdf, lerp(material.specularColor, (float3)1.0, fresnelWeight), tmpPdf) * setup.dielectricWeight;

    // Dielectric and metal share the same visible-GGX half-vector proposal, so the conditional PDF for the whole reflection group is the same proposal.
    pdf = tmpPdf;
  }

  // Metallic reflection
  // Schlick Fresnel from the albedo as F0. Its PDF is the same VNDF density, so overwriting pdf leaves the value unchanged.

  if(setup.metalProbability > 0.0)
  {
    result += EvaluateMicrofacetReflection(material, brdf, lerp(material.albedo, (float3)1.0, F_SchlickWeight(abs(brdf.VdotH))), tmpPdf) * setup.metalWeight;
    pdf     = tmpPdf;
  }

  return result * abs(brdf.NdotL);
}

// Rough dielectric reflection or transmission, times |NdotL|, with the PDF including the Fresnel branch choice.
// Glass stays in its own group because the sampled event can flip between reflection and refraction after Fresnel branching.
float3 EvaluateGlassBsdfGroup(DisneyMaterialData material, DisneyBrdfData brdf, DisneyLobeSetup setup, out float pdf)
{
  pdf = 0.0;

  if(setup.glassProbability <= 0.0)
  {
    return (float3)0.0;
  }

  const float fresnel = DielectricFresnel(abs(brdf.VdotH), brdf.etaI, brdf.etaT);
  float       tmpPdf  = 0.0;

  // The sampler reflects with probability F, so the reflection PDF carries that factor.
  if(brdf.reflection)
  {
    const float3 reflection = EvaluateMicrofacetReflection(material, brdf, (float3)fresnel, tmpPdf) * setup.glassWeight;
    pdf                     = tmpPdf * fresnel;

    return reflection * abs(brdf.NdotL);
  }

  // The sampler refracts with probability 1 - F.
  const float3 transmission = EvaluateMicrofacetRefraction(material, brdf, fresnel, tmpPdf) * setup.glassWeight;
  pdf                       = tmpPdf * (1.0 - fresnel);

  return transmission * abs(brdf.NdotL);
}

// Clearcoat reflection, times |NdotL|, with its conditional PDF.
// Clearcoat keeps its own group because it uses its own isotropic GGX proposal instead of the base anisotropic reflection sampler.
float3 EvaluateClearcoatBsdfGroup(DisneyMaterialData material, DisneyBrdfData brdf, DisneyLobeSetup setup, out float pdf)
{
  pdf = 0.0;

  if(setup.clearcoatProbability <= 0.0)
  {
    return (float3)0.0;
  }

  return EvaluateClearcoat(material, brdf, pdf) * setup.clearcoatWeight * abs(brdf.NdotL);
}

// Full BSDF (times |NdotL|) and the full mixture PDF for a fixed direction pair.
// This is the general BSDF evaluation covering every group; the tracer's own direct lighting uses the broad-only EvaluateDirectLightBsdf below.
float3 EvaluateSurfaceBsdf(SurfaceData surface, float3 viewDir, float3 lightDir, out float pdf)
{
  const DisneyMaterialData material = GetDisneyMaterialData(surface);
  const DisneyBrdfData     brdf     = PrepareBrdfData(surface, material, viewDir, lightDir);
  const DisneyLobeSetup    setup    = GetLobeSetup(material, brdf);

  float broadPdf = 0.0;
  float specPdf  = 0.0;
  float glassPdf = 0.0;
  float coatPdf  = 0.0;

  const float3 broad = EvaluateBroadBsdfGroup(material, brdf, setup, broadPdf);
  const float3 spec  = EvaluateReflectionBsdfGroup(material, brdf, setup, specPdf);
  const float3 glass = EvaluateGlassBsdfGroup(material, brdf, setup, glassPdf);
  const float3 coat  = EvaluateClearcoatBsdfGroup(material, brdf, setup, coatPdf);

  // This is the full mixture PDF in outgoing-direction solid angle. It is what direct-light MIS should compare against when it wants to ask: "how likely was the BSDF path to generate this same direction?"
  pdf = broadPdf * BroadGroupProbability(setup) + specPdf * ReflectionGroupProbability(setup) + glassPdf * setup.glassProbability + coatPdf * setup.clearcoatProbability;

  return broad + spec + glass + coat;
}

// Evaluates one specific proposal group for a fixed direction, returning that group's contribution and the JOINT density p(omega, lobe) = P(lobe) * p(omega|lobe).
// This is the exact inverse of what SampleSurfaceBsdf does: it divides the same group's contribution by the same joint density. Feeding back the lobe that sampler chose therefore reproduces its bsdfOverPdf exactly.
// Needed by shift mappings, which redirect a path through a geometrically fixed direction and must reproduce the base path's per-lobe factorization. The full-mixture EvaluateSurfaceBsdf cannot: it sums all groups and reports a marginal PDF.
// Lobe kinds: 0 broad, 1 glossy reflection, 2 glass, anything else clearcoat.
float3 EvaluateSurfaceBsdfGroup(SurfaceData surface, float3 viewDir, float3 lightDir, uint lobeKind, out float pdf)
{
  const DisneyMaterialData material = GetDisneyMaterialData(surface);

  // Group probabilities come from the same shading-normal probe setup the sampler used, so the reproduced density matches the one the base path divided by.
  // GetLobeSetup reads only NdotV today, which makes the probe equivalent to the sampled direction, but the probe keeps this parity explicit.
  const DisneyBrdfData  probeBrdf  = PrepareBrdfData(surface, material, viewDir, surface.shadingNormal);
  const DisneyLobeSetup setup      = GetLobeSetup(material, probeBrdf);
  const DisneyBrdfData  sampleBrdf = PrepareBrdfData(surface, material, viewDir, lightDir);

  float  conditionalPdf   = 0.0;
  float  groupProbability = 0.0;
  float3 contribution     = (float3)0.0;

  if(lobeKind == 0u)
  {
    contribution     = EvaluateBroadBsdfGroup(material, sampleBrdf, setup, conditionalPdf);
    groupProbability = BroadGroupProbability(setup);
  }
  else if(lobeKind == 1u)
  {
    contribution     = EvaluateReflectionBsdfGroup(material, sampleBrdf, setup, conditionalPdf);
    groupProbability = ReflectionGroupProbability(setup);
  }
  else if(lobeKind == 2u)
  {
    contribution     = EvaluateGlassBsdfGroup(material, sampleBrdf, setup, conditionalPdf);
    groupProbability = setup.glassProbability;
  }
  else
  {
    contribution     = EvaluateClearcoatBsdfGroup(material, sampleBrdf, setup, conditionalPdf);
    groupProbability = setup.clearcoatProbability;
  }

  pdf = groupProbability * conditionalPdf;

  return contribution;
}

// Explicit direct lighting estimates only the broad cosine-proposal BSDF group. Glossy / transmission groups are owned by continuation rays.
float3 EvaluateDirectLightBsdf(SurfaceData surface, float3 viewDir, float3 lightDir, out float pdf)
{
  const DisneyMaterialData material = GetDisneyMaterialData(surface);
  const DisneyBrdfData     brdf     = PrepareBrdfData(surface, material, viewDir, lightDir);
  const DisneyLobeSetup    setup    = GetLobeSetup(material, brdf);

  float groupPdf = 0.0;

  const float3 result = EvaluateBroadBsdfGroup(material, brdf, setup, groupPdf);

  // The direct-light estimator samples the whole broad group as one proposal, so the reported PDF must include the probability of choosing that group.
  pdf                 = groupPdf * BroadGroupProbability(setup);

  return result;
}

// Lobe sampling
// Importance sampling of the proposal groups for continuation rays.

// Samples proposal groups explicitly: broad cosine lobes, glossy reflection, glass, and clearcoat.
// The return values are intentionally split. bsdfOverPdf is the throughput for the sampled continuation event. directLightPdf is only the PDF of the subset that direct-light sampling can compete with later in miss/emissive-hit MIS.
// sampledLobeKind reports the group (0 broad, 1 reflection, 2 glass, 3 clearcoat) so EvaluateSurfaceBsdfGroup can reproduce the same factorization.
bool SampleSurfaceBsdf(SurfaceData surface, float3 viewDir, inout PathSampleStream seed, out float3 lightDir, out float3 bsdfOverPdf, out float directLightPdf, out bool isTransmissionEvent, out uint sampledLobeKind)
{
  // Lobe setup
  // Built from a probe with L set to the shading normal, because the sampled direction is not known yet.

  const DisneyMaterialData material  = GetDisneyMaterialData(surface);
  const DisneyBrdfData     probeBrdf = PrepareBrdfData(surface, material, viewDir, surface.shadingNormal);
  const DisneyLobeSetup    setup     = GetLobeSetup(material, probeBrdf);
  lightDir                           = (float3)0.0;
  bsdfOverPdf                        = (float3)0.0;
  isTransmissionEvent                = false;
  directLightPdf                     = 0.0;
  sampledLobeKind                    = 0u;

  const float broadProbability      = BroadGroupProbability(setup);
  const float reflectionProbability = ReflectionGroupProbability(setup);
  const float glassProbability      = setup.glassProbability;
  const float clearcoatProbability  = setup.clearcoatProbability;
  const float totalProbability      = broadProbability + reflectionProbability + glassProbability + clearcoatProbability;

  if(totalProbability <= 0.0)
  {
    return false;
  }

  // Group selection
  // One uniform picks the group from the probabilities' running sum; clearcoat takes whatever lies above cdfGlass.
  // Every lobe uses the same two direction dimensions, so lobe changes do not shift later decisions.

  const float  sampleGroup      = NextRandom(seed);
  const float2 xi               = NextRandom2(seed);
  const float  cdfBroad         = broadProbability;
  const float  cdfReflection    = cdfBroad + reflectionProbability;
  const float  cdfGlass         = cdfReflection + glassProbability;
  const bool   sampleBroad      = sampleGroup < cdfBroad;
  const bool   sampleReflection = sampleGroup >= cdfBroad && sampleGroup < cdfReflection;
  const bool   sampleGlass      = sampleGroup >= cdfReflection && sampleGroup < cdfGlass;

  // Direction sampling

  if(sampleBroad)
  {
    sampledLobeKind = 0u;

    // One cosine sample stands in for both diffuse and sheen. The BSDF evaluation later resolves how much each sub-lobe contributes.
    float ignoredPdf = 0.0;
    lightDir         = SampleCosineHemisphereDirection(surface.shadingNormal, xi, ignoredPdf);
  }
  else if(sampleReflection || sampleGlass)
  {
    sampledLobeKind = sampleReflection ? 1u : 2u;

    // Glossy reflection and glass start from the same visible-GGX microfacet sample. Glass then does a second Fresnel branch to decide reflection vs refraction for that sampled microfacet.
    const float3 viewLocal = ToLocal(surface.shadingNormal, surface.tangent, surface.bitangent, normalize(viewDir));
    float3       halfLocal = ImportanceSampleVisibleGGX(viewLocal, material.alphaAnisotropicX, material.alphaAnisotropicY, xi);

    if(halfLocal.z < 0.0)
    {
      halfLocal = -halfLocal;
    }

    const float3 halfVector = ToWorld(surface.shadingNormal, surface.tangent, surface.bitangent, halfLocal);

    if(sampleReflection)
    {
      lightDir = normalize(reflect(-viewDir, halfVector));
    }
    else
    {
      // The group selector is rescaled within the glass interval and reused as the Fresnel coin, which saves a random draw.
      const float etaI     = surface.isFrontFace != 0 ? 1.0 : material.refractionIndex;
      const float etaT     = surface.isFrontFace != 0 ? material.refractionIndex : 1.0;
      const float fresnel  = DielectricFresnel(abs(dot(normalize(viewDir), halfVector)), etaI, etaT);
      const float remapped = RescaleRandomNumber(sampleGroup, cdfReflection, cdfGlass);

      if(remapped < fresnel)
      {
        lightDir = normalize(reflect(-viewDir, halfVector));
      }
      else
      {
        const float eta = etaI / etaT;
        lightDir        = normalize(refract(-viewDir, halfVector, eta));

        // Intended to fall back to reflection when refract() reports total internal reflection with a zero vector.
        if(dot(lightDir, lightDir) <= 0.0)
        {
          lightDir = normalize(reflect(-viewDir, halfVector));
        }
      }
    }
  }
  else
  {
    sampledLobeKind = 3u;

    float3 halfLocal = ImportanceSampleGGX(material.clearcoatAlpha, xi);

    if(halfLocal.z < 0.0)
    {
      halfLocal = -halfLocal;
    }

    const float3 halfVector = ToWorld(surface.shadingNormal, surface.tangent, surface.bitangent, halfLocal);
    lightDir                = normalize(reflect(-viewDir, halfVector));
  }

  if(dot(lightDir, lightDir) <= 0.0)
  {
    return false;
  }

  // Contribution and PDF
  // Rebuild the BRDF data at the sampled direction so the sampled event and the evaluated contribution are driven by the same geometry/Fresnel terms.
  // Each branch divides by the joint density P(group) * p(omega | group), the same factorization EvaluateSurfaceBsdfGroup reproduces.

  const DisneyBrdfData sampleBrdf     = PrepareBrdfData(surface, material, viewDir, lightDir);
  float                conditionalPdf = 0.0;
  float3               contribution   = (float3)0.0;
  float                samplePdf      = 0.0;

  if(sampleBroad)
  {
    contribution  = EvaluateBroadBsdfGroup(material, sampleBrdf, setup, conditionalPdf);
    samplePdf     = broadProbability * conditionalPdf;

    // Only broad-group samples can be compared against explicit direct-light sampling, so only this branch exports a direct-light-compatible PDF.
    directLightPdf = samplePdf;
  }
  else if(sampleReflection)
  {
    contribution = EvaluateReflectionBsdfGroup(material, sampleBrdf, setup, conditionalPdf);
    samplePdf    = reflectionProbability * conditionalPdf;
  }
  else if(sampleGlass)
  {
    contribution        = EvaluateGlassBsdfGroup(material, sampleBrdf, setup, conditionalPdf);
    samplePdf           = glassProbability * conditionalPdf;
    isTransmissionEvent = !sampleBrdf.reflection;
  }
  else
  {
    contribution = EvaluateClearcoatBsdfGroup(material, sampleBrdf, setup, conditionalPdf);
    samplePdf    = clearcoatProbability * conditionalPdf;
  }

  // A sample the group cannot produce (for example a reflection that went below the surface) terminates the path.
  if(samplePdf <= 0.0 || SafeMax3(contribution) <= 0.0)
  {
    isTransmissionEvent = false;
    directLightPdf      = 0.0;
    return false;
  }

  // Continuation throughput always uses the actual sampled proposal PDF, even though miss/emissive MIS may later use a narrower directLightPdf contract.
  bsdfOverPdf = contribution / samplePdf;

  return true;
}

#endif

