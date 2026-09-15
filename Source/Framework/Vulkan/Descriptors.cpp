#include "Descriptors.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace rtpt
{

void DescriptorBindings::Add(uint32_t binding, VkDescriptorType type, uint32_t count, VkShaderStageFlags stages, VkDescriptorBindingFlags flags, const VkSampler* immutableSamplers)
{
  Add(VkDescriptorSetLayoutBinding { .binding = binding, .descriptorType = type, .descriptorCount = count, .stageFlags = stages, .pImmutableSamplers = immutableSamplers }, flags);
}

void DescriptorBindings::Add(const VkDescriptorSetLayoutBinding& binding, VkDescriptorBindingFlags flags)
{
  // Vulkan rejects a layout that declares the same binding number twice; failing here names the binding.
  const auto duplicate = std::ranges::find(m_Bindings, binding.binding, &VkDescriptorSetLayoutBinding::binding);

  if(duplicate != m_Bindings.end())
  {
    throw std::runtime_error("duplicate descriptor binding " + std::to_string(binding.binding));
  }

  m_Bindings.push_back(binding);
  m_Flags.push_back(flags);
}

VkWriteDescriptorSet DescriptorBindings::MakeWrite(uint32_t binding, VkDescriptorSet set, uint32_t arrayElement, uint32_t descriptorCount) const
{
  const auto found = std::ranges::find(m_Bindings, binding, &VkDescriptorSetLayoutBinding::binding);

  // The write must name a declared binding and stay inside its descriptor array.
  if(found == m_Bindings.end() || descriptorCount == 0 || arrayElement + descriptorCount > found->descriptorCount)
  {
    throw std::runtime_error("descriptor write is outside its declared binding");
  }

  return VkWriteDescriptorSet { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = binding, .dstArrayElement = arrayElement, .descriptorCount = descriptorCount, .descriptorType = found->descriptorType };
}

const std::vector<VkDescriptorSetLayoutBinding>& DescriptorBindings::LayoutBindings() const noexcept { return m_Bindings; }
const std::vector<VkDescriptorBindingFlags>& DescriptorBindings::BindingFlags() const noexcept { return m_Flags; }

DescriptorPack::DescriptorPack(DescriptorPack&& other) noexcept
{
  *this = std::move(other);
}

DescriptorPack& DescriptorPack::operator=(DescriptorPack&& other) noexcept
{
  // Releases this pack's objects before taking over the other's, leaving the source empty.
  if(this != &other)
  {
    Destroy();

    m_Bindings       = std::move(other.m_Bindings);
    m_Device         = std::exchange(other.m_Device, VK_NULL_HANDLE);
    m_Layout         = std::exchange(other.m_Layout, VK_NULL_HANDLE);
    m_Pool           = std::exchange(other.m_Pool, VK_NULL_HANDLE);
    m_Sets           = std::move(other.m_Sets);
    m_PushDescriptor = std::exchange(other.m_PushDescriptor, false);
  }

  return *this;
}

DescriptorPack::~DescriptorPack()
{
  Destroy();
}

VkResult DescriptorPack::Initialize(VkDevice device, const DescriptorBindings& bindings, uint32_t setCount, VkDescriptorSetLayoutCreateFlags layoutFlags, VkDescriptorPoolCreateFlags poolFlags, std::span<const uint32_t> variableDescriptorCounts)
{
  // Arguments
  // Push-descriptor layouts are never allocated from a pool, so they must ask for zero sets, and every other layout must ask for at least one.
  // Variable descriptor counts are given per set, so their count must match.

  Destroy();

  const bool pushDescriptor = (layoutFlags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0;

  if(device == VK_NULL_HANDLE || pushDescriptor != (setCount == 0) || (!variableDescriptorCounts.empty() && variableDescriptorCounts.size() != setCount))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  m_Device         = device;
  m_Bindings       = bindings;
  m_PushDescriptor = pushDescriptor;

  // Layout
  // Binding flags ride along in a pNext structure whose array is index-aligned with the bindings. A push-descriptor pack stops here.

  const auto& flags = bindings.BindingFlags();

  const VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo {
    .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
    .bindingCount  = static_cast<uint32_t>(flags.size()),
    .pBindingFlags = flags.data(),
  };

  const VkDescriptorSetLayoutCreateInfo layoutInfo {
    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .pNext        = flags.empty() ? nullptr : &flagsInfo,
    .flags        = layoutFlags,
    .bindingCount = static_cast<uint32_t>(bindings.LayoutBindings().size()),
    .pBindings    = bindings.LayoutBindings().data(),
  };

  VkResult result = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_Layout);

  if(result != VK_SUCCESS || setCount == 0)
  {
    return result;
  }

  // Pool
  // Sized for exactly setCount copies of every binding, using each binding's declared count even when a variable count will allocate fewer.

  std::map<VkDescriptorType, uint32_t> counts;
  for(const auto& binding : bindings.LayoutBindings())
  {
    counts[binding.descriptorType] += binding.descriptorCount * setCount;
  }

  std::vector<VkDescriptorPoolSize> poolSizes;
  poolSizes.reserve(counts.size());
  for(const auto& [type, count] : counts)
  {
    poolSizes.push_back({ .type = type, .descriptorCount = count });
  }

  const VkDescriptorPoolCreateInfo poolInfo { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = poolFlags, .maxSets = setCount, .poolSizeCount = static_cast<uint32_t>(poolSizes.size()), .pPoolSizes = poolSizes.data() };

  result = vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_Pool);

  if(result != VK_SUCCESS)
  {
    Destroy();
    return result;
  }

  // Sets
  // All sets share the one layout. Variable descriptor counts are chained in only when the caller supplied them.

  std::vector<VkDescriptorSetLayout> layouts(setCount, m_Layout);

  const VkDescriptorSetVariableDescriptorCountAllocateInfo variableInfo {
    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
    .descriptorSetCount = static_cast<uint32_t>(variableDescriptorCounts.size()),
    .pDescriptorCounts  = variableDescriptorCounts.data(),
  };

  const VkDescriptorSetAllocateInfo allocateInfo {
    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .pNext              = variableDescriptorCounts.empty() ? nullptr : &variableInfo,
    .descriptorPool     = m_Pool,
    .descriptorSetCount = setCount,
    .pSetLayouts        = layouts.data(),
  };

  m_Sets.resize(setCount);
  result = vkAllocateDescriptorSets(device, &allocateInfo, m_Sets.data());

  if(result != VK_SUCCESS)
  {
    Destroy();
  }

  return result;
}

void DescriptorPack::Destroy()
{
  // Destroying the pool frees its sets, so the set handles are only forgotten.
  if(m_Device != VK_NULL_HANDLE)
  {
    if(m_Pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_Device, m_Pool, nullptr);
    if(m_Layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_Device, m_Layout, nullptr);
  }

  m_Sets.clear();

  m_Bindings       = {};
  m_Pool           = VK_NULL_HANDLE;
  m_Layout         = VK_NULL_HANDLE;
  m_Device         = VK_NULL_HANDLE;
  m_PushDescriptor = false;
}

VkWriteDescriptorSet DescriptorPack::MakeWrite(uint32_t binding, uint32_t setIndex, uint32_t arrayElement, uint32_t descriptorCount) const
{
  // Pushed descriptors have no set object; vkCmdPushDescriptorSet ignores dstSet, so it stays null.
  if(m_PushDescriptor)
  {
    if(setIndex != 0)
    {
      throw std::out_of_range("push descriptor packs expose only logical set index 0");
    }
    return m_Bindings.MakeWrite(binding, VK_NULL_HANDLE, arrayElement, descriptorCount);
  }

  return m_Bindings.MakeWrite(binding, Set(setIndex), arrayElement, descriptorCount);
}

VkResult CreatePipelineLayout(VkDevice device, VkPipelineLayout& layout, std::span<const VkDescriptorSetLayout> setLayouts, std::span<const VkPushConstantRange> pushConstantRanges)
{
  const VkPipelineLayoutCreateInfo createInfo {
    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount         = static_cast<uint32_t>(setLayouts.size()),
    .pSetLayouts            = setLayouts.data(),
    .pushConstantRangeCount = static_cast<uint32_t>(pushConstantRanges.size()),
    .pPushConstantRanges    = pushConstantRanges.data(),
  };

  return vkCreatePipelineLayout(device, &createInfo, nullptr, &layout);
}

}  // namespace rtpt
