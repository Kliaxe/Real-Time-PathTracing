#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <volk.h>
#include <vk_mem_alloc.h>

namespace rtpt
{

class ResourceAllocator;
class GpuExecution;

// Buffer
// Move-only handle to a VMA-backed buffer created by ResourceAllocator.
// Destruction goes back through the allocator, which retires the Vulkan objects so in-flight command buffers can still use them.

struct Buffer
{
  Buffer() = default;
  Buffer(const Buffer&)            = delete;
  Buffer& operator=(const Buffer&) = delete;
  Buffer(Buffer&& other) noexcept;
  Buffer& operator=(Buffer&& other) noexcept;
  ~Buffer();

  // Releases the buffer through its owner. A no-op for an empty handle.
  void Reset();

  // Vulkan buffer handle.
  VkBuffer        buffer = VK_NULL_HANDLE;
  // Size requested at creation, in bytes.
  VkDeviceSize    bufferSize = 0;
  // Device address. Non-zero only for buffers created with shader-device-address usage.
  VkDeviceAddress address = 0;
  // Persistent host mapping. Non-null only for allocations created with the mapped flag.
  std::byte*      mapping = nullptr;
  // VMA allocation backing the buffer.
  VmaAllocation   allocation = VK_NULL_HANDLE;

  [[nodiscard]] explicit operator bool() const noexcept { return buffer != VK_NULL_HANDLE; }

private:

  friend class ResourceAllocator;

  // Allocator that created the buffer. Null for empty or moved-from handles.
  ResourceAllocator* owner = nullptr;
};

// Image
// Move-only handle to a VMA-backed image and its optional view, created by ResourceAllocator.
// It carries the creation parameters that later barriers and descriptor writes need.

struct Image
{
  Image() = default;
  Image(const Image&)            = delete;
  Image& operator=(const Image&) = delete;
  Image(Image&& other) noexcept;
  Image& operator=(Image&& other) noexcept;
  ~Image();

  // Releases the image through its owner. A no-op for an empty handle.
  void Reset();

  // Vulkan image handle.
  VkImage              image = VK_NULL_HANDLE;
  // Extent at creation.
  VkExtent3D            extent {};
  // Mip level count at creation.
  uint32_t              mipLevels = 0;
  // Array layer count at creation.
  uint32_t              arrayLayers = 0;
  // Format at creation.
  VkFormat              format = VK_FORMAT_UNDEFINED;
  // VMA allocation backing the image.
  VmaAllocation         allocation = VK_NULL_HANDLE;
  // The allocator fills in the view and records the initial layout; the sampler is left to callers.
  VkDescriptorImageInfo descriptor {};

  [[nodiscard]] explicit operator bool() const noexcept { return image != VK_NULL_HANDLE; }

private:

  friend class ResourceAllocator;

  // Allocator that created the image. Null for empty or moved-from handles.
  ResourceAllocator* owner = nullptr;
};

// AccelerationStructure
// Move-only handle to an acceleration structure and the buffer that stores it, created by ResourceAllocator.

struct AccelerationStructure
{
  AccelerationStructure() = default;
  AccelerationStructure(const AccelerationStructure&)            = delete;
  AccelerationStructure& operator=(const AccelerationStructure&) = delete;
  AccelerationStructure(AccelerationStructure&& other) noexcept;
  AccelerationStructure& operator=(AccelerationStructure&& other) noexcept;
  ~AccelerationStructure();

  // Releases the structure and its storage through the owner. A no-op for an empty handle.
  void Reset();

  // Vulkan acceleration structure handle.
  VkAccelerationStructureKHR accel = VK_NULL_HANDLE;
  // Device address, referenced by top-level instances.
  VkDeviceAddress            address = 0;
  // Storage buffer sized from the build's reported size.
  Buffer                     buffer {};

  [[nodiscard]] explicit operator bool() const noexcept { return accel != VK_NULL_HANDLE; }

private:

  friend class ResourceAllocator;

  // Allocator that created the structure. Null for empty or moved-from handles.
  ResourceAllocator* owner = nullptr;
};

// Sampler
// Move-only handle to a sampler created by ResourceAllocator, so samplers share the same deferred destruction and leak accounting as other resources.

struct Sampler
{
  Sampler() = default;
  Sampler(const Sampler&)            = delete;
  Sampler& operator=(const Sampler&) = delete;
  Sampler(Sampler&& other) noexcept;
  Sampler& operator=(Sampler&& other) noexcept;
  ~Sampler();

  // Releases the sampler through its owner. A no-op for an empty handle.
  void Reset();

  // Vulkan sampler handle.
  VkSampler sampler = VK_NULL_HANDLE;

  [[nodiscard]] explicit operator bool() const noexcept { return sampler != VK_NULL_HANDLE; }

private:

  friend class ResourceAllocator;

  // Allocator that created the sampler. Null for empty or moved-from handles.
  ResourceAllocator* owner = nullptr;
};

// ResourceAllocator
// Creates and destroys GPU resources on top of VMA.
// Destroy calls clear the handle immediately but retire the Vulkan objects through GpuExecution, so work already submitted can finish using them.
// A live-resource count catches leaks: destroying the allocator while resources remain aborts, because pending retirements would otherwise run against a dead allocator.

class ResourceAllocator
{
public:

  ResourceAllocator() = default;
  ResourceAllocator(const ResourceAllocator&)            = delete;
  ResourceAllocator& operator=(const ResourceAllocator&) = delete;
  ~ResourceAllocator();

  void Initialize(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t apiVersion, GpuExecution& execution);

  // Throws if any owned resource is still alive, including resources whose retirement has not run yet.
  void Destroy();

  // More than one queue family makes the buffer concurrently shared. A non-zero minimum alignment uses VMA's aligned allocation path.
  VkResult CreateBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags2 usage, VmaMemoryUsage memoryUsage = VMA_MEMORY_USAGE_AUTO, VmaAllocationCreateFlags flags = 0, VkDeviceSize minAlignment = 0, std::span<const uint32_t> queueFamilies = {}) const;
  VkResult CreateBuffer(Buffer& buffer, const VkBufferCreateInfo& bufferInfo, const VmaAllocationCreateInfo& allocationInfo, VkDeviceSize minAlignment = 0) const;
  void DestroyBuffer(Buffer& buffer) const;

  // A view is created only when viewInfo is given; its image field is filled in here. Allocation defaults to device-preferred memory.
  VkResult CreateImage(Image& image, const VkImageCreateInfo& imageInfo, const VkImageViewCreateInfo* viewInfo = nullptr, const VmaAllocationCreateInfo* allocationInfo = nullptr) const;
  void DestroyImage(Image& image) const;

  // Allocates the storage buffer and fills in createInfo's buffer field.
  VkResult CreateAccelerationStructure(AccelerationStructure& accelerationStructure, const VkAccelerationStructureCreateInfoKHR& createInfo) const;
  void DestroyAccelerationStructure(AccelerationStructure& accelerationStructure) const;

  VkResult CreateSampler(Sampler& sampler, const VkSamplerCreateInfo& createInfo) const;
  void DestroySampler(Sampler& sampler) const;

  // Mapped memory may not be host-coherent: flush after writing and invalidate before reading.
  VkResult FlushBuffer(const Buffer& buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const;
  VkResult InvalidateBuffer(const Buffer& buffer, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const;

  [[nodiscard]] VkDevice Device() const noexcept { return m_Device; }
  [[nodiscard]] VkPhysicalDevice PhysicalDevice() const noexcept { return m_PhysicalDevice; }
  [[nodiscard]] VmaAllocator Handle() const noexcept { return m_Allocator; }
  [[nodiscard]] uint32_t LiveResourceCount() const noexcept { return m_LiveResourceCount; }

private:

  void TrackCreatedResource() const noexcept;

  // Aborts on underflow, which would mean a resource was destroyed twice.
  void TrackDestroyedResource() const noexcept;

  // VMA allocator.
  VmaAllocator     m_Allocator = VK_NULL_HANDLE;
  // Physical device the allocator was created for.
  VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
  // Logical device used for views, samplers, and acceleration structures.
  VkDevice         m_Device = VK_NULL_HANDLE;
  // Receives retirements so destruction waits for in-flight GPU work.
  GpuExecution*    m_Execution = nullptr;
  // Resources created and not yet released. An acceleration structure and its storage buffer count separately. Mutable because the create and destroy methods are const.
  mutable uint32_t m_LiveResourceCount = 0;
};

}  // namespace rtpt
