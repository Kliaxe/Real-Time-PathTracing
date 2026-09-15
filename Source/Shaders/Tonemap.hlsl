// TonemapperSettings
// Push-constant block; mirrors rtpt::TonemapperSettings, whose static_asserts pin the 72-byte scalar layout.

struct TonemapperSettings
{
  // Nonzero applies the grading chain; zero copies the input through unchanged.
  int      active;

  // UI value only. Already folded into inputMatrix on the CPU.
  float    exposure;

  // UI value only, in kelvin. Already folded into inputMatrix on the CPU.
  float    temperature;

  // UI value only. Already folded into inputMatrix on the CPU.
  float    tint;

  // Exposure and white-balance gains, rebuilt by Tonemapper::Run before every dispatch.
  float3x3 inputMatrix;

  // Scales distance from mid grey after the filmic curve.
  float    contrast;

  // Applied as a 1 / brightness power.
  float    brightness;

  // Blend between luminance (0) and the original colour (1); values above 1 oversaturate.
  float    saturation;

  // Strength of the radial darkening.
  float    vignette;

  // Nonzero adds dither noise before 8-bit quantization.
  int      dither;
};

[[vk::push_constant]] ConstantBuffer<TonemapperSettings> settings;
[[vk::binding(0, 0)]] Texture2D<float4> inputImage;
[[vk::binding(1, 0), vk::image_format("rgba8")]] RWTexture2D<float4> outputImage;

float Luminance(float3 color)
{
  // Rec. 709 luma weights.
  return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 RtptFilmic(float3 color)
{
  // Filmic curve
  // A power-shaped response gives the toe enough separation without delaying the shoulder.
  // These parameters keep middle gray and practical highlights close to the established project look while remaining an RTPT-owned curve.

  const float blackOffset = 0.0035;
  const float curvePower = 0.925;
  const float shoulder = 0.192;
  const float3 positive = max(color - blackOffset.xxx, 0.0.xxx);
  const float3 shaped = pow(positive, curvePower.xxx);

  return shaped / (shaped + shoulder.xxx);
}

float3 AdjustSaturation(float3 color, float saturation)
{
  return lerp(Luminance(color).xxx, color, saturation.xxx);
}

uint ScramblePixel(uint2 pixel, uint stream)
{
  // Integer hash
  // Combines pixel and stream into one word, then mixes it with xor-shift and multiply rounds so neighbouring pixels decorrelate.

  uint bits = pixel.x ^ (pixel.y * 0xB6E1B209u) ^ ((stream + 1u) * 0xCF2D65F7u);

  bits ^= bits >> 16u;
  bits *= 0xEAFE9E09u;
  bits ^= bits >> 15u;
  bits *= 0x2F5EFFA1u;
  bits ^= bits >> 16u;

  return bits;
}

float UnitNoise(uint2 pixel, uint stream)
{
  // The top 24 bits are exactly representable in a float, giving a uniform value in [0, 1).
  return float(ScramblePixel(pixel, stream) >> 8u) * (1.0 / 16777216.0);
}

float3 ApplyDither(float3 color, uint2 pixel)
{
  // Noise shape
  // Two independent streams give triangular noise over (-1, 1) in the interior of the range.
  // Within half a quantization step of 0 or 1, the noise switches to a single uniform value over [-0.5, 0.5).

  const float quantizationSteps = 255.0;
  const float first = UnitNoise(pixel, 0u);
  const float second = UnitNoise(pixel, 1u);
  const float triangular = first - second;
  const float boxNoise = first - 0.5;
  const float halfStep = 0.5 / quantizationSteps;
  const float3 edgeDistance = min(color, 1.0.xxx - color);
  const float3 edgeWeight = 1.0.xxx - step(halfStep.xxx, edgeDistance);
  const float3 noise = lerp(triangular.xxx, boxNoise.xxx, edgeWeight);

  // Noise is scaled to one 8-bit code value.
  return color + noise / quantizationSteps;
}

// 16x16 matches the group size Tonemapper::Run divides the extent by.
[numthreads(16, 16, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
  uint width;
  uint height;

  outputImage.GetDimensions(width, height);

  // The dispatch rounds up to whole groups, so edge invocations can fall outside the image.
  if(pixel.x >= width || pixel.y >= height)
  {
    return;
  }

  float4 color = inputImage.Load(int3(pixel, 0));

  // Grading chain
  // Order matters: colour correction and the filmic curve operate on linear HDR, and contrast, brightness, and saturation then operate on the curve's [0, 1] output.
  // Vignette and dither come last so they act on the final display values.

  if(settings.active != 0)
  {
    color.rgb = mul(settings.inputMatrix, color.rgb);
    color.rgb = RtptFilmic(color.rgb);
    color.rgb = clamp(lerp(0.5.xxx, color.rgb, settings.contrast.xxx), 0.0.xxx, 1.0.xxx);
    color.rgb = pow(color.rgb, (1.0 / settings.brightness).xxx);
    color.rgb = AdjustSaturation(color.rgb, settings.saturation);

    const float2 centeredUv = ((float2)pixel / float2(width, height)) * 2.0 - 1.0;

    color.rgb *= 1.0 - dot(centeredUv, centeredUv) * settings.vignette;

    if(settings.dither != 0)
    {
      color.rgb = ApplyDither(color.rgb, pixel);
    }
  }

  outputImage[pixel] = color;
}
