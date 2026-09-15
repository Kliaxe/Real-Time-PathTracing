#pragma once

#include <array>
#include <vector>

#include <volk.h>

#include "ReSTIR/PTParameters.h"
#include "Framework/Vulkan/Diagnostics.h"
#include "Framework/Vulkan/GpuResources.h"
#include "Shaders/ShaderIo.h"

namespace rtpt
{

// ReSTIRPTResources
// Owns the ReSTIR PT buffers/images whose size depends on the viewport, plus the few fixed-size buffers the same descriptor set binds.
// This class does not decide what a pass does. It only guarantees that the GPU memory used by those passes exists and matches the current resolution.
// There is no neighbour-offset table: Section 3's paired spatial reuse derives neighbours from the pairing textures, which are allocated here instead.

class ReSTIRPTResources
{
public:

  // CreateInfo
  // Lifetime dependencies borrowed from the renderer.

  struct CreateInfo
  {
    // Allocates every buffer and image this class owns.
    rtpt::ResourceAllocator* resources = nullptr;
    // Optional. Names buffers for debugging tools when present.
    const rtpt::Diagnostics* diagnostics = nullptr;
  };

  explicit ReSTIRPTResources(const CreateInfo& createInfo);

  void Destroy();

  // Creates missing resources and recreates viewport-sized resources when resolution changes.
  void EnsureForViewport(VkExtent2D viewportSize);

  VkExtent2D          GetViewportSize() const;

  // All rotating reservoir arrays live inside this one packed buffer.
  const rtpt::Buffer& GetPathReservoirBuffer() const;

  // Two surface buffers ping-pong between current and previous frame history.
  const rtpt::Buffer& GetSurfaceBuffer(uint32_t historyIndex) const;

  // Section 5 duplication scores, one float per pixel in row-major order.
  // Written at the end of a frame and read by the next frame's temporal pass, so it is history and must be cleared whenever the rest of the history is.
  const rtpt::Buffer& GetDuplicationBuffer() const;

  // Section 3 pairing textures, concatenated. Resolution independent - they tile across the screen - so this is built once per sigma rather than per viewport.
  const rtpt::Buffer& GetPairingBuffer() const;

  // Section 3 shared shift records, one per pixel per pairing slot.
  const rtpt::Buffer& GetPairedShiftBuffer() const;

  // Section 6.3 vector-valued shading weights, one RGB value per pixel.
  const rtpt::Buffer& GetShadingWeightBuffer() const;

  // Section 6.4 per-pixel motion vectors, carried one frame forward.
  const rtpt::Buffer& GetMotionVectorBuffer() const;

  // Per-pixel NRD guides from initial sampling: two hit distances and a specular energy share.
  const rtpt::Buffer& GetDenoiserGuideBuffer() const;

  // Section 6.1 presampled light tiles.
  const rtpt::Buffer& GetLightTileBuffer() const;

  // Section 6.2.2 sorting. The counter buffer doubles as the indirect trace arguments, so it carries INDIRECT_BUFFER usage and a device address.
  const rtpt::Buffer& GetPrepassWorkBuffer() const;
  const rtpt::Buffer& GetPrepassCounterBuffer() const;

  // Rebuilds the pairing textures for a new delta standard deviation.
  // Returns false if generation failed, in which case paired reuse must stay off: the descriptors record a size of zero and every lookup declines.
  bool EnsurePairingTextures(float sigma);

  // Per-texture descriptors, with size 0 marking an unusable slot.
  const std::array<ReSTIRPTPairingTextureParameters, RESTIR_PT_MAX_PAIRING_TEXTURES>& GetPairingTextureParameters() const;

  const rtpt::Image&  GetAccumulationImage() const;

  const ReSTIRPTReservoirBufferParameters& GetReservoirBufferParameters() const;

  // Total bytes of reservoir storage, reported in the UI because the 64-byte reservoir times two live sets dominates this renderer's memory budget.
  VkDeviceSize GetPathReservoirBufferSize() const;

private:

  // Viewport resources are replaced instead of resized in place.
  void CreateOrResizeViewportResources(VkExtent2D viewportSize);

  rtpt::Buffer CreateStorageBuffer(VkDeviceSize size, const char* debugName) const;

  // Allocates and releases every resource below.
  rtpt::ResourceAllocator*    m_Resources = nullptr;
  // Optional debug naming; null disables it.
  const rtpt::Diagnostics*    m_Diagnostics = nullptr;
  // Resolution the viewport-sized resources were allocated for. Zero until the first frame.
  VkExtent2D                  m_ViewportSize {};
  // Indexed by ReSTIRFrameContext current/previous history index.
  std::array<rtpt::Buffer, 2> m_SurfaceBuffers {};
  // Every rotating reservoir array, packed end to end at reservoirArrayPitch.
  rtpt::Buffer                m_PathReservoirBuffer;
  // Section 5 duplication scores; history, cleared with the reservoirs.
  rtpt::Buffer                m_DuplicationBuffer;
  // Two fixed-size pairing buffers, alternated on regeneration.
  // The size is a compile-time constant, so the allocation never has to change - only its contents - and alternating means a frame still in flight keeps reading the buffer it was bound to instead of one being rewritten under it.
  std::array<rtpt::Buffer, 2> m_PairingBuffers {};
  // Which of m_PairingBuffers currently holds the published textures.
  uint32_t                    m_PairingBufferIndex = 0;
  // Section 3 shared shift records, sized for the maximum pairing slot count.
  rtpt::Buffer                m_PairedShiftBuffer;
  // Section 6.3 summed RGB resampling weights, one per pixel.
  rtpt::Buffer                m_ShadingWeightBuffer;
  // Section 6.4 motion vectors, two floats per pixel.
  rtpt::Buffer                m_MotionVectorBuffer;
  // NRD guide values from initial sampling, three floats per pixel.
  rtpt::Buffer                m_DenoiserGuideBuffer;
  // Section 6.1 light tiles. Sized from compile-time constants rather than the viewport, so unlike every buffer around it this one survives a resize.
  rtpt::Buffer                m_LightTileBuffer;
  // One entry per (pixel, pairing slot), sized for the worst case where every pair survives, because the list length is only known on the GPU.
  rtpt::Buffer                m_PrepassWorkBuffer;
  // Running append count followed by the three VkTraceRaysIndirectCommandKHR dimensions. Fixed size, so it survives a resize.
  rtpt::Buffer                m_PrepassCounterBuffer;
  // Sigma the current pairing textures were built for; regeneration is skipped while it is unchanged, since building them walks every texel many times.
  float                       m_PairingSigma = 0.0f;
  // Per-slot descriptors for the published pairing textures. Size 0 marks a slot unusable.
  std::array<ReSTIRPTPairingTextureParameters, RESTIR_PT_MAX_PAIRING_TEXTURES> m_PairingTextureParameters {};
  // HDR radiance before tonemapping or denoising, in RGBA32F.
  rtpt::Image                 m_AccumulationImage;
  // Bytes allocated for m_PathReservoirBuffer, cached for the UI.
  VkDeviceSize                m_PathReservoirBufferSize = 0;

  // Addressing pitches the reservoir buffer was sized with. The shaders use the same layout, so the two must agree or passes silently read the wrong pixels.
  ReSTIRPTReservoirBufferParameters m_ReservoirBufferParameters {};
};

}  // namespace rtpt
