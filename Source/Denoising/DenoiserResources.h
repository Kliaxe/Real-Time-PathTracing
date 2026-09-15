#pragma once

#include <vulkan/vulkan_core.h>

#include "Framework/Vulkan/Diagnostics.h"
#include "Framework/Vulkan/GpuResources.h"

namespace rtpt
{

// DenoiserResources
// Owns the NRD input images shared by the ray-traced renderers: primary-hit guide buffers plus the split diffuse/specular noisy signals.
// Each renderer writes these from its own shaders and hands them to NrdDenoiser, so the images live outside the denoiser and follow the renderer's viewport.

class DenoiserResources
{
public:

  // CreateInfo
  // Dependencies supplied by the owning renderer. Only the allocator is required; diagnostics just names the images.

  struct CreateInfo
  {
    // Allocates every image. The constructor throws if this is null.
    rtpt::ResourceAllocator* resources = nullptr;

    // Optional; used only to attach debug names.
    const rtpt::Diagnostics* diagnostics = nullptr;
  };

  explicit DenoiserResources(const CreateInfo& createInfo);

  void Destroy();

  void EnsureForViewport(VkExtent2D viewportSize);

  VkExtent2D GetViewportSize() const;

  const rtpt::Image& GetMotionVectorsImage() const;
  rtpt::Image&       GetMotionVectorsImage();

  const rtpt::Image& GetNormalRoughnessImage() const;
  rtpt::Image&       GetNormalRoughnessImage();

  const rtpt::Image& GetBaseColorMetalnessImage() const;
  rtpt::Image&       GetBaseColorMetalnessImage();

  const rtpt::Image& GetViewZImage() const;
  rtpt::Image&       GetViewZImage();

  const rtpt::Image& GetDiffuseRadianceHitDistanceImage() const;
  rtpt::Image&       GetDiffuseRadianceHitDistanceImage();

  const rtpt::Image& GetSpecularRadianceHitDistanceImage() const;
  rtpt::Image&       GetSpecularRadianceHitDistanceImage();

  const rtpt::Image& GetSpecularDemodulationFactorImage() const;
  rtpt::Image&       GetSpecularDemodulationFactorImage();

private:

  void CreateOrResizeViewportResources(VkExtent2D viewportSize);

  void DestroyViewportResources();

  rtpt::Image CreateStorageImage(VkExtent2D viewportSize, VkFormat format, const char* debugName) const;

  // Allocates every image owned here.
  rtpt::ResourceAllocator* m_Resources = nullptr;

  // Optional debug-name sink; may be null.
  const rtpt::Diagnostics* m_Diagnostics = nullptr;

  // Resolution the images were allocated at. Zero until the first non-empty viewport.
  VkExtent2D               m_ViewportSize {};

  // IN_MV: screen-UV motion in xy and previous-minus-current viewZ in z.
  rtpt::Image              m_MotionVectorsImage;

  // IN_NORMAL_ROUGHNESS, packed with NRD_FrontEnd_PackNormalAndRoughness.
  rtpt::Image              m_NormalRoughnessImage;

  // Primary-hit base colour and metalness. NRD 4.17 no longer reads it; the compose pass uses it to rebuild the diffuse demodulation factor.
  rtpt::Image              m_BaseColorMetalnessImage;

  // IN_VIEWZ: linear view depth of the primary hit, or a huge sentinel where the primary ray missed.
  rtpt::Image              m_ViewZImage;

  // IN_DIFF_RADIANCE_HITDIST: demodulated diffuse radiance with normalized hit distance.
  rtpt::Image              m_DiffuseRadianceHitDistanceImage;

  // IN_SPEC_RADIANCE_HITDIST: demodulated specular radiance with normalized hit distance.
  rtpt::Image              m_SpecularRadianceHitDistanceImage;

  // Per-pixel specular factor the shaders divided out, kept so the compose pass can multiply it back in.
  rtpt::Image              m_SpecularDemodulationFactorImage;
};

}  // namespace rtpt
