#include "VulkanDevice.h"

#include "Diagnostics.h"

#include <algorithm>
#include <array>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace rtpt
{
namespace
{
// Discrete GPUs always outrank integrated ones through the top bit; the maximum 2D image dimension breaks ties within a type.
uint64_t DeviceScore(const DeviceSupport& support)
{
  const bool discrete = support.properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
  return (discrete ? uint64_t { 1 } << 63 : 0) | support.properties.properties.limits.maxImageDimension2D;
}

// Lists every rejected device with its missing requirements so the user can see why each one failed.
std::string BuildUnsupportedMessage(const std::vector<std::pair<std::string, DeviceSupport>>& rejected)
{
  std::ostringstream message;
  message << "no Vulkan device satisfies the renderer requirements";

  for(const auto& [name, support] : rejected)
  {
    message << "\n  " << name << ':';
    for(const std::string& missing : support.missing)
    {
      message << "\n    - " << missing;
    }
  }

  return message.str();
}
}  // namespace

VulkanDevice::~VulkanDevice()
{
  Destroy();
}

void VulkanDevice::Initialize(VkInstance instance, VkSurfaceKHR surface)
{
  if(m_Device != VK_NULL_HANDLE || instance == VK_NULL_HANDLE)
  {
    throw std::invalid_argument("invalid VulkanDevice initialization");
  }

  const bool windowed = surface != VK_NULL_HANDLE;

  // Enumerate

  uint32_t physicalDeviceCount = 0;
  CheckVk(vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, nullptr), "vkEnumeratePhysicalDevices(count)");

  if(physicalDeviceCount == 0)
  {
    throw std::runtime_error("no Vulkan physical devices were found");
  }

  std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
  CheckVk(vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices.data()), "vkEnumeratePhysicalDevices");

  // Select
  // Only devices that meet every requirement compete on score. Rejected devices are kept by name for the error message.

  std::vector<std::pair<std::string, DeviceSupport>> rejected;
  uint64_t bestScore = 0;

  for(VkPhysicalDevice candidate : physicalDevices)
  {
    DeviceSupport support = QueryDeviceSupport(candidate, surface, windowed);

    if(!support.Complete())
    {
      rejected.emplace_back(support.properties.properties.deviceName, std::move(support));
      continue;
    }

    const uint64_t score = DeviceScore(support);

    if(m_PhysicalDevice == VK_NULL_HANDLE || score > bestScore)
    {
      bestScore        = score;
      m_PhysicalDevice = candidate;
      m_Support        = std::move(support);
    }
  }

  if(m_PhysicalDevice == VK_NULL_HANDLE)
  {
    throw std::runtime_error(BuildUnsupportedMessage(rejected));
  }

  // Stored support
  // The probe linked its pNext chains inside the local it was moved from, so the stored pointers dangle. Every reader only takes values and nothing hands the stored support back to Vulkan, so the chains are cleared rather than relinked; copies taken from it, such as a renderer's ray tracing properties, then carry no dangling pointer either.

  m_Support.features.Unlink();

  m_Support.properties.pNext           = nullptr;
  m_Support.rayTracingProperties.pNext = nullptr;

  m_Queues = m_Support.queues;

  // Queues
  // Vulkan rejects duplicate queue family entries, so render and present share one entry when they are the same family.

  const std::set<uint32_t> uniqueQueueFamilies { m_Queues.renderFamily, m_Queues.presentFamily };
  constexpr float priority = 1.0f;
  std::vector<VkDeviceQueueCreateInfo> queueInfos;

  for(uint32_t family : uniqueQueueFamilies)
  {
    queueInfos.push_back(VkDeviceQueueCreateInfo {
      .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family,
      .queueCount       = 1,
      .pQueuePriorities = &priority,
    });
  }

  // Features
  // A fresh chain enables exactly the features QueryDeviceSupport required, rather than everything the device reported.
  // privateData is the one exception: the renderer never uses it, but Streamline's DLSS plugins create private data slots on this device. Vulkan 1.3 makes it mandatory for every device, so enabling it cannot fail.

  DeviceFeatureChain enabled;
  enabled.Link(windowed);

  enabled.core.features.shaderInt16                                = VK_TRUE;
  enabled.core.features.shaderInt64                                = VK_TRUE;
  enabled.vulkan12.bufferDeviceAddress                             = VK_TRUE;
  enabled.vulkan12.scalarBlockLayout                               = VK_TRUE;
  enabled.vulkan12.timelineSemaphore                               = VK_TRUE;
  enabled.vulkan12.runtimeDescriptorArray                          = VK_TRUE;
  enabled.vulkan12.shaderSampledImageArrayNonUniformIndexing       = VK_TRUE;
  enabled.vulkan12.descriptorBindingSampledImageUpdateAfterBind    = VK_TRUE;
  enabled.vulkan12.descriptorBindingUpdateUnusedWhilePending       = VK_TRUE;
  enabled.vulkan12.descriptorBindingPartiallyBound                 = VK_TRUE;
  enabled.vulkan13.synchronization2                                = VK_TRUE;
  enabled.vulkan13.dynamicRendering                                = VK_TRUE;
  enabled.vulkan13.shaderDemoteToHelperInvocation                  = VK_TRUE;
  enabled.vulkan13.privateData                                     = VK_TRUE;
  enabled.accelerationStructure.accelerationStructure              = VK_TRUE;
  enabled.rayTracingPipeline.rayTracingPipeline                    = VK_TRUE;
  enabled.rayTracingPipeline.rayTracingPipelineTraceRaysIndirect   = VK_TRUE;
  enabled.computeShaderDerivatives.computeDerivativeGroupQuads     = VK_TRUE;

  if(windowed)
  {
    enabled.swapchainMaintenance.swapchainMaintenance1 = VK_TRUE;
  }

  // Device
  // volkLoadDevice switches volk's function table to direct dispatch for this device.

  const std::vector<const char*> extensions = RequiredDeviceExtensions(windowed);

  const VkDeviceCreateInfo deviceInfo {
    .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .pNext                   = &enabled.core,
    .queueCreateInfoCount    = static_cast<uint32_t>(queueInfos.size()),
    .pQueueCreateInfos       = queueInfos.data(),
    .enabledExtensionCount   = static_cast<uint32_t>(extensions.size()),
    .ppEnabledExtensionNames = extensions.data(),
  };

  CheckVk(vkCreateDevice(m_PhysicalDevice, &deviceInfo, nullptr, &m_Device), "vkCreateDevice");
  volkLoadDevice(m_Device);

  vkGetDeviceQueue(m_Device, m_Queues.renderFamily, 0, &m_RenderQueue);
  vkGetDeviceQueue(m_Device, m_Queues.presentFamily, 0, &m_PresentQueue);
}

void VulkanDevice::Destroy()
{
  if(m_Device != VK_NULL_HANDLE)
  {
    vkDestroyDevice(m_Device, nullptr);
  }

  m_PhysicalDevice = VK_NULL_HANDLE;
  m_Device         = VK_NULL_HANDLE;
  m_RenderQueue    = VK_NULL_HANDLE;
  m_PresentQueue   = VK_NULL_HANDLE;
  m_Queues         = {};
  m_Support        = {};
}

}  // namespace rtpt
