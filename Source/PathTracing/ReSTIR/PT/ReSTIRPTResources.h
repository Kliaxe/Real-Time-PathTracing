#pragma once

#include <array>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "ReSTIR/PTParameters.h"
#include "Shaders/ShaderIo.h"
#include "nvvk/resource_allocator.hpp"
#include "nvvk/resources.hpp"

namespace nvapp
{
class Application;
}

namespace nvsamples
{

// Owns the ReSTIR PT buffers/images whose size depends on the viewport.
// This class does not decide what a pass does. It only guarantees that the GPU
// memory used by those passes exists and matches the current resolution.
//
// Compared with ReSTIRDIResources there is no neighbor-offset table: DI picks
// spatial neighbors from a fixed low-discrepancy offset list, whereas PT's paired
// spatial reuse derives neighbors from pairing textures instead. Those arrive with
// the spatial pass and are not allocated here yet.
class ReSTIRPTResources
{
public:
  struct CreateInfo
  {
    nvapp::Application*      app       = nullptr;
    nvvk::ResourceAllocator* allocator = nullptr;
  };

  explicit ReSTIRPTResources(const CreateInfo& createInfo);

  void Destroy();
  // Creates missing resources and recreates viewport-sized resources when resolution changes.
void EnsureForViewport(VkExtent2D viewportSize);

  VkExtent2D          GetViewportSize() const;
  // All rotating reservoir arrays live inside this one packed buffer.
  const nvvk::Buffer& GetPathReservoirBuffer() const;
  // Two surface buffers ping-pong between current and previous frame history.
  const nvvk::Buffer& GetSurfaceBuffer(uint32_t historyIndex) const;
  // Section 5 duplication scores, one float per pixel in row-major order. Written
  // at the end of a frame and read by the next frame's temporal pass, so it is
  // history and must be cleared whenever the rest of the history is.
  const nvvk::Buffer& GetDuplicationBuffer() const;
  // Section 3 pairing textures, concatenated. Resolution independent - they tile
  // across the screen - so this is built once per sigma rather than per viewport.
  const nvvk::Buffer& GetPairingBuffer() const;
  // Section 3 shared shift records, one per pixel per pairing slot.
  const nvvk::Buffer& GetPairedShiftBuffer() const;
  // Section 6.3 vector-valued shading weights, one RGB value per pixel.
  const nvvk::Buffer& GetShadingWeightBuffer() const;
  // Section 6.4 per-pixel motion vectors, carried one frame forward.
  const nvvk::Buffer& GetMotionVectorBuffer() const;
  // Per-pixel NRD guides from initial sampling: two hit distances and a specular
  // energy share.
  const nvvk::Buffer& GetDenoiserGuideBuffer() const;
  const nvvk::Buffer& GetLightTileBuffer() const;
  // Section 6.2.2 sorting. The counter buffer doubles as the indirect trace
  // arguments, so it carries INDIRECT_BUFFER usage and a device address.
  const nvvk::Buffer& GetPrepassWorkBuffer() const;
  const nvvk::Buffer& GetPrepassCounterBuffer() const;

  // Rebuilds the pairing textures for a new delta standard deviation. Returns
  // false if generation failed, in which case paired reuse must stay off: the
  // descriptors record a size of zero and every lookup declines.
  bool EnsurePairingTextures(float sigma);
  // Per-texture descriptors, with size 0 marking an unusable slot.
  const std::array<ReSTIRPTPairingTextureParameters, RESTIR_PT_MAX_PAIRING_TEXTURES>& GetPairingTextureParameters() const;
  const nvvk::Image&  GetAccumulationImage() const;

  const ReSTIRPTReservoirBufferParameters& GetReservoirBufferParameters() const;
  // Total bytes of reservoir storage, reported in the UI because the 64-byte
  // reservoir times two live sets dominates this renderer's memory budget.
  VkDeviceSize GetPathReservoirBufferSize() const;


private:
  // Viewport resources are replaced instead of resized in place.
  void CreateOrResizeViewportResources(VkExtent2D viewportSize);
  // Old resources may still be referenced by submitted frames, so defer destruction.
  void ScheduleBufferDestroy(nvvk::Buffer buffer);
  void ScheduleImageDestroy(nvvk::Image image);
  nvvk::Buffer CreateStorageBuffer(VkDeviceSize size, const char* debugName) const;

  nvapp::Application*         m_App       = nullptr;
  nvvk::ResourceAllocator*    m_Allocator = nullptr;
  VkExtent2D                  m_ViewportSize{};
  // Indexed by ReSTIRPTFrameContext current/previous history index.
  std::array<nvvk::Buffer, 2> m_SurfaceBuffers{};
  nvvk::Buffer                m_PathReservoirBuffer;
  nvvk::Buffer                m_DuplicationBuffer;
  // Two fixed-size pairing buffers, alternated on regeneration. The size is a
  // compile-time constant, so the allocation never has to change - only its
  // contents - and alternating means a frame still in flight keeps reading the
  // buffer it was bound to instead of one being rewritten under it.
  std::array<nvvk::Buffer, 2> m_PairingBuffers{};
  uint32_t                    m_PairingBufferIndex = 0;
  nvvk::Buffer                m_PairedShiftBuffer;
  nvvk::Buffer                m_ShadingWeightBuffer;
  nvvk::Buffer                m_MotionVectorBuffer;
  nvvk::Buffer                m_DenoiserGuideBuffer;
  // Section 6.1 light tiles. Sized from compile-time constants rather than the
  // viewport, so unlike every buffer around it this one survives a resize.
  nvvk::Buffer                m_LightTileBuffer;
  // One entry per (pixel, pairing slot), sized for the worst case where every
  // pair survives, because the list length is only known on the GPU.
  nvvk::Buffer                m_PrepassWorkBuffer;
  nvvk::Buffer                m_PrepassCounterBuffer;
  // Sigma the current pairing textures were built for; regeneration is skipped
  // while it is unchanged, since building them walks every texel many times.
  float                       m_PairingSigma = 0.0f;
  std::array<ReSTIRPTPairingTextureParameters, RESTIR_PT_MAX_PAIRING_TEXTURES> m_PairingTextureParameters{};
  nvvk::Image                 m_AccumulationImage;
  VkDeviceSize                m_PathReservoirBufferSize = 0;

  ReSTIRPTReservoirBufferParameters m_ReservoirBufferParameters{};
};

}  // namespace nvsamples
