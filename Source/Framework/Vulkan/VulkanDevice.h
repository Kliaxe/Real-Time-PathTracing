#pragma once

#include "DeviceRequirements.h"

namespace rtpt
{

// VulkanDevice
// Picks the best physical device that meets every renderer requirement and creates the logical device with exactly those features enabled.
// Passing a surface selects windowed mode, which adds presentation requirements; without one the device is set up for headless runs.

class VulkanDevice
{
public:

  VulkanDevice() = default;
  VulkanDevice(const VulkanDevice&)            = delete;
  VulkanDevice& operator=(const VulkanDevice&) = delete;
  ~VulkanDevice();

  // Throws with a per-device list of missing requirements when no device qualifies.
  void Initialize(VkInstance instance, VkSurfaceKHR surface = VK_NULL_HANDLE);
  void Destroy();

  [[nodiscard]] VkDevice Handle() const noexcept { return m_Device; }
  [[nodiscard]] VkPhysicalDevice PhysicalDevice() const noexcept { return m_PhysicalDevice; }
  [[nodiscard]] VkQueue RenderQueue() const noexcept { return m_RenderQueue; }
  [[nodiscard]] VkQueue PresentQueue() const noexcept { return m_PresentQueue; }
  [[nodiscard]] const QueueSelection& Queues() const noexcept { return m_Queues; }
  [[nodiscard]] const DeviceSupport& Support() const noexcept { return m_Support; }

private:

  // Selected physical device.
  VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
  // Logical device.
  VkDevice         m_Device = VK_NULL_HANDLE;
  // Queue 0 of the render family.
  VkQueue          m_RenderQueue = VK_NULL_HANDLE;
  // Queue 0 of the present family. May be the same queue as m_RenderQueue.
  VkQueue          m_PresentQueue = VK_NULL_HANDLE;
  // Queue families chosen for the selected device.
  QueueSelection   m_Queues {};
  // Probe results for the selected device, for reading values only. Initialize clears its pNext pointers after the move into this member, because they pointed into the probe's local.
  DeviceSupport    m_Support {};
};

}  // namespace rtpt
