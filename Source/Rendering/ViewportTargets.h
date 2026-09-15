#pragma once

#include "Framework/Vulkan/GpuResources.h"
#include "RenderTargetView.h"

namespace rtpt
{

// ViewportTargets
// Owns the three images every frame renders through: HDR color, LDR display color, and depth.
// The active renderer writes linear radiance into HDR, the tonemapper writes LDR from it, and the UI shows LDR. They are always resized together, so they share one extent.

class ViewportTargets
{
public:

  // Number of images a successful Resize allocates. The framework tests use it to check resource accounting.
  static constexpr uint32_t kTargetCount = 3;

  ViewportTargets() = default;
  ViewportTargets(const ViewportTargets&)            = delete;
  ViewportTargets& operator=(const ViewportTargets&) = delete;
  ~ViewportTargets();

  // Binds the allocator and picks a depth format. Throws if already initialized or if the device supports no depth attachment format.
  void Initialize(ResourceAllocator& resources, VkPhysicalDevice physicalDevice);

  void Destroy();

  // Reallocates all targets at extent. Returns false for an empty or unchanged extent; if allocation throws, the previous targets stay in place.
  bool Resize(VkExtent2D extent);

  [[nodiscard]] RenderTargetView Hdr() const noexcept;
  [[nodiscard]] RenderTargetView Ldr() const noexcept;
  [[nodiscard]] RenderTargetView Depth() const noexcept;
  [[nodiscard]] VkExtent2D Extent() const noexcept { return m_Extent; }

  // Float RGBA keeps linear radiance unclamped for tonemapping and the .linear.hdr capture.
  [[nodiscard]] VkFormat HdrFormat() const noexcept { return VK_FORMAT_R32G32B32A32_SFLOAT; }

  // Matches the rgba8 storage format Tonemap.hlsl declares and the 8-bit PNG capture.
  [[nodiscard]] VkFormat LdrFormat() const noexcept { return VK_FORMAT_R8G8B8A8_UNORM; }

  [[nodiscard]] VkFormat DepthFormat() const noexcept { return m_DepthFormat; }

private:

  // First candidate format the device supports as a depth attachment, or VK_FORMAT_UNDEFINED.
  [[nodiscard]] VkFormat SelectDepthFormat() const;

  // Allocator that creates the images. Null means uninitialized.
  ResourceAllocator* m_Resources = nullptr;

  // Queried for depth format support.
  VkPhysicalDevice   m_PhysicalDevice = VK_NULL_HANDLE;

  // Current size of all three targets. Zero until the first successful Resize.
  VkExtent2D         m_Extent {};

  // Depth format chosen at initialization.
  VkFormat           m_DepthFormat = VK_FORMAT_UNDEFINED;

  // Linear HDR color written by the active renderer.
  Image              m_Hdr;

  // Tonemapped display color, shown by the UI and read back for PNG captures.
  Image              m_Ldr;

  // Depth attachment for the rasterizer preview.
  Image              m_Depth;
};

}  // namespace rtpt
