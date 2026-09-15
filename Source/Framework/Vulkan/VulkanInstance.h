#pragma once

#include "Diagnostics.h"

#include <span>
#include <string_view>

namespace rtpt
{

// VulkanInstanceCreateInfo
// Options for creating the Vulkan instance.

struct VulkanInstanceCreateInfo
{
  // Reported to the driver in VkApplicationInfo.
  std::string_view       applicationName = "RealTimePathTracing";
  // Instance extensions the caller needs, such as the window system's surface extensions.
  std::span<const char* const> requiredExtensions {};
  // Enables the Khronos validation layer and debug messenger.
  bool                   validation = false;
  // Adds synchronization validation on top of validation. Requires validation.
  bool                   synchronizationValidation = false;
};

// VulkanInstance
// Loads the Vulkan loader through volk, creates the instance, and owns the Diagnostics that report validation output.
// It fails early with a descriptive error when the loader, an extension, or the validation layer does not meet requirements.

class VulkanInstance
{
public:

  VulkanInstance() = default;
  VulkanInstance(const VulkanInstance&)            = delete;
  VulkanInstance& operator=(const VulkanInstance&) = delete;
  ~VulkanInstance();

  void Initialize(const VulkanInstanceCreateInfo& createInfo);
  void Destroy();

  [[nodiscard]] VkInstance Handle() const noexcept { return m_Instance; }
  [[nodiscard]] Diagnostics& Debug() noexcept { return m_Diagnostics; }
  [[nodiscard]] const Diagnostics& Debug() const noexcept { return m_Diagnostics; }
  [[nodiscard]] uint32_t ApiVersion() const noexcept { return m_ApiVersion; }

private:

  // Instance handle.
  VkInstance  m_Instance = VK_NULL_HANDLE;
  // Debug messenger and validation counters. Inactive when validation is off.
  Diagnostics m_Diagnostics;
  // API version the instance was created for. Always Vulkan 1.3 after Initialize.
  uint32_t    m_ApiVersion = 0;
};

}  // namespace rtpt
