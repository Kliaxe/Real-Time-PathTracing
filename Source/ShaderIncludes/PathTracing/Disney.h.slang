#ifndef PATH_TRACING_DISNEY_H
#define PATH_TRACING_DISNEY_H

#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"
#include "ShaderIncludes/PathTracing/MonteCarlo.h.slang"

// -------------------------------------------------------------------------
//    BRDF data
// -------------------------------------------------------------------------

struct DisneyMaterialData
{
  float3 albedo;              // Base diffuse / metal color after texturing.
  float3 specularColor;       // Dielectric F0 color after specular + tint remapping.
  float3 sheenColor;          // RGB sheen reflectance resolved from the material.
  float  metallic;            // Metalness selector for base lobe partitioning.
  float  roughness;           // Perceptual roughness kept for diffuse terms.
  float  alpha;               // Squared roughness used by GGX.
  float  alphaAnisotropicX;   // Tangent-axis GGX alpha.
  float  alphaAnisotropicY;   // Bitangent-axis GGX alpha.
  float  transmission;        // Transmission strength for glass behavior.
  float  refractionIndex;     // IOR shared by reflection / refraction Fresnel.
  float  specular;            // Scalar dielectric specular scale.
  float  specularTint;        // Amount of albedo tint applied to dielectric F0.
  float  subsurface;          // Blend factor between Burley diffuse and HK-style subsurface diffuse.
  float  anisotropy;          // Authoring anisotropy input used to derive alphaX / alphaY.
  float  clearcoat;           // Extra top-layer lobe strength.
  float  clearcoatRoughness;  // Perceptual clearcoat roughness.
  float  clearcoatAlpha;      // Squared clearcoat roughness.
  float  sheenRoughness;      // Charlie sheen roughness parameter.
  float  baseF0;              // Untinted dielectric F0 derived only from IOR.
};

struct DisneyBrdfData
{
  float3 V;           // View direction in world space, pointing away from the surface.
  float3 N;           // Shading normal in world space.
  float3 L;           // Light / outgoing direction in world space.
  float3 H;           // Reflection or refraction half vector, depending on event type.
  float3 X;           // Tangent axis.
  float3 Y;           // Bitangent axis.
  float  NdotV;       // Cosine between shading normal and view.
  float  NdotL;       // Cosine between shading normal and light.
  float  NdotH;       // Cosine between shading normal and half vector.
  float  VdotH;       // Cosine between view and half vector.
  float  LdotH;       // Cosine between light and half vector.
  float  VdotX;       // Tangent projection of view.
  float  VdotY;       // Bitangent projection of view.
  float  LdotX;       // Tangent projection of light.
  float  LdotY;       // Bitangent projection of light.
  float  HdotX;       // Tangent projection of half vector.
  float  HdotY;       // Bitangent projection of half vector.
  float  etaI;        // Incident IOR for the current interface orientation.
  float  etaT;        // Transmitted IOR for the current interface orientation.
  float  eta;         // Relative IOR (etaI / etaT) used by transmission formulas.
  bool   reflection;  // True when view and light stay on the same side of the shading normal.
};

struct DisneyLobeSetup
{
  float diffuseWeight;          // Energy weight used when evaluating the diffuse/subsurface lobe.
  float dielectricWeight;       // Energy weight used when evaluating dielectric reflection.
  float metalWeight;            // Energy weight used when evaluating metallic reflection.
  float glassWeight;            // Energy weight used when evaluating transmission/refraction.
  float sheenWeight;            // Energy weight used when evaluating sheen.
  float clearcoatWeight;        // Energy weight used when evaluating clearcoat.
  float diffuseProbability;     // Sampling probability of the diffuse/subsurface lobe.
  float dielectricProbability;  // Sampling probability of the dielectric reflection lobe.
  float metalProbability;       // Sampling probability of the metallic reflection lobe.
  float glassProbability;       // Sampling probability of the glass branch before Fresnel splitting.
  float sheenProbability;       // Sampling probability of the sheen lobe.
  float clearcoatProbability;   // Sampling probability of the clearcoat lobe.
};

// -------------------------------------------------------------------------
//    BRDF utilities
// -------------------------------------------------------------------------

float D_GGX(float alpha, float NdotH)
{
  // Isotropic GGX normal distribution function.
  const float oneMinusNoHSquared = 1.0 - NdotH * NdotH;
  const float a                  = NdotH * alpha;
  const float k                  = alpha / (oneMinusNoHSquared + a * a);
  return k * k * kInvPi;
}

float D_GGX_Anisotropic(float alphaX, float alphaY, float HdotX, float HdotY, float NdotH)
{
  // Anisotropic GGX NDF in the local tangent frame.
  const float alphaSquared = alphaX * alphaY;
  const float3 d           = float3(alphaY * HdotX, alphaX * HdotY, alphaSquared * NdotH);
  const float  dSquared    = dot(d, d);
  const float  bSquared    = alphaSquared / max(dSquared, 1.0e-6);
  return alphaSquared * bSquared * bSquared * kInvPi;
}

float D_Charlie(float roughness, float NdotH)
{
  // Charlie distribution used for the broad retro-reflective sheen lobe.
  const float clampedRoughness = max(roughness, 0.045);
  const float invR             = 1.0 / clampedRoughness;
  const float cos2h            = NdotH * NdotH;
  const float sin2h            = max(0.0, 1.0 - cos2h);
  return (2.0 + invR) * pow(sin2h, invR * 0.5) * (0.5 * kInvPi);
}

float V_Smith_G1_GGX(float alpha, float NdotV)
{
  // Single-direction GGX masking term.
  const float a = alpha * alpha;
  const float b = NdotV * NdotV;
  return (2.0 * NdotV) / max(NdotV + sqrt(max(a + b - a * b, 0.0)), 1.0e-5);
}

float V_Smith_G1_GGX_Anisotropic(float alphaX, float alphaY, float VdotX, float VdotY, float NdotV)
{
  // Single-direction anisotropic GGX masking term.
  const float a = VdotX * alphaX;
  const float b = VdotY * alphaY;
  return (2.0 * NdotV) / max(NdotV + sqrt(a * a + b * b + NdotV * NdotV), 1.0e-5);
}

float V_Smith_G2_Correlated_GGX(float alpha, float NdotV, float NdotL)
{
  // Correlated isotropic Smith visibility term.
  const float a2      = alpha * alpha;
  const float lambdaV = NdotL * sqrt(max((NdotV - a2 * NdotV) * NdotV + a2, 0.0));
  const float lambdaL = NdotV * sqrt(max((NdotL - a2 * NdotL) * NdotL + a2, 0.0));
  return 0.5 / max(lambdaV + lambdaL, 1.0e-5);
}

float V_Smith_G2_Correlated_GGX_Anisotropic(float alphaX,
                                            float alphaY,
                                            float VdotX,
                                            float VdotY,
                                            float LdotX,
                                            float LdotY,
                                            float NdotV,
                                            float NdotL)
{
  // Correlated anisotropic Smith visibility term.
  const float lambdaV = NdotL * length(float3(alphaX * VdotX, alphaY * VdotY, NdotV));
  const float lambdaL = NdotV * length(float3(alphaX * LdotX, alphaY * LdotY, NdotL));
  return 0.5 / max(lambdaV + lambdaL, 1.0e-5);
}

float V_Kelemen(float LdotH)
{
  // Cheap visibility approximation traditionally used for clearcoat.
  return 0.25 / max(LdotH * LdotH, 1.0e-5);
}

float V_Neubelt(float NdotV, float NdotL)
{
  // Visibility approximation paired here with the Charlie sheen distribution.
  return 1.0 / max(4.0 * (NdotL + NdotV - NdotL * NdotV), 1.0e-5);
}

float F_SchlickWeight(float u)
{
  // Shared fifth-power Schlick helper.
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

float DielectricFresnel(float cosThetaI, float etaI, float etaT)
{
  // Exact Fresnel for dielectric interfaces, including total internal reflection.
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

float Fd_Burley(float roughness, float NdotV, float NdotL, float LdotH)
{
  // Disney diffuse term used for the default opaque base.
  const float f90          = 0.5 + 2.0 * roughness * LdotH * LdotH;
  const float lightScatter = F_Schlick(1.0, f90, NdotL);
  const float viewScatter  = F_Schlick(1.0, f90, NdotV);
  return lightScatter * viewScatter * kInvPi;
}

float Fd_HanrahanKrueger(float roughness, float NdotV, float NdotL, float LdotH)
{
  // Burley-style approximation for a softer subsurface-like diffuse response.
  const float fss90      = roughness * LdotH * LdotH;
  const float lightFss   = F_SchlickWeight(NdotL);
  const float viewFss    = F_SchlickWeight(NdotV);
  const float fss        = lerp(1.0, fss90, lightFss) * lerp(1.0, fss90, viewFss);
  return 1.25 * (fss * (1.0 / max(NdotL + NdotV, 1.0e-5) - 0.5) + 0.5) * kInvPi;
}

float DielectricF0(float refractionIndex)
{
  // Converts IOR into scalar normal-incidence reflectance.
  const float r = (refractionIndex - 1.0) / (refractionIndex + 1.0);
  return r * r;
}

float3 CalculateTint(float3 albedo)
{
  // Produces a chroma-only tint so specular tint does not unintentionally change energy with luminance.
  const float luminance = Luminance(albedo);
  return luminance > 0.0 ? (albedo / luminance) : float3(1.0);
}

DisneyMaterialData GetDisneyMaterialData(SurfaceData surface)
{
  // Converts the resolved surface into the compact BSDF parameter set used by the Disney helper code below.
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
  material.sheenColor         = clamp(surface.sheenColor, float3(0.0), float3(1.0));
  material.sheenRoughness     = clamp(surface.sheenRoughness, 0.0, 1.0);
  material.baseF0             = DielectricF0(material.refractionIndex);

  const float3 tint           = CalculateTint(material.albedo);
  material.specularColor      = float3(material.baseF0) * lerp(float3(1.0), tint, material.specularTint) * material.specular;

  // Maps the single anisotropy control onto separate tangent/bitangent roughnesses.
  const float aspect          = sqrt(max(1.0 - 0.9 * material.anisotropy, 0.001));
  material.alphaAnisotropicX  = max(0.002025, material.alpha / aspect);
  material.alphaAnisotropicY  = max(0.002025, material.alpha * aspect);
  return material;
}

DisneyBrdfData PrepareBrdfData(SurfaceData surface, DisneyMaterialData material, float3 viewDir, float3 lightDir)
{
  // Precomputes the projections used by every lobe so evaluation and sampling stay consistent.
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
  // Transmission uses the generalized dielectric half vector consistent with refract(-V, H, etaI / etaT):
  // wm ∝ wt + wo * eta, where eta = etaI / etaT. Putting eta on the transmitted direction instead flips the
  // transmission-side half-vector geometry and breaks Fresnel / D / G / PDF coherence for the BTDF.
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

DisneyLobeSetup GetLobeSetup(DisneyMaterialData material, DisneyBrdfData brdf)
{
  // Separates "how much energy each lobe gets" from "how often each lobe is sampled".
  DisneyLobeSetup setup;

  const float clearcoatEnergyLoss = 1.0 - (F_Schlick(0.04, 1.0, abs(brdf.LdotH)) * material.clearcoat);
  setup.diffuseWeight             = (1.0 - material.metallic) * (1.0 - material.transmission) * clearcoatEnergyLoss;
  setup.dielectricWeight          = setup.diffuseWeight;
  setup.metalWeight               = material.metallic * clearcoatEnergyLoss;
  setup.glassWeight               = (1.0 - material.metallic) * material.transmission;
  setup.sheenWeight               = clearcoatEnergyLoss;
  setup.clearcoatWeight           = material.clearcoat;

  // The probabilities are luminance-weighted so bright lobes get sampled more often even when their energy weight is similar.
  const float schlickWeight       = F_SchlickWeight(abs(brdf.NdotV));
  const float diffuseProbability  = setup.diffuseWeight * Luminance(material.albedo);
  const float dielectricProbability = setup.dielectricWeight * Luminance(lerp(material.specularColor, float3(1.0), schlickWeight));
  const float metalProbability    = setup.metalWeight * Luminance(lerp(material.albedo, float3(1.0), schlickWeight));
  const float glassProbability    = setup.glassWeight;
  const float sheenProbability    = setup.sheenWeight * Luminance(material.sheenColor);
  const float clearcoatProbability = setup.clearcoatWeight;

  const float total = max(
    diffuseProbability + 
    dielectricProbability + 
    metalProbability + 
    glassProbability + 
    sheenProbability + 
    clearcoatProbability, 1.0e-5);

  setup.diffuseProbability    = diffuseProbability / total;
  setup.dielectricProbability = dielectricProbability / total;
  setup.metalProbability      = metalProbability / total;
  setup.glassProbability      = glassProbability / total;
  setup.sheenProbability      = sheenProbability / total;
  setup.clearcoatProbability  = clearcoatProbability / total;
  return setup;
}

// -------------------------------------------------------------------------
//    Lobe evaluation
// -------------------------------------------------------------------------

float3 EvaluateDisneyDiffuse(DisneyMaterialData material, DisneyBrdfData brdf, out float pdf)
{
  // Returns f for the diffuse family and reports the matching cosine-hemisphere PDF.
  pdf = 0.0;
  if(brdf.NdotL <= 0.0)
  {
    return float3(0.0);
  }

  const float diffuse     = Fd_Burley(material.roughness, abs(brdf.NdotV), abs(brdf.NdotL), abs(brdf.LdotH));
  const float subsurface  = Fd_HanrahanKrueger(material.roughness, abs(brdf.NdotV), abs(brdf.NdotL), abs(brdf.LdotH));
  pdf                     = abs(brdf.NdotL) * kInvPi;
  return lerp(diffuse, subsurface, material.subsurface) * material.albedo;
}

float3 EvaluateMicrofacetReflection(DisneyMaterialData material, DisneyBrdfData brdf, float3 fresnel, out float pdf)
{
  // Returns f for anisotropic microfacet reflection and the corresponding visible-normal PDF.
  pdf = 0.0;
  if(brdf.NdotL <= 0.0)
  {
    return float3(0.0);
  }

  const float D  = D_GGX_Anisotropic(material.alphaAnisotropicX, material.alphaAnisotropicY, brdf.HdotX, brdf.HdotY, abs(brdf.NdotH));
  const float G1V = V_Smith_G1_GGX_Anisotropic(material.alphaAnisotropicX, material.alphaAnisotropicY, brdf.VdotX, brdf.VdotY, abs(brdf.NdotV));
  const float G1L = V_Smith_G1_GGX_Anisotropic(material.alphaAnisotropicX, material.alphaAnisotropicY, brdf.LdotX, brdf.LdotY, abs(brdf.NdotL));

  pdf = D * G1V / max(4.0 * abs(brdf.NdotV), 1.0e-5);

  const float G = G1V * G1L;
  return fresnel * D * G / max(4.0 * abs(brdf.NdotV) * abs(brdf.NdotL), 1.0e-5);
}

float3 EvaluateMicrofacetRefraction(DisneyMaterialData material, DisneyBrdfData brdf, float fresnel, out float pdf)
{
  // Returns f for anisotropic microfacet transmission and the corresponding refracted-direction PDF.
  pdf = 0.0;
  if(brdf.NdotL >= 0.0)
  {
    return float3(0.0);
  }

  if(brdf.VdotH * brdf.NdotV <= 0.0 || brdf.LdotH * brdf.NdotL <= 0.0)
  {
    return float3(0.0);
  }

  const float D  = D_GGX_Anisotropic(material.alphaAnisotropicX, material.alphaAnisotropicY, brdf.HdotX, brdf.HdotY, abs(brdf.NdotH));
  const float G1V =
      V_Smith_G1_GGX_Anisotropic(material.alphaAnisotropicX, material.alphaAnisotropicY, brdf.VdotX, brdf.VdotY, abs(brdf.NdotV));
  const float G1L =
      V_Smith_G1_GGX_Anisotropic(material.alphaAnisotropicX, material.alphaAnisotropicY, brdf.LdotX, brdf.LdotY, abs(brdf.NdotL));

  // With wm ∝ wt + wo * eta, the transmission change-of-variables uses:
  //   dwm / dwt = |wt . wm| / (wt . wm + eta * wo . wm)^2
  // Matching this convention in both PDF and BSDF evaluation is what keeps bsdf/pdf coherent for glass.
  float denom    = brdf.LdotH + brdf.VdotH * brdf.eta;
  denom *= denom;
  const float jacobian = abs(brdf.LdotH) / max(denom, 1.0e-5);

  pdf = D * G1V * abs(brdf.VdotH) * jacobian / max(abs(brdf.NdotV), 1.0e-5);

  const float G             = G1V * G1L;
  const float transmission  =
      (1.0 - fresnel) * D * G * abs(brdf.LdotH * brdf.VdotH)
      / max(abs(brdf.NdotV * brdf.NdotL) * denom, 1.0e-5);
  const float radianceScale = 1.0 / max(brdf.eta * brdf.eta, 1.0e-5);
  return sqrt(material.albedo) * transmission * radianceScale;
}

float3 EvaluateSheen(DisneyMaterialData material, DisneyBrdfData brdf, out float pdf)
{
  // Sheen is evaluated as a broad reflection lobe with cosine-hemisphere sampling.
  pdf = 0.0;
  if(brdf.NdotL <= 0.0)
  {
    return float3(0.0);
  }

  pdf = abs(brdf.NdotL) * kInvPi;
  return D_Charlie(material.sheenRoughness, clamp(brdf.NdotH, 0.0, 1.0)) * V_Neubelt(abs(brdf.NdotV), abs(brdf.NdotL))
         * material.sheenColor;
}

float3 EvaluateClearcoat(DisneyMaterialData material, DisneyBrdfData brdf, out float pdf)
{
  // Clearcoat is a narrow dielectric top layer with its own GGX / Kelemen model.
  pdf = 0.0;
  if(brdf.NdotL <= 0.0)
  {
    return float3(0.0);
  }

  const float D = D_GGX(material.clearcoatAlpha, abs(brdf.NdotH));
  const float V = V_Kelemen(abs(brdf.LdotH));
  const float F = F_Schlick(0.04, 1.0, abs(brdf.LdotH));

  pdf = D * abs(brdf.NdotH) / max(4.0 * abs(brdf.LdotH), 1.0e-5);
  return float3(D * V * F);
}

float BroadGroupProbability(DisneyLobeSetup setup)
{
  // Broad lobes share the same cosine-weighted proposal, so direct lighting can
  // treat them as one estimator family.
  return setup.diffuseProbability + setup.sheenProbability;
}

float ReflectionGroupProbability(DisneyLobeSetup setup)
{
  // Dielectric and metallic reflection both use the same visible-GGX proposal,
  // so continuation rays can sample them as one glossy-reflection family.
  return setup.dielectricProbability + setup.metalProbability;
}

float3 EvaluateBroadBsdfGroup(DisneyMaterialData material, DisneyBrdfData brdf, DisneyLobeSetup setup, out float pdf)
{
  // This group is the "area-light / environment-light friendly" part of the
  // BSDF. Everything here can be sampled by a cosine hemisphere proposal.
  pdf = 0.0;
  if(!brdf.reflection)
  {
    return float3(0.0);
  }

  float3 result = float3(0.0);
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
    return float3(0.0);
  }

  // Diffuse and sheen both live on the same cosine proposal, so the group PDF
  // is just the cosine-hemisphere PDF once, not one value per sub-lobe.
  pdf = abs(brdf.NdotL) * kInvPi;
  return result * abs(brdf.NdotL);
}

float3 EvaluateReflectionBsdfGroup(DisneyMaterialData material, DisneyBrdfData brdf, DisneyLobeSetup setup, out float pdf)
{
  // This group is continuation-ray only. Direct lighting never samples it
  // explicitly, so its PDF only needs to be coherent with the BSDF sampler.
  pdf = 0.0;
  if(!brdf.reflection)
  {
    return float3(0.0);
  }

  float3 result = float3(0.0);
  float  tmpPdf = 0.0;

  if(setup.dielectricProbability > 0.0)
  {
    const float fresnelWeight =
        (DielectricFresnel(abs(brdf.VdotH), brdf.etaI, brdf.etaT) - material.baseF0) / max(1.0 - material.baseF0, 1.0e-5);
    result += EvaluateMicrofacetReflection(material, brdf, lerp(material.specularColor, float3(1.0), fresnelWeight), tmpPdf)
              * setup.dielectricWeight;
    // Dielectric and metal share the same visible-GGX half-vector proposal, so
    // the conditional PDF for the whole reflection group is the same proposal.
    pdf = tmpPdf;
  }

  if(setup.metalProbability > 0.0)
  {
    result += EvaluateMicrofacetReflection(material, brdf, lerp(material.albedo, float3(1.0), F_SchlickWeight(abs(brdf.VdotH))), tmpPdf)
              * setup.metalWeight;
    pdf = tmpPdf;
  }

  return result * abs(brdf.NdotL);
}

float3 EvaluateGlassBsdfGroup(DisneyMaterialData material, DisneyBrdfData brdf, DisneyLobeSetup setup, out float pdf)
{
  // Glass stays in its own group because the sampled event can flip between
  // reflection and refraction after Fresnel branching.
  pdf = 0.0;
  if(setup.glassProbability <= 0.0)
  {
    return float3(0.0);
  }

  const float fresnel = DielectricFresnel(abs(brdf.VdotH), brdf.etaI, brdf.etaT);
  float       tmpPdf  = 0.0;

  if(brdf.reflection)
  {
    const float3 reflection = EvaluateMicrofacetReflection(material, brdf, float3(fresnel), tmpPdf) * setup.glassWeight;
    pdf                     = tmpPdf * fresnel;
    return reflection * abs(brdf.NdotL);
  }

  const float3 transmission = EvaluateMicrofacetRefraction(material, brdf, fresnel, tmpPdf) * setup.glassWeight;
  pdf                       = tmpPdf * (1.0 - fresnel);
  return transmission * abs(brdf.NdotL);
}

float3 EvaluateClearcoatBsdfGroup(DisneyMaterialData material, DisneyBrdfData brdf, DisneyLobeSetup setup, out float pdf)
{
  // Clearcoat keeps its own group because it uses its own isotropic GGX
  // proposal instead of the base anisotropic reflection sampler.
  pdf = 0.0;
  if(setup.clearcoatProbability <= 0.0)
  {
    return float3(0.0);
  }

  return EvaluateClearcoat(material, brdf, pdf) * setup.clearcoatWeight * abs(brdf.NdotL);
}

float3 EvaluateSurfaceBsdf(SurfaceData surface, float3 viewDir, float3 lightDir, out float pdf)
{
  // This is the source-of-truth BSDF evaluation used by both direct lighting and continuation rays.
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

  // This is the full mixture PDF in outgoing-direction solid angle. It is what
  // direct-light MIS should compare against when it wants to ask:
  // "how likely was the BSDF path to generate this same direction?"
  pdf = broadPdf * BroadGroupProbability(setup) + specPdf * ReflectionGroupProbability(setup) + glassPdf * setup.glassProbability
        + coatPdf * setup.clearcoatProbability;

  return broad + spec + glass + coat;
}

float3 EvaluateDirectLightBsdf(SurfaceData surface, float3 viewDir, float3 lightDir, out float pdf)
{
  // Explicit direct lighting estimates only the broad cosine-proposal BSDF group. Glossy / transmission groups are owned by continuation rays.
  const DisneyMaterialData material = GetDisneyMaterialData(surface);
  const DisneyBrdfData     brdf     = PrepareBrdfData(surface, material, viewDir, lightDir);
  const DisneyLobeSetup    setup    = GetLobeSetup(material, brdf);

  float groupPdf = 0.0;
  const float3 result = EvaluateBroadBsdfGroup(material, brdf, setup, groupPdf);
  // The direct-light estimator samples the whole broad group as one proposal,
  // so the reported PDF must include the probability of choosing that group.
  pdf                 = groupPdf * BroadGroupProbability(setup);
  return result;
}

// -------------------------------------------------------------------------
//    Lobe sampling
// -------------------------------------------------------------------------

bool SampleSurfaceBsdf(SurfaceData surface,
                       float3 viewDir,
                       inout uint seed,
                       out float3 lightDir,
                       out float3 bsdfOverPdf,
                       out float directLightPdf,
                       out bool isTransmissionEvent,
                       out uint sampledLobeKind)
{
  // Samples proposal groups explicitly: broad cosine lobes, glossy reflection,
  // glass, and clearcoat. The return values are intentionally split:
  // - bsdfOverPdf: throughput for the sampled continuation event
  // - directLightPdf: only the PDF of the subset that direct-light sampling can
  //   compete with later in miss/emissive-hit MIS
  const DisneyMaterialData material  = GetDisneyMaterialData(surface);
  const DisneyBrdfData     probeBrdf = PrepareBrdfData(surface, material, viewDir, surface.shadingNormal);
  const DisneyLobeSetup    setup     = GetLobeSetup(material, probeBrdf);
  lightDir                         = float3(0.0);
  bsdfOverPdf                      = float3(0.0);
  isTransmissionEvent               = false;
  directLightPdf                   = 0.0;
  sampledLobeKind                  = 0u;

  const float broadProbability      = BroadGroupProbability(setup);
  const float reflectionProbability = ReflectionGroupProbability(setup);
  const float glassProbability      = setup.glassProbability;
  const float clearcoatProbability  = setup.clearcoatProbability;
  const float totalProbability =
      broadProbability + reflectionProbability + glassProbability + clearcoatProbability;
  if(totalProbability <= 0.0)
  {
    return false;
  }

  const float  sampleGroup   = NextRandom(seed);
  const float2 xi            = float2(NextRandom(seed), NextRandom(seed));
  const float  cdfBroad      = broadProbability;
  const float  cdfReflection = cdfBroad + reflectionProbability;
  const float  cdfGlass      = cdfReflection + glassProbability;
  const bool   sampleBroad   = sampleGroup < cdfBroad;
  const bool   sampleReflection = sampleGroup >= cdfBroad && sampleGroup < cdfReflection;
  const bool   sampleGlass      = sampleGroup >= cdfReflection && sampleGroup < cdfGlass;

  if(sampleBroad)
  {
    sampledLobeKind = 0u;
    // One cosine sample stands in for both diffuse and sheen. The BSDF
    // evaluation later resolves how much each sub-lobe contributes.
    float ignoredPdf = 0.0;
    lightDir         = SampleCosineHemisphereDirection(surface.shadingNormal, seed, ignoredPdf);
  }
  else if(sampleReflection || sampleGlass)
  {
    sampledLobeKind = sampleReflection ? 1u : 2u;
    // Glossy reflection and glass start from the same visible-GGX microfacet
    // sample. Glass then does a second Fresnel branch to decide reflection vs
    // refraction for that sampled microfacet.
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

  // Rebuild the BRDF data at the sampled direction so the sampled event and the
  // evaluated contribution are driven by the same geometry/Fresnel terms.
  const DisneyBrdfData sampleBrdf = PrepareBrdfData(surface, material, viewDir, lightDir);
  float                conditionalPdf = 0.0;
  float3               contribution    = float3(0.0);
  float                samplePdf       = 0.0;

  if(sampleBroad)
  {
    contribution  = EvaluateBroadBsdfGroup(material, sampleBrdf, setup, conditionalPdf);
    samplePdf     = broadProbability * conditionalPdf;
    // Only broad-group samples can be compared against explicit direct-light
    // sampling, so only this branch exports a direct-light-compatible PDF.
    directLightPdf = samplePdf;
  }
  else if(sampleReflection)
  {
    contribution = EvaluateReflectionBsdfGroup(material, sampleBrdf, setup, conditionalPdf);
    samplePdf    = reflectionProbability * conditionalPdf;
  }
  else if(sampleGlass)
  {
    contribution       = EvaluateGlassBsdfGroup(material, sampleBrdf, setup, conditionalPdf);
    samplePdf          = glassProbability * conditionalPdf;
    isTransmissionEvent = !sampleBrdf.reflection;
  }
  else
  {
    contribution = EvaluateClearcoatBsdfGroup(material, sampleBrdf, setup, conditionalPdf);
    samplePdf    = clearcoatProbability * conditionalPdf;
  }

  if(samplePdf <= 0.0 || SafeMax3(contribution) <= 0.0)
  {
    isTransmissionEvent = false;
    directLightPdf      = 0.0;
    return false;
  }

  // Continuation throughput always uses the actual sampled proposal PDF, even
  // though miss/emissive MIS may later use a narrower directLightPdf contract.
  bsdfOverPdf = contribution / samplePdf;
  return true;
}

#endif
