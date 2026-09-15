#pragma once

#include <atomic>
#include <stdexcept>
#include <string_view>

#include <volk.h>

namespace rtpt
{

// VulkanError
// Exception thrown for a failed Vulkan call. It keeps the raw VkResult so callers can react to specific failures instead of parsing the message.

class VulkanError final : public std::runtime_error
{
public:

  VulkanError(VkResult result, std::string_view operation);

  [[nodiscard]] VkResult Result() const noexcept;

private:

  // The result that caused the failure.
  VkResult m_Result;
};

// Throws VulkanError for anything other than VK_SUCCESS, including non-error codes such as VK_SUBOPTIMAL_KHR, so callers handle those first.
void CheckVk(VkResult result, std::string_view operation);
const char* VkResultName(VkResult result) noexcept;

// Diagnostics
// Owns the debug-utils messenger that prints validation output and counts validation errors and warnings.
// Tests read the counts to fail a run that produced validation errors.
// Object names and command labels degrade to no-ops when VK_EXT_debug_utils was not enabled.

class Diagnostics
{
public:

  Diagnostics() = default;
  Diagnostics(const Diagnostics&)            = delete;
  Diagnostics& operator=(const Diagnostics&) = delete;

  void Initialize(VkInstance instance);
  void Destroy();
  void ResetCounts() noexcept;

  // Also chained into instance creation so messages from vkCreateInstance itself are reported. The callback's user data is this object, so it must not move.
  [[nodiscard]] VkDebugUtilsMessengerCreateInfoEXT CreateInfo() noexcept;

  void SetObjectName(VkDevice device, VkObjectType type, uint64_t handle, const char* name) const;
  void BeginLabel(VkCommandBuffer commandBuffer, const char* name, const float color[4]) const;
  void EndLabel(VkCommandBuffer commandBuffer) const;

  [[nodiscard]] uint64_t ErrorCount() const noexcept;
  [[nodiscard]] uint64_t WarningCount() const noexcept;

private:

  static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData);

  // Instance the messenger belongs to.
  VkInstance               m_Instance  = VK_NULL_HANDLE;
  // Messenger created by Initialize. Null when validation is off.
  VkDebugUtilsMessengerEXT m_Messenger = VK_NULL_HANDLE;
  // Validation and performance errors seen. Atomic because the callback runs on whichever thread made the Vulkan call.
  std::atomic_uint64_t      m_ErrorCount = 0;
  // Validation and performance warnings seen.
  std::atomic_uint64_t      m_WarningCount = 0;
};

// CommandLabel
// Scoped debug label: opens a command-buffer label on construction and closes it on destruction so begin and end always pair.
// A null Diagnostics makes it a no-op.

class CommandLabel
{
public:

  CommandLabel(const Diagnostics* diagnostics, VkCommandBuffer commandBuffer, const char* name, const float color[4] = nullptr);
  ~CommandLabel();

  CommandLabel(const CommandLabel&)            = delete;
  CommandLabel& operator=(const CommandLabel&) = delete;

private:

  // Label sink, or null to disable labelling.
  const Diagnostics* m_Diagnostics = nullptr;
  // Command buffer the label was opened on.
  VkCommandBuffer    m_CommandBuffer = VK_NULL_HANDLE;
};

}  // namespace rtpt
