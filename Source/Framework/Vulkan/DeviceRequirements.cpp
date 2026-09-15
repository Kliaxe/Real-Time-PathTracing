#include "DeviceRequirements.h"

#include "Diagnostics.h"

#include <algorithm>
#include <string_view>

namespace rtpt
{
namespace
{
bool HasExtension(std::span<const VkExtensionProperties> available, const char* name)
{
  return std::ranges::any_of(available, [name](const VkExtensionProperties& extension) {
    return std::string_view(extension.extensionName) == name;
  });
}

// Records a requirement by name instead of failing on the first one, so the error can list everything a device lacks.
void Require(bool supported, std::string name, std::vector<std::string>& missing)
{
  if(!supported)
  {
    missing.push_back(std::move(name));
  }
}
}  // namespace

void DeviceFeatureChain::Link(bool windowed)
{
  // The swapchain-maintenance structure is only valid while its extension is enabled, which is only in windowed runs.
  core.pNext                     = &vulkan12;
  vulkan12.pNext                 = &vulkan13;
  vulkan13.pNext                 = &accelerationStructure;
  accelerationStructure.pNext    = &rayTracingPipeline;
  rayTracingPipeline.pNext       = &computeShaderDerivatives;
  computeShaderDerivatives.pNext = windowed ? &swapchainMaintenance : nullptr;
  swapchainMaintenance.pNext     = nullptr;
}

void DeviceFeatureChain::Unlink()
{
  core.pNext                     = nullptr;
  vulkan12.pNext                 = nullptr;
  vulkan13.pNext                 = nullptr;
  accelerationStructure.pNext    = nullptr;
  rayTracingPipeline.pNext       = nullptr;
  computeShaderDerivatives.pNext = nullptr;
  swapchainMaintenance.pNext     = nullptr;
}

std::vector<const char*> RequiredDeviceExtensions(bool windowed)
{
  // Deferred host operations is listed because the acceleration-structure extension depends on it.
  std::vector<const char*> extensions {
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME,
  };

  if(windowed)
  {
    extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    extensions.push_back(kSwapchainMaintenanceExtension);
  }

  return extensions;
}

DeviceSupport QueryDeviceSupport(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, bool windowed)
{
  // Features and properties
  // Both are read through pNext chains so the extension structures are filled in the same call.

  DeviceSupport support;
  support.features.Link(windowed);
  vkGetPhysicalDeviceFeatures2(physicalDevice, &support.features.core);

  support.properties.pNext           = &support.rayTracingProperties;
  support.rayTracingProperties.pNext = &support.accelerationStructureProperties;
  vkGetPhysicalDeviceProperties2(physicalDevice, &support.properties);

  // Extensions

  uint32_t extensionCount = 0;
  CheckVk(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr), "vkEnumerateDeviceExtensionProperties(count)");

  std::vector<VkExtensionProperties> availableExtensions(extensionCount);
  CheckVk(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, availableExtensions.data()), "vkEnumerateDeviceExtensionProperties");

  for(const char* extension : RequiredDeviceExtensions(windowed))
  {
    Require(HasExtension(availableExtensions, extension), std::string("extension ") + extension, support.missing);
  }

  // Queues
  // The renderer submits everything to one queue, so the render family must support graphics, compute, and transfer together.
  // The first qualifying family wins for both roles. Headless runs never present, so the render family stands in for presentation.

  uint32_t queueCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, nullptr);

  std::vector<VkQueueFamilyProperties> queues(queueCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, queues.data());

  for(uint32_t index = 0; index < queueCount; ++index)
  {
    const VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;

    if(support.queues.renderFamily == VK_QUEUE_FAMILY_IGNORED && (queues[index].queueFlags & required) == required)
    {
      support.queues.renderFamily = index;
    }

    if(windowed)
    {
      VkBool32 present = VK_FALSE;
      CheckVk(vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, index, surface, &present), "vkGetPhysicalDeviceSurfaceSupportKHR");

      if(support.queues.presentFamily == VK_QUEUE_FAMILY_IGNORED && present)
      {
        support.queues.presentFamily = index;
      }
    }
  }

  if(!windowed)
  {
    support.queues.presentFamily = support.queues.renderFamily;
  }

  Require(support.queues.renderFamily != VK_QUEUE_FAMILY_IGNORED, "graphics/compute/transfer queue", support.missing);
  Require(support.queues.presentFamily != VK_QUEUE_FAMILY_IGNORED, "presentation queue", support.missing);

  // Features
  // Every feature checked here must also be enabled in VulkanDevice::Initialize; the two lists are kept in sync by hand.

  const VkPhysicalDeviceFeatures& core = support.features.core.features;
  Require(core.shaderInt16, "feature shaderInt16", support.missing);
  Require(core.shaderInt64, "feature shaderInt64", support.missing);

  const VkPhysicalDeviceVulkan12Features& v12 = support.features.vulkan12;
  Require(v12.bufferDeviceAddress, "feature bufferDeviceAddress", support.missing);
  Require(v12.scalarBlockLayout, "feature scalarBlockLayout", support.missing);
  Require(v12.timelineSemaphore, "feature timelineSemaphore", support.missing);
  Require(v12.runtimeDescriptorArray, "feature runtimeDescriptorArray", support.missing);
  Require(v12.shaderSampledImageArrayNonUniformIndexing, "feature shaderSampledImageArrayNonUniformIndexing", support.missing);
  Require(v12.descriptorBindingSampledImageUpdateAfterBind, "feature descriptorBindingSampledImageUpdateAfterBind", support.missing);
  Require(v12.descriptorBindingUpdateUnusedWhilePending, "feature descriptorBindingUpdateUnusedWhilePending", support.missing);
  Require(v12.descriptorBindingPartiallyBound, "feature descriptorBindingPartiallyBound", support.missing);

  const VkPhysicalDeviceVulkan13Features& v13 = support.features.vulkan13;
  Require(v13.synchronization2, "feature synchronization2", support.missing);
  Require(v13.dynamicRendering, "feature dynamicRendering", support.missing);
  Require(v13.shaderDemoteToHelperInvocation, "feature shaderDemoteToHelperInvocation", support.missing);
  Require(support.features.accelerationStructure.accelerationStructure, "feature accelerationStructure", support.missing);
  Require(support.features.rayTracingPipeline.rayTracingPipeline, "feature rayTracingPipeline", support.missing);
  Require(support.features.rayTracingPipeline.rayTracingPipelineTraceRaysIndirect, "feature rayTracingPipelineTraceRaysIndirect", support.missing);
  Require(support.features.computeShaderDerivatives.computeDerivativeGroupQuads, "feature computeDerivativeGroupQuads", support.missing);

  if(windowed)
  {
    Require(support.features.swapchainMaintenance.swapchainMaintenance1, "feature swapchainMaintenance1", support.missing);
  }

  return support;
}

}  // namespace rtpt
