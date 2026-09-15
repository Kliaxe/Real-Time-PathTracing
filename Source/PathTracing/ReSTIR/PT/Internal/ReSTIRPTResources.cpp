#include <cassert>
#include "PathTracing/ReSTIR/PT/ReSTIRPTResources.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "PathTracing/ReSTIR/PT/ReSTIRPTParameterContext.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTPairingTexture.h"
#include "Framework/Vulkan/Diagnostics.h"
#include "Framework/Platform/Log.h"

#include <cstring>
#include <fmt/format.h>

namespace rtpt
{

namespace
{

// RGBA32F keeps HDR radiance before tonemapping and denoising.
constexpr VkFormat kAccumulationFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

// Section 3.2 recommends different edge lengths per neighbour slot so the tilings never align; these are the paper's values.
// They are also even, which the construction requires in order to hand out link indices in pairs.
constexpr uint32_t kPairingTextureSizes[RESTIR_PT_MAX_PAIRING_TEXTURES] = { 254, 230, 210 };

// CalculatePairingBufferSize
// Every pairing texture concatenated, one packed uint per texel. Fixed because the sizes above are.

VkDeviceSize CalculatePairingBufferSize()
{
  VkDeviceSize texels = 0;

  for(uint32_t size : kPairingTextureSizes)
  {
    texels += VkDeviceSize(size) * VkDeviceSize(size);
  }

  return texels * sizeof(uint32_t);
}

}  // namespace

ReSTIRPTResources::ReSTIRPTResources(const CreateInfo& createInfo)
    : m_Resources(createInfo.resources)
    , m_Diagnostics(createInfo.diagnostics)
{
}

void ReSTIRPTResources::Destroy()
{
  // Release
  // Destroy runs during full renderer shutdown, where no submitted work should still reference these resources.

  for(rtpt::Buffer& surfaceBuffer : m_SurfaceBuffers)
  {
    surfaceBuffer.Reset();
  }

  m_PathReservoirBuffer.Reset();
  m_DuplicationBuffer.Reset();

  for(rtpt::Buffer& pairingBuffer : m_PairingBuffers)
  {
    pairingBuffer.Reset();
  }

  m_PairingBufferIndex = 0;
  m_PairedShiftBuffer.Reset();
  m_ShadingWeightBuffer.Reset();
  m_MotionVectorBuffer.Reset();
  m_DenoiserGuideBuffer.Reset();
  m_LightTileBuffer.Reset();
  m_PrepassWorkBuffer.Reset();
  m_PrepassCounterBuffer.Reset();
  m_PairingSigma             = 0.0f;
  m_PairingTextureParameters = {};
  m_AccumulationImage.Reset();

  // Cached state
  // Clearing the viewport size forces the next EnsureForViewport to allocate from scratch.

  m_ViewportSize              = {};
  m_PathReservoirBufferSize   = 0;
  m_ReservoirBufferParameters = {};
}

void ReSTIRPTResources::EnsureForViewport(VkExtent2D viewportSize)
{
  CreateOrResizeViewportResources(viewportSize);
}

VkExtent2D ReSTIRPTResources::GetViewportSize() const
{
  return m_ViewportSize;
}

const rtpt::Buffer& ReSTIRPTResources::GetPathReservoirBuffer() const
{
  return m_PathReservoirBuffer;
}

const rtpt::Buffer& ReSTIRPTResources::GetSurfaceBuffer(uint32_t historyIndex) const
{
  // Current/previous surfaces are ping-ponged by frame parity.
  return m_SurfaceBuffers[historyIndex & 1u];
}

const rtpt::Buffer& ReSTIRPTResources::GetDuplicationBuffer() const
{
  return m_DuplicationBuffer;
}

const rtpt::Buffer& ReSTIRPTResources::GetPairingBuffer() const
{
  return m_PairingBuffers[m_PairingBufferIndex];
}

const rtpt::Buffer& ReSTIRPTResources::GetPairedShiftBuffer() const
{
  return m_PairedShiftBuffer;
}

const rtpt::Buffer& ReSTIRPTResources::GetShadingWeightBuffer() const
{
  return m_ShadingWeightBuffer;
}

const rtpt::Buffer& ReSTIRPTResources::GetMotionVectorBuffer() const
{
  return m_MotionVectorBuffer;
}

const rtpt::Buffer& ReSTIRPTResources::GetDenoiserGuideBuffer() const
{
  return m_DenoiserGuideBuffer;
}

const rtpt::Buffer& ReSTIRPTResources::GetLightTileBuffer() const
{
  return m_LightTileBuffer;
}

const rtpt::Buffer& ReSTIRPTResources::GetPrepassWorkBuffer() const
{
  return m_PrepassWorkBuffer;
}

const rtpt::Buffer& ReSTIRPTResources::GetPrepassCounterBuffer() const
{
  return m_PrepassCounterBuffer;
}

const std::array<ReSTIRPTPairingTextureParameters, RESTIR_PT_MAX_PAIRING_TEXTURES>& ReSTIRPTResources::GetPairingTextureParameters() const
{
  return m_PairingTextureParameters;
}

bool ReSTIRPTResources::EnsurePairingTextures(float sigma)
{
  // Generation is expensive enough to be worth skipping, and sigma only moves when the user drags the spatial radius.
  if(m_PairingSigma != 0.0f && std::abs(sigma - m_PairingSigma) < 1.0e-4f)
  {
    return m_PairingTextureParameters[0].size != 0;
  }

  // The pairing buffers are allocated with the first viewport; before that there is nowhere to upload.
  if(m_PairingBuffers[0].buffer == VK_NULL_HANDLE)
  {
    return false;
  }

  // Generation
  // Each slot's texture is generated on the CPU, validated, and packed into one concatenated array that matches the buffer layout.

  std::array<ReSTIRPTPairingTexture, RESTIR_PT_MAX_PAIRING_TEXTURES>           textures {};
  std::vector<uint32_t>                                                        packed;
  std::array<ReSTIRPTPairingTextureParameters, RESTIR_PT_MAX_PAIRING_TEXTURES> descriptors {};

  for(uint32_t slot = 0; slot < RESTIR_PT_MAX_PAIRING_TEXTURES; ++slot)
  {
    // Each texture gets its own seed so the three are independent; two slots sharing a permutation would pair pixels identically and defeat the point of having several.
    textures[slot] = GenerateReSTIRPTPairingTexture(kPairingTextureSizes[slot], sigma, 0x9E3779B9u * (slot + 1u));

    // Reciprocity is the one property the shared-shift optimisation cannot do without, so a texture that fails the check disables paired reuse outright rather than being used and quietly double-counting MIS weights.
    if(textures[slot].deltas.empty() || !textures[slot].involutionValid)
    {
      rtpt::Log(rtpt::LogLevel::Warning, fmt::format("ReSTIR PT pairing texture {} (size {}, sigma {:.3f}) failed to generate; paired spatial reuse disabled", slot, kPairingTextureSizes[slot], sigma));

      m_PairingSigma             = sigma;
      m_PairingTextureParameters = {};

      return false;
    }

    descriptors[slot].size         = textures[slot].size;
    descriptors[slot].bufferOffset = uint32_t(packed.size());
    descriptors[slot].transform    = 0;
    descriptors[slot].translation  = 0;

    const uint32_t texelCount = textures[slot].size * textures[slot].size;

    packed.reserve(packed.size() + texelCount);

    for(uint32_t texel = 0; texel < texelCount; ++texel)
    {
      // Bias by 128 so the signed deltas survive as unsigned bytes in one word.
      const uint32_t x = uint32_t(int32_t(textures[slot].deltas[size_t(texel) * 2 + 0]) + 128);
      const uint32_t y = uint32_t(int32_t(textures[slot].deltas[size_t(texel) * 2 + 1]) + 128);

      packed.push_back((y << 8) | x);
    }
  }

  // Publish
  // Fill the buffer this frame is NOT bound to, then publish it. A frame still in flight keeps reading the one it was given.

  const uint32_t writeIndex = 1u - m_PairingBufferIndex;

  std::memcpy(m_PairingBuffers[writeIndex].mapping, packed.data(), packed.size() * sizeof(uint32_t));

  // VMA guarantees this allocation is host visible, not host coherent, so a mapped write is not automatically visible to the device. Flushing is a no-op where the memory happens to be coherent and the difference between correct and silently stale pairing data where it is not.
  rtpt::CheckVk(m_Resources->FlushBuffer(m_PairingBuffers[writeIndex]), "ResourceAllocator::FlushBuffer(ReSTIR pairing)");

  m_PairingBufferIndex = writeIndex;

  m_PairingSigma             = sigma;
  m_PairingTextureParameters = descriptors;

  return true;
}

const rtpt::Image& ReSTIRPTResources::GetAccumulationImage() const
{
  return m_AccumulationImage;
}

const ReSTIRPTReservoirBufferParameters& ReSTIRPTResources::GetReservoirBufferParameters() const
{
  return m_ReservoirBufferParameters;
}

VkDeviceSize ReSTIRPTResources::GetPathReservoirBufferSize() const
{
  return m_PathReservoirBufferSize;
}

void ReSTIRPTResources::CreateOrResizeViewportResources(VkExtent2D viewportSize)
{
  if(viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  // Existing buffers already match the current resolution.
  if(m_ViewportSize.width == viewportSize.width && m_ViewportSize.height == viewportSize.height && m_PathReservoirBuffer.buffer != VK_NULL_HANDLE)
  {
    return;
  }

  // Sizes
  // The shader uses the reservoir layout to address packed reservoir arrays; the two must agree or passes silently read the wrong pixels.

  m_ReservoirBufferParameters = CalculateReSTIRPTReservoirBufferParameters(viewportSize.width, viewportSize.height);

  // One buffer holds every rotating reservoir array.
  const VkDeviceSize reservoirElementCount = VkDeviceSize(m_ReservoirBufferParameters.reservoirArrayPitch) * VkDeviceSize(kReSTIRPTReservoirBufferCount);
  const VkDeviceSize reservoirBufferSize   = reservoirElementCount * sizeof(ReSTIRPTPackedReservoir);
  const VkDeviceSize pixelCount            = VkDeviceSize(viewportSize.width) * VkDeviceSize(viewportSize.height);
  const VkDeviceSize surfaceBufferSize     = pixelCount * sizeof(shaderio::ReSTIRPTSurface);

  // One float per pixel. Deliberately a plain screen-space array rather than the block-linear reservoir layout: its only consumer backprojects into it.
  const VkDeviceSize duplicationBufferSize = pixelCount * sizeof(float);

  // One shared shift record per pixel per pairing slot. Sized for the maximum slot count rather than the current neighbour count so the allocation does not churn when that setting changes.
  const VkDeviceSize pairedShiftBufferSize = pixelCount * VkDeviceSize(RESTIR_PT_MAX_PAIRING_TEXTURES) * sizeof(shaderio::ReSTIRPTPairedShift);

  // Release old viewport resources
  // Old viewport resources may still be referenced by submitted frames. Reset hands them to the allocator's retirement queue, which releases them only once that submitted work has completed.

  for(rtpt::Buffer& surfaceBuffer : m_SurfaceBuffers)
  {
    surfaceBuffer.Reset();
  }

  m_PathReservoirBuffer.Reset();
  m_DuplicationBuffer.Reset();
  m_PairedShiftBuffer.Reset();
  m_ShadingWeightBuffer.Reset();
  m_MotionVectorBuffer.Reset();
  m_DenoiserGuideBuffer.Reset();
  m_PrepassWorkBuffer.Reset();
  m_AccumulationImage.Reset();

  m_ViewportSize            = viewportSize;
  m_PathReservoirBufferSize = reservoirBufferSize;

  // Surface and reservoir history

  m_SurfaceBuffers[0] = CreateStorageBuffer(surfaceBufferSize, "ReSTIRPTSurfaceHistory0Buffer");
  m_SurfaceBuffers[1] = CreateStorageBuffer(surfaceBufferSize, "ReSTIRPTSurfaceHistory1Buffer");

  m_PathReservoirBuffer = CreateStorageBuffer(reservoirBufferSize, "ReSTIRPTPathReservoirBuffer");
  m_DuplicationBuffer   = CreateStorageBuffer(duplicationBufferSize, "ReSTIRPTDuplicationBuffer");

  // Pairing buffers
  // Allocated up front and never resized: the descriptor set is written every frame whether or not paired reuse is on, and a null buffer there is invalid without VK_EXT_robustness2's nullDescriptor.
  // Until generation fills them the pairing descriptors report size 0, so no shader reads the contents.

  if(m_PairingBuffers[0].buffer == VK_NULL_HANDLE)
  {
    for(size_t i = 0; i < m_PairingBuffers.size(); ++i)
    {
      rtpt::CheckVk(m_Resources->CreateBuffer(m_PairingBuffers[i], CalculatePairingBufferSize(), VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT), "ResourceAllocator::CreateBuffer(ReSTIR pairing)");
    }
  }

  // Per-pixel pass outputs

  m_PairedShiftBuffer   = CreateStorageBuffer(pairedShiftBufferSize, "ReSTIRPTPairedShiftBuffer");

  // Three floats per pixel under scalar layout, matching RWStructuredBuffer<float3>.
  m_ShadingWeightBuffer = CreateStorageBuffer(pixelCount * 3 * sizeof(float), "ReSTIRPTShadingWeightBuffer");
  m_MotionVectorBuffer  = CreateStorageBuffer(pixelCount * 2 * sizeof(float), "ReSTIRPTMotionVectorBuffer");

  // Three floats per pixel under scalar layout, matching RWStructuredBuffer<float3>. Allocated even when denoising is off: the descriptor set is written every frame regardless, and a null buffer there is invalid without VK_EXT_robustness2's nullDescriptor.
  m_DenoiserGuideBuffer = CreateStorageBuffer(pixelCount * 3 * sizeof(float), "ReSTIRPTDenoiserGuideBuffer");

  // Prepass work list
  // Section 6.2.2 stream compaction. One work-list slot per (pixel, pairing slot); the list is sized for the worst case where every pair survives, because its length is only known on the GPU.
  // PackPrepassWorkItem spends two bits on the slot, leaving 30 for the pixel index. Nothing else enforces that, and the failure past it would be aliased work items rather than an allocation error.

  assert(pixelCount < (VkDeviceSize(1) << 30) && "viewport exceeds the prepass work item packing range");

  const VkDeviceSize prepassPairCount = pixelCount * VkDeviceSize(RESTIR_PT_MAX_PAIRING_TEXTURES);

  m_PrepassWorkBuffer = CreateStorageBuffer(prepassPairCount * sizeof(uint32_t), "ReSTIRPTPrepassWorkBuffer");

  // Prepass counters
  // Allocated once: tiny, and its size does not depend on the viewport. It holds the work list count followed by the three VkTraceRaysIndirectCommandKHR dimensions - hence the indirect usage and the device address the indirect trace needs.

  if(m_PrepassCounterBuffer.buffer == VK_NULL_HANDLE)
  {
    rtpt::CheckVk(m_Resources->CreateBuffer(m_PrepassCounterBuffer, VkDeviceSize(RESTIR_PT_PREPASS_COUNTER_UINTS) * sizeof(uint32_t), VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT | VK_BUFFER_USAGE_2_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE), "ResourceAllocator::CreateBuffer(ReSTIR prepass counters)");
  }

  // Light tiles
  // Allocated once and never resized: its size comes from the tile constants, not the viewport, and the descriptor set is written every frame whether or not light tiles are enabled.

  if(m_LightTileBuffer.buffer == VK_NULL_HANDLE)
  {
    const VkDeviceSize lightTileBytes = VkDeviceSize(RESTIR_PT_LIGHT_TILE_COUNT) * VkDeviceSize(RESTIR_PT_LIGHT_TILE_SIZE) * sizeof(shaderio::ReSTIRPTLightTileSample);

    m_LightTileBuffer = CreateStorageBuffer(lightTileBytes, "ReSTIRPTLightTileBuffer");
  }

  // Accumulation image
  // Stores HDR radiance before tonemapping and denoising. The ReSTIR passes bind it as a storage image, and the denoiser is handed its view afterwards.

  VkImageCreateInfo imageInfo {
      .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType     = VK_IMAGE_TYPE_2D,
      .format        = kAccumulationFormat,
      .extent        = { .width = viewportSize.width, .height = viewportSize.height, .depth = 1 },
      .mipLevels     = 1,
      .arrayLayers   = 1,
      .samples       = VK_SAMPLE_COUNT_1_BIT,
      .tiling        = VK_IMAGE_TILING_OPTIMAL,
      .usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  VkImageViewCreateInfo viewInfo {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = imageInfo.format,
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
  };

  rtpt::CheckVk(m_Resources->CreateImage(m_AccumulationImage, imageInfo, &viewInfo), "ResourceAllocator::CreateImage(ReSTIR accumulation)");

  // The image starts UNDEFINED; the renderer transitions it before the first pass binds it as storage.
  m_AccumulationImage.descriptor.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  m_AccumulationImage.descriptor.sampler     = VK_NULL_HANDLE;
}

rtpt::Buffer ReSTIRPTResources::CreateStorageBuffer(VkDeviceSize size, const char* debugName) const
{
  rtpt::Buffer buffer;

  // Device-local storage buffers are written by GPU passes and cleared by transfer when ReSTIR history is invalidated.
  rtpt::CheckVk(m_Resources->CreateBuffer(buffer, size, VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE), "ResourceAllocator::CreateBuffer(ReSTIR storage)");

  if(m_Diagnostics != nullptr)
  {
    m_Diagnostics->SetObjectName(m_Resources->Device(), VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(buffer.buffer), debugName);
  }

  return buffer;
}

}  // namespace rtpt
