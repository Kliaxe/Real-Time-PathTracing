#ifndef RTPT_DENOISER_INPUTS_HLSLI
#define RTPT_DENOISER_INPUTS_HLSLI

// Denoiser inputs
// Conditioning shared by every renderer that writes NRD's noisy signals, so the path tracer and ReSTIR PT hand the denoiser identically prepared radiance.

// Scales radiance down so its luminance does not exceed clampLuminance, keeping its colour. Zero disables the clamp.
// This is RTXPT's NRDRadianceClamp: one firefly otherwise enters REBLUR's history at full strength and is smeared across the blur radius for the whole history length. The clamp is applied to the demodulated signal, which is what NRD accumulates.
float3 ClampDenoiserRadiance(float3 radiance, float clampLuminance)
{
  const float luminance = dot(radiance, float3(0.2126, 0.7152, 0.0722));

  if(clampLuminance <= 0.0 || luminance <= clampLuminance)
  {
    return radiance;
  }

  return radiance * (clampLuminance / luminance);
}

#endif
