#include "Diagnostics.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

namespace rtpt
{
namespace
{
std::string BuildErrorMessage(VkResult result, std::string_view operation)
{
  return std::string(operation) + " failed with " + VkResultName(result) + " (" + std::to_string(result) + ")";
}
}  // namespace

VulkanError::VulkanError(VkResult result, std::string_view operation)
  : std::runtime_error(BuildErrorMessage(result, operation)), m_Result(result)
{
}

VkResult VulkanError::Result() const noexcept
{
  return m_Result;
}

void CheckVk(VkResult result, std::string_view operation)
{
  if(result != VK_SUCCESS)
  {
    throw VulkanError(result, operation);
  }
}

const char* VkResultName(VkResult result) noexcept
{
  switch(result)
  {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    default: return "VK_RESULT_UNKNOWN";
  }
}

void Diagnostics::Initialize(VkInstance instance)
{
  m_Instance = instance;

  const VkDebugUtilsMessengerCreateInfoEXT createInfo = CreateInfo();
  CheckVk(vkCreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &m_Messenger), "vkCreateDebugUtilsMessengerEXT");
}

void Diagnostics::ResetCounts() noexcept
{
  m_ErrorCount.store(0, std::memory_order_relaxed);
  m_WarningCount.store(0, std::memory_order_relaxed);
}

VkDebugUtilsMessengerCreateInfoEXT Diagnostics::CreateInfo() noexcept
{
  // Every severity is subscribed so verbose and info output is printed too; only validation and performance errors and warnings are counted.
  return {
    .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
    .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
    .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
    .pfnUserCallback = DebugCallback,
    .pUserData       = this,
  };
}

void Diagnostics::Destroy()
{
  if(m_Messenger != VK_NULL_HANDLE)
  {
    vkDestroyDebugUtilsMessengerEXT(m_Instance, m_Messenger, nullptr);
  }

  m_Messenger = VK_NULL_HANDLE;
  m_Instance  = VK_NULL_HANDLE;
}

void Diagnostics::SetObjectName(VkDevice device, VkObjectType type, uint64_t handle, const char* name) const
{
  // volk leaves the function pointer null when VK_EXT_debug_utils is not enabled.
  if(handle == 0 || name == nullptr || vkSetDebugUtilsObjectNameEXT == nullptr)
  {
    return;
  }

  const VkDebugUtilsObjectNameInfoEXT info {
    .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
    .objectType   = type,
    .objectHandle = handle,
    .pObjectName  = name,
  };

  CheckVk(vkSetDebugUtilsObjectNameEXT(device, &info), "vkSetDebugUtilsObjectNameEXT");
}

void Diagnostics::BeginLabel(VkCommandBuffer commandBuffer, const char* name, const float color[4]) const
{
  // volk leaves the function pointer null when VK_EXT_debug_utils is not enabled.
  if(vkCmdBeginDebugUtilsLabelEXT == nullptr)
  {
    return;
  }

  const std::array<float, 4> defaultColor { 0.25F, 0.55F, 0.85F, 1.0F };

  VkDebugUtilsLabelEXT label { .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = name };

  const float* source = color == nullptr ? defaultColor.data() : color;
  std::copy_n(source, 4, label.color);

  vkCmdBeginDebugUtilsLabelEXT(commandBuffer, &label);
}

void Diagnostics::EndLabel(VkCommandBuffer commandBuffer) const
{
  if(vkCmdEndDebugUtilsLabelEXT != nullptr)
  {
    vkCmdEndDebugUtilsLabelEXT(commandBuffer);
  }
}

uint64_t Diagnostics::ErrorCount() const noexcept { return m_ErrorCount.load(std::memory_order_relaxed); }
uint64_t Diagnostics::WarningCount() const noexcept { return m_WarningCount.load(std::memory_order_relaxed); }

VKAPI_ATTR VkBool32 VKAPI_CALL Diagnostics::DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData)
{
  // Counting
  // General messages, such as loader output, are printed but never counted, so the counts reflect only validation findings.

  auto& diagnostics = *static_cast<Diagnostics*>(userData);

  const bool validationMessage = (types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) != 0;

  if(validationMessage && (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
  {
    diagnostics.m_ErrorCount.fetch_add(1, std::memory_order_relaxed);
  }
  else if(validationMessage && (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
  {
    diagnostics.m_WarningCount.fetch_add(1, std::memory_order_relaxed);
  }

  // Printing
  // Returning VK_FALSE lets the Vulkan call that triggered the message continue normally.

  const char* level = !validationMessage ? "Vulkan" : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 ? "Validation Error" : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0 ? "Validation Warning" : "Validation";

  std::fprintf(stderr, "%s: %s\n", level, data != nullptr && data->pMessage != nullptr ? data->pMessage : "<no message>");

  return VK_FALSE;
}

CommandLabel::CommandLabel(const Diagnostics* diagnostics, VkCommandBuffer commandBuffer, const char* name, const float color[4])
  : m_Diagnostics(diagnostics), m_CommandBuffer(commandBuffer)
{
  if(m_Diagnostics != nullptr)
  {
    m_Diagnostics->BeginLabel(commandBuffer, name, color);
  }
}

CommandLabel::~CommandLabel()
{
  if(m_Diagnostics != nullptr)
  {
    m_Diagnostics->EndLabel(m_CommandBuffer);
  }
}

}  // namespace rtpt
