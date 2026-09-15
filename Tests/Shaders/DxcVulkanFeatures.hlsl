// DxcVulkanFeatures
// Compute shader that exercises DXC's Vulkan-specific HLSL features in one entry point: vk::BufferPointer, vk::RawBufferLoad, push constants, and unbounded descriptor arrays indexed with NonUniformResourceIndex.
// No build target or test currently references this file.

// ProbePayload
// Record read through a raw buffer device address.

struct ProbePayload
{
  // Value added to the sampled texel and written to the output.
  float4 value;
};

typedef vk::BufferPointer<ProbePayload> ProbePayloadPointer;

// Reads a T directly from a buffer device address.
template<typename T>
T LoadValue(uint64_t address)
{
  return vk::RawBufferLoad<T>(address);
}

// ProbePushConstants
// Push constant block locating the payload array.

struct ProbePushConstants
{
  // Buffer device address of the first ProbePayload. Element N lives at payloadAddress + N * sizeof(ProbePayload).
  uint64_t payloadAddress;
};

// Push constants carrying the payload address.
[[vk::push_constant]] ConstantBuffer<ProbePushConstants> pushConstants;

// Unbounded texture array; each invocation samples the texture matching its dispatch index.
[[vk::binding(0, 0)]] Texture2D<float4> textures[];

// Unbounded sampler array paired index-for-index with textures.
[[vk::binding(1, 0)]] SamplerState textureSamplers[];

// One result per invocation: payload value plus the sampled texel.
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> output;

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
  // Each invocation picks a different descriptor, so the index must be marked non-uniform.
  const uint textureIndex = NonUniformResourceIndex(dispatchThreadId.x);

  uint64_t payloadAddress = pushConstants.payloadAddress;

  payloadAddress += uint64_t(dispatchThreadId.x) * uint64_t(sizeof(ProbePayload));

  output[dispatchThreadId.x] = LoadValue<ProbePayload>(payloadAddress).value + textures[textureIndex].SampleLevel(textureSamplers[textureIndex], float2(0.5f, 0.5f), 0.0f);
}
