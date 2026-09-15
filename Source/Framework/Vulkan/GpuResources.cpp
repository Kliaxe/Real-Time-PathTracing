#include <cstdio>

// VMA configuration
// This translation unit holds the VMA implementation, so the configuration macros must be defined before GpuResources.h pulls in vk_mem_alloc.h.
// volk loads Vulkan function pointers at runtime, so VMA resolves its functions dynamically instead of linking them statically. Leak reports go to stderr.

#define VMA_IMPLEMENTATION
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_LEAK_LOG_FORMAT(format, ...)                                                                               \
  {                                                                                                                    \
    std::fprintf(stderr, format, __VA_ARGS__);                                                                         \
    std::fprintf(stderr, "\n");                                                                                        \
  }
#include "GpuResources.h"

#include "GpuExecution.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace rtpt
{

// Resource handles
// Each handle's move operations release the destination first, then take every field from the source, leaving it empty.
// Reset forwards to the owning allocator, so a moved-from or default handle destroys nothing.

Buffer::Buffer(Buffer&& other) noexcept
{
  *this = std::move(other);
}

Buffer& Buffer::operator=(Buffer&& other) noexcept
{
  if(this != &other)
  {
    Reset();

    buffer     = std::exchange(other.buffer, VK_NULL_HANDLE);
    bufferSize = std::exchange(other.bufferSize, 0);
    address    = std::exchange(other.address, 0);
    mapping    = std::exchange(other.mapping, nullptr);
    allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
    owner      = std::exchange(other.owner, nullptr);
  }

  return *this;
}

Buffer::~Buffer()
{
  Reset();
}

void Buffer::Reset()
{
  if(owner != nullptr)
  {
    owner->DestroyBuffer(*this);
  }
}

Image::Image(Image&& other) noexcept
{
  *this = std::move(other);
}

Image& Image::operator=(Image&& other) noexcept
{
  if(this != &other)
  {
    Reset();

    image       = std::exchange(other.image, VK_NULL_HANDLE);
    extent      = std::exchange(other.extent, VkExtent3D {});
    mipLevels   = std::exchange(other.mipLevels, 0);
    arrayLayers = std::exchange(other.arrayLayers, 0);
    format      = std::exchange(other.format, VK_FORMAT_UNDEFINED);
    allocation  = std::exchange(other.allocation, VK_NULL_HANDLE);
    descriptor  = std::exchange(other.descriptor, VkDescriptorImageInfo {});
    owner       = std::exchange(other.owner, nullptr);
  }

  return *this;
}

Image::~Image()
{
  Reset();
}

void Image::Reset()
{
  if(owner != nullptr)
  {
    owner->DestroyImage(*this);
  }
}

AccelerationStructure::AccelerationStructure(AccelerationStructure&& other) noexcept
{
  *this = std::move(other);
}

AccelerationStructure& AccelerationStructure::operator=(AccelerationStructure&& other) noexcept
{
  if(this != &other)
  {
    Reset();

    accel   = std::exchange(other.accel, VK_NULL_HANDLE);
    address = std::exchange(other.address, 0);
    buffer  = std::move(other.buffer);
    owner   = std::exchange(other.owner, nullptr);
  }

  return *this;
}

AccelerationStructure::~AccelerationStructure()
{
  Reset();
}

void AccelerationStructure::Reset()
{
  if(owner != nullptr)
  {
    owner->DestroyAccelerationStructure(*this);
  }
}

Sampler::Sampler(Sampler&& other) noexcept
{
  *this = std::move(other);
}

Sampler& Sampler::operator=(Sampler&& other) noexcept
{
  if(this != &other)
  {
    Reset();

    sampler = std::exchange(other.sampler, VK_NULL_HANDLE);
    owner   = std::exchange(other.owner, nullptr);
  }

  return *this;
}

Sampler::~Sampler()
{
  Reset();
}

void Sampler::Reset()
{
  if(owner != nullptr)
  {
    owner->DestroySampler(*this);
  }
}

ResourceAllocator::~ResourceAllocator()
{
  // A destructor cannot throw, and pending retirements capture this allocator, so outstanding resources abort the process instead.
  if(m_LiveResourceCount != 0)
  {
    std::fprintf(stderr, "ResourceAllocator destroyed with %u live resource(s)\n", m_LiveResourceCount);
    std::abort();
  }

  Destroy();
}

void ResourceAllocator::Initialize(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t apiVersion, GpuExecution& execution)
{
  if(m_Allocator != VK_NULL_HANDLE || instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE || execution.Queue() == VK_NULL_HANDLE)
  {
    throw std::invalid_argument("invalid ResourceAllocator initialization");
  }

  // VMA allocator
  // Only the two loader entry points are passed; VMA looks up the rest itself. The buffer-device-address flag lets VMA allocate memory for buffers whose device address shaders use.

  const VmaVulkanFunctions vulkanFunctions {
    .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
    .vkGetDeviceProcAddr   = vkGetDeviceProcAddr,
  };

  const VmaAllocatorCreateInfo createInfo {
    .flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
    .physicalDevice   = physicalDevice,
    .device           = device,
    .pVulkanFunctions = &vulkanFunctions,
    .instance         = instance,
    .vulkanApiVersion = apiVersion,
  };

  if(vmaCreateAllocator(&createInfo, &m_Allocator) != VK_SUCCESS)
  {
    throw std::runtime_error("vmaCreateAllocator failed");
  }

  m_PhysicalDevice = physicalDevice;
  m_Device         = device;
  m_Execution      = &execution;
}

void ResourceAllocator::Destroy()
{
  if(m_LiveResourceCount != 0)
  {
    throw std::logic_error("ResourceAllocator::Destroy requires all owned resources to be destroyed first");
  }

  if(m_Allocator != VK_NULL_HANDLE)
  {
    vmaDestroyAllocator(m_Allocator);
  }

  m_Allocator      = VK_NULL_HANDLE;
  m_PhysicalDevice = VK_NULL_HANDLE;
  m_Device         = VK_NULL_HANDLE;
  m_Execution      = nullptr;
}

VkResult ResourceAllocator::CreateBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags2 usage, VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags flags, VkDeviceSize minAlignment, std::span<const uint32_t> queueFamilies) const
{
  // Usage bits are narrowed to the 32-bit VkBufferUsageFlags field of VkBufferCreateInfo.
  const VkBufferCreateInfo bufferInfo {
    .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size                  = size,
    .usage                 = static_cast<VkBufferUsageFlags>(usage),
    .sharingMode           = queueFamilies.size() > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
    .queueFamilyIndexCount = static_cast<uint32_t>(queueFamilies.size()),
    .pQueueFamilyIndices   = queueFamilies.data(),
  };

  const VmaAllocationCreateInfo allocationInfo { .flags = flags, .usage = memoryUsage };

  return CreateBuffer(buffer, bufferInfo, allocationInfo, minAlignment);
}

VkResult ResourceAllocator::CreateBuffer(Buffer& buffer, const VkBufferCreateInfo& bufferInfo, const VmaAllocationCreateInfo& allocationInfo, VkDeviceSize minAlignment) const
{
  // A non-empty handle would be overwritten and leak.
  if(m_Allocator == VK_NULL_HANDLE || buffer)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  // Allocation

  VmaAllocationInfo resolvedAllocation {};
  const VkResult result = minAlignment == 0 ? vmaCreateBuffer(m_Allocator, &bufferInfo, &allocationInfo, &buffer.buffer, &buffer.allocation, &resolvedAllocation) : vmaCreateBufferWithAlignment(m_Allocator, &bufferInfo, &allocationInfo, minAlignment, &buffer.buffer, &buffer.allocation, &resolvedAllocation);

  if(result != VK_SUCCESS)
  {
    return result;
  }

  // Handle fields
  // pMappedData is non-null only for allocations requested with the mapped flag. The device address is queried only when the usage allows it.

  buffer.bufferSize = bufferInfo.size;
  buffer.mapping    = static_cast<std::byte*>(resolvedAllocation.pMappedData);
  buffer.owner      = const_cast<ResourceAllocator*>(this);
  TrackCreatedResource();

  if((bufferInfo.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0)
  {
    const VkBufferDeviceAddressInfo addressInfo { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer.buffer };
    buffer.address = vkGetBufferDeviceAddress(m_Device, &addressInfo);
  }

  return VK_SUCCESS;
}

void ResourceAllocator::DestroyBuffer(Buffer& buffer) const
{
  // Clear the handle now
  // The handle is emptied immediately so it can be reused and a second destroy is a no-op, while the Vulkan objects wait for in-flight GPU work.

  const VkBuffer rawBuffer = buffer.buffer;
  const VmaAllocation allocation = buffer.allocation;

  buffer.buffer     = VK_NULL_HANDLE;
  buffer.bufferSize = 0;
  buffer.address    = 0;
  buffer.mapping    = nullptr;
  buffer.allocation = VK_NULL_HANDLE;
  buffer.owner      = nullptr;

  // Retire the objects

  if(rawBuffer != VK_NULL_HANDLE)
  {
    m_Execution->Retire([this, rawBuffer, allocation] {
      vmaDestroyBuffer(m_Allocator, rawBuffer, allocation);
      TrackDestroyedResource();
    });
  }
}

VkResult ResourceAllocator::CreateImage(Image& image, const VkImageCreateInfo& imageInfo, const VkImageViewCreateInfo* viewInfo, const VmaAllocationCreateInfo* allocationInfo) const
{
  // A non-empty handle would be overwritten and leak.
  if(m_Allocator == VK_NULL_HANDLE || image)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  // Allocation

  const VmaAllocationCreateInfo defaultAllocation { .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE };
  const VmaAllocationCreateInfo& allocation = allocationInfo == nullptr ? defaultAllocation : *allocationInfo;

  VkResult result = vmaCreateImage(m_Allocator, &imageInfo, &allocation, &image.image, &image.allocation, nullptr);

  if(result != VK_SUCCESS)
  {
    return result;
  }

  // Handle fields
  // The descriptor's layout is initialized to the creation layout; the allocator never updates it afterwards.

  image.extent      = imageInfo.extent;
  image.mipLevels   = imageInfo.mipLevels;
  image.arrayLayers = imageInfo.arrayLayers;
  image.format      = imageInfo.format;
  image.descriptor.imageLayout = imageInfo.initialLayout;
  image.owner       = const_cast<ResourceAllocator*>(this);
  TrackCreatedResource();

  // View
  // The image is already tracked, so a failed view goes through DestroyImage to keep the count balanced.

  if(viewInfo != nullptr)
  {
    VkImageViewCreateInfo resolvedView = *viewInfo;
    resolvedView.image = image.image;

    result = vkCreateImageView(m_Device, &resolvedView, nullptr, &image.descriptor.imageView);

    if(result != VK_SUCCESS)
    {
      DestroyImage(image);
      return result;
    }
  }

  return VK_SUCCESS;
}

void ResourceAllocator::DestroyImage(Image& image) const
{
  // Clear the handle now
  // The handle is emptied immediately so it can be reused and a second destroy is a no-op, while the Vulkan objects wait for in-flight GPU work.

  const VkImage rawImage = image.image;
  const VkImageView rawView = image.descriptor.imageView;
  const VmaAllocation allocation = image.allocation;

  image.image       = VK_NULL_HANDLE;
  image.extent      = {};
  image.mipLevels   = 0;
  image.arrayLayers = 0;
  image.format      = VK_FORMAT_UNDEFINED;
  image.allocation  = VK_NULL_HANDLE;
  image.descriptor  = {};
  image.owner       = nullptr;

  // Retire the objects
  // The view is destroyed before the image it refers to.

  if(rawImage != VK_NULL_HANDLE)
  {
    m_Execution->Retire([this, rawImage, rawView, allocation] {
      if(rawView != VK_NULL_HANDLE)
      {
        vkDestroyImageView(m_Device, rawView, nullptr);
      }
      vmaDestroyImage(m_Allocator, rawImage, allocation);
      TrackDestroyedResource();
    });
  }
}

VkResult ResourceAllocator::CreateAccelerationStructure(AccelerationStructure& accelerationStructure, const VkAccelerationStructureCreateInfoKHR& createInfo) const
{
  // Storage
  // The structure lives inside a buffer the allocator creates here; it also needs a device address for the build.

  VkResult result = CreateBuffer(accelerationStructure.buffer, createInfo.size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

  if(result != VK_SUCCESS)
  {
    return result;
  }

  // Structure

  VkAccelerationStructureCreateInfoKHR resolved = createInfo;
  resolved.buffer = accelerationStructure.buffer.buffer;

  result = vkCreateAccelerationStructureKHR(m_Device, &resolved, nullptr, &accelerationStructure.accel);

  if(result != VK_SUCCESS)
  {
    DestroyBuffer(accelerationStructure.buffer);
    return result;
  }

  // Address
  // Top-level instances refer to bottom-level structures by this address. The structure is tracked separately from its storage buffer.

  const VkAccelerationStructureDeviceAddressInfoKHR addressInfo {
    .sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
    .accelerationStructure = accelerationStructure.accel,
  };

  accelerationStructure.address = vkGetAccelerationStructureDeviceAddressKHR(m_Device, &addressInfo);
  accelerationStructure.owner   = const_cast<ResourceAllocator*>(this);
  TrackCreatedResource();

  return VK_SUCCESS;
}

void ResourceAllocator::DestroyAccelerationStructure(AccelerationStructure& accelerationStructure) const
{
  // The structure is retired before its storage buffer, so the retirement queue destroys them in that order.
  const VkAccelerationStructureKHR rawAcceleration = accelerationStructure.accel;

  accelerationStructure.accel   = VK_NULL_HANDLE;
  accelerationStructure.address = 0;
  accelerationStructure.owner   = nullptr;

  if(rawAcceleration != VK_NULL_HANDLE)
  {
    m_Execution->Retire([this, rawAcceleration] {
      vkDestroyAccelerationStructureKHR(m_Device, rawAcceleration, nullptr);
      TrackDestroyedResource();
    });
  }

  DestroyBuffer(accelerationStructure.buffer);
}

VkResult ResourceAllocator::CreateSampler(Sampler& sampler, const VkSamplerCreateInfo& createInfo) const
{
  if(m_Allocator == VK_NULL_HANDLE || sampler)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkResult result = vkCreateSampler(m_Device, &createInfo, nullptr, &sampler.sampler);

  if(result == VK_SUCCESS)
  {
    sampler.owner = const_cast<ResourceAllocator*>(this);
    TrackCreatedResource();
  }

  return result;
}

void ResourceAllocator::DestroySampler(Sampler& sampler) const
{
  const VkSampler rawSampler = sampler.sampler;

  sampler.sampler = VK_NULL_HANDLE;
  sampler.owner   = nullptr;

  if(rawSampler != VK_NULL_HANDLE)
  {
    m_Execution->Retire([this, rawSampler] {
      vkDestroySampler(m_Device, rawSampler, nullptr);
      TrackDestroyedResource();
    });
  }
}

VkResult ResourceAllocator::FlushBuffer(const Buffer& buffer, VkDeviceSize offset, VkDeviceSize size) const
{
  return vmaFlushAllocation(m_Allocator, buffer.allocation, offset, size);
}

VkResult ResourceAllocator::InvalidateBuffer(const Buffer& buffer, VkDeviceSize offset, VkDeviceSize size) const
{
  return vmaInvalidateAllocation(m_Allocator, buffer.allocation, offset, size);
}

void ResourceAllocator::TrackCreatedResource() const noexcept
{
  ++m_LiveResourceCount;
}

void ResourceAllocator::TrackDestroyedResource() const noexcept
{
  // This runs inside retirement callbacks and is noexcept, so an accounting error aborts instead of throwing.
  if(m_LiveResourceCount == 0)
  {
    std::fprintf(stderr, "ResourceAllocator resource accounting underflow\n");
    std::abort();
  }

  --m_LiveResourceCount;
}

}  // namespace rtpt
