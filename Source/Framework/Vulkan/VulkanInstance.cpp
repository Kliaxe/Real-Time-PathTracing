#include "VulkanInstance.h"

#include "Framework/Platform/Log.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

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

const VkLayerProperties* FindLayer(std::span<const VkLayerProperties> available, const char* name)
{
  const auto found = std::ranges::find_if(available, [name](const VkLayerProperties& layer) {
    return std::string_view(layer.layerName) == name;
  });

  return found == available.end() ? nullptr : &*found;
}
}  // namespace

VulkanInstance::~VulkanInstance()
{
  Destroy();
}

void VulkanInstance::Initialize(const VulkanInstanceCreateInfo& createInfo)
{
  if(m_Instance != VK_NULL_HANDLE)
  {
    throw std::logic_error("VulkanInstance is already initialized");
  }

  if(createInfo.synchronizationValidation && !createInfo.validation)
  {
    throw std::invalid_argument("synchronization validation requires validation");
  }

  // Loader
  // volk must load the Vulkan loader before any Vulkan call. The loader must support 1.3, and the instance then targets exactly 1.3 even when a newer version is available.
  // A custom entry point replaces the system loader lookup; every later volk table is then filled through it.

  if(createInfo.loaderEntryPoint != nullptr)
  {
    volkInitializeCustom(createInfo.loaderEntryPoint);
  }
  else
  {
    CheckVk(volkInitialize(), "volkInitialize");
  }

  CheckVk(vkEnumerateInstanceVersion(&m_ApiVersion), "vkEnumerateInstanceVersion");

  if(m_ApiVersion < VK_API_VERSION_1_3)
  {
    throw std::runtime_error("Vulkan 1.3 or newer is required");
  }

  m_ApiVersion = VK_API_VERSION_1_3;

  // Extensions
  // Validation adds debug utils for the messenger. Every requested extension is checked up front so a missing one produces a named error instead of a bare VK_ERROR_EXTENSION_NOT_PRESENT.

  uint32_t extensionCount = 0;
  CheckVk(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr), "vkEnumerateInstanceExtensionProperties(count)");

  std::vector<VkExtensionProperties> availableExtensions(extensionCount);
  CheckVk(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, availableExtensions.data()), "vkEnumerateInstanceExtensionProperties");

  std::vector<const char*> extensions(createInfo.requiredExtensions.begin(), createInfo.requiredExtensions.end());

  if(createInfo.validation)
  {
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }

  for(const char* extension : extensions)
  {
    if(!HasExtension(availableExtensions, extension))
    {
      throw std::runtime_error(std::string("required Vulkan instance extension is unavailable: ") + extension);
    }
  }

  // Validation layer
  // The Khronos layer must be installed when validation is requested.
  // When the window requested KHR surface maintenance, the layer must be 1.4.341 or newer to handle KHR surface/swapchain maintenance; otherwise the user is told to select a compatible SDK with VK_LAYER_PATH.

  std::vector<const char*> layers;

  if(createInfo.validation)
  {
    uint32_t layerCount = 0;
    CheckVk(vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "vkEnumerateInstanceLayerProperties(count)");

    std::vector<VkLayerProperties> availableLayers(layerCount);
    CheckVk(vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data()), "vkEnumerateInstanceLayerProperties");

    constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
    const VkLayerProperties* validationLayer = FindLayer(availableLayers, kValidationLayer);

    if(validationLayer == nullptr)
    {
      throw std::runtime_error("VK_LAYER_KHRONOS_validation is unavailable");
    }

    const bool usesSurfaceMaintenance = std::ranges::any_of(createInfo.requiredExtensions, [](const char* extension) {
      return std::string_view(extension) == VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
    });

    constexpr uint32_t kMinimumMaintenanceValidationVersion = VK_MAKE_API_VERSION(0, 1, 4, 341);

    if(usesSurfaceMaintenance && validationLayer->specVersion < kMinimumMaintenanceValidationVersion)
    {
      throw std::runtime_error("VK_LAYER_KHRONOS_validation 1.4.341 or newer is required for KHR surface/swapchain maintenance; " "select a compatible SDK with VK_LAYER_PATH");
    }

    layers.push_back(kValidationLayer);
  }

  // Create info chain
  // The messenger create info is chained into instance creation so messages from vkCreateInstance itself are reported, and synchronization validation is chained behind it when requested.
  // Counts are reset first so they only reflect this instance.

  const VkValidationFeatureEnableEXT synchronizationFeature = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;

  const VkValidationFeaturesEXT validationFeatures {
    .sType                          = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
    .enabledValidationFeatureCount  = createInfo.synchronizationValidation ? 1u : 0u,
    .pEnabledValidationFeatures     = createInfo.synchronizationValidation ? &synchronizationFeature : nullptr,
  };

  m_Diagnostics.ResetCounts();

  VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo = m_Diagnostics.CreateInfo();
  debugCreateInfo.pNext = createInfo.synchronizationValidation ? &validationFeatures : nullptr;

  // Vulkan needs a null-terminated application name, which a string_view does not guarantee.
  const std::string applicationName(createInfo.applicationName);

  const VkApplicationInfo applicationInfo {
    .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName   = applicationName.c_str(),
    .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
    .pEngineName        = "RTPT",
    .engineVersion      = VK_MAKE_API_VERSION(0, 0, 1, 0),
    .apiVersion         = m_ApiVersion,
  };

  const VkInstanceCreateInfo instanceInfo {
    .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pNext                   = createInfo.validation ? &debugCreateInfo : nullptr,
    .pApplicationInfo        = &applicationInfo,
    .enabledLayerCount       = static_cast<uint32_t>(layers.size()),
    .ppEnabledLayerNames     = layers.data(),
    .enabledExtensionCount   = static_cast<uint32_t>(extensions.size()),
    .ppEnabledExtensionNames = extensions.data(),
  };

  // Instance
  // volkLoadInstance fills in the instance-level function pointers. The persistent messenger is created once the instance exists.

  CheckVk(vkCreateInstance(&instanceInfo, nullptr, &m_Instance), "vkCreateInstance");
  volkLoadInstance(m_Instance);

  if(createInfo.validation)
  {
    m_Diagnostics.Initialize(m_Instance);
    Log(LogLevel::Info, createInfo.synchronizationValidation ? "Validation layer enabled with synchronization validation" : "Validation layer enabled");
  }
}

void VulkanInstance::Destroy()
{
  if(m_Instance == VK_NULL_HANDLE)
  {
    return;
  }

  // The messenger belongs to the instance and must go first.
  m_Diagnostics.Destroy();
  vkDestroyInstance(m_Instance, nullptr);

  m_Instance   = VK_NULL_HANDLE;
  m_ApiVersion = 0;
}

}  // namespace rtpt
