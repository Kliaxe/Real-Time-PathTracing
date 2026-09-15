#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include <volk.h>

namespace rtpt
{

// Provides the present fences and vkReleaseSwapchainImagesKHR that Swapchain depends on. Required only for windowed runs.
inline constexpr const char* kSwapchainMaintenanceExtension = VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;

// QueueSelection
// The queue families the renderer submits and presents on, as chosen during device probing.

struct QueueSelection
{
  // First family supporting graphics, compute, and transfer together; every render submission goes here.
  uint32_t renderFamily = VK_QUEUE_FAMILY_IGNORED;
  // First family that can present to the surface. Headless runs reuse renderFamily.
  uint32_t presentFamily = VK_QUEUE_FAMILY_IGNORED;
};

// DeviceFeatureChain
// Every feature structure the renderer cares about, linked into one pNext chain.
// The same chain type is used to query what a device supports and to enable features at device creation, so the two cannot drift apart structurally.
// The pNext pointers point into this object: a copied or moved chain still points at the original, so call Link again before handing a copy to Vulkan.

struct DeviceFeatureChain
{
  // Head of the chain; Vulkan 1.0 core features.
  VkPhysicalDeviceFeatures2                         core { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
  // Buffer device addresses, timeline semaphores, and descriptor indexing.
  VkPhysicalDeviceVulkan12Features                  vulkan12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
  // Synchronization2, dynamic rendering, and demote-to-helper.
  VkPhysicalDeviceVulkan13Features                  vulkan13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
  // Acceleration structure support.
  VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructure { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
  // Ray tracing pipelines and indirect trace dispatch.
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipeline { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
  // Quad-group derivatives in compute shaders.
  VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR computeShaderDerivatives { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR };
  // Swapchain maintenance. Linked only for windowed runs.
  VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchainMaintenance { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR };

  // Headless runs leave the swapchain-maintenance structure off the chain because its extension is not requested.
  void Link(bool windowed);

  // Clears every pNext pointer, for a stored copy whose chain would otherwise still point into the object it was copied or moved from.
  void Unlink();
};

// DeviceSupport
// Everything learned while probing one physical device: selected queues, supported features, relevant properties, and what is missing.
// Rejected devices keep their missing list so the final error can explain why no device qualified.

struct DeviceSupport
{
  // Queue families chosen on this device.
  QueueSelection                   queues {};
  // Features the device reports.
  DeviceFeatureChain               features {};
  // Core properties; head of the properties pNext chain.
  VkPhysicalDeviceProperties2      properties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
  // Shader group handle size and alignment limits used to lay out shader binding tables.
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingProperties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
  // Scratch offset alignment used when allocating acceleration-structure scratch buffers.
  VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationStructureProperties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
  // Human-readable names of unmet requirements. Empty means the device qualifies.
  std::vector<std::string>         missing;

  [[nodiscard]] bool Complete() const noexcept { return missing.empty(); }
};

[[nodiscard]] std::vector<const char*> RequiredDeviceExtensions(bool windowed);

// Probes one device against every renderer requirement. The surface is only consulted when windowed.
[[nodiscard]] DeviceSupport QueryDeviceSupport(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, bool windowed);

}  // namespace rtpt
