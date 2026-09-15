#pragma once

#include <initializer_list>
#include <span>
#include <vector>

#include <volk.h>

namespace rtpt
{

// DescriptorBindings
// The declared bindings of one descriptor set layout, kept with their per-binding flags.
// Vulkan takes binding flags as a separate array index-aligned with the bindings, so both are stored in lockstep.
// The declaration also lets MakeWrite fill in and range-check the descriptor type and count for a write.

class DescriptorBindings
{
public:

  // Throws if the binding number is already declared.
  void Add(uint32_t binding, VkDescriptorType type, uint32_t count, VkShaderStageFlags stages, VkDescriptorBindingFlags flags = 0, const VkSampler* immutableSamplers = nullptr);
  void Add(const VkDescriptorSetLayoutBinding& binding, VkDescriptorBindingFlags flags = 0);

  // Returns a write with the target, type, and count filled in; the caller attaches the image, buffer, or acceleration-structure payload. Throws if the range falls outside the declared binding.
  [[nodiscard]] VkWriteDescriptorSet MakeWrite(uint32_t binding, VkDescriptorSet set, uint32_t arrayElement = 0, uint32_t descriptorCount = 1) const;
  [[nodiscard]] const std::vector<VkDescriptorSetLayoutBinding>& LayoutBindings() const noexcept;
  [[nodiscard]] const std::vector<VkDescriptorBindingFlags>& BindingFlags() const noexcept;

private:

  // Declared bindings in insertion order.
  std::vector<VkDescriptorSetLayoutBinding> m_Bindings;
  // Binding flags, index-aligned with m_Bindings.
  std::vector<VkDescriptorBindingFlags>      m_Flags;
};

// DescriptorPack
// Owns a descriptor set layout together with a pool sized for exactly its sets and the sets themselves.
// A setCount of zero with the push-descriptor layout flag creates only the layout, for passes that push their descriptors each dispatch.
// Renderers allocate one set per frame slot so each in-flight frame keeps its own descriptors.

class DescriptorPack
{
public:

  DescriptorPack() = default;
  DescriptorPack(const DescriptorPack&)            = delete;
  DescriptorPack& operator=(const DescriptorPack&) = delete;
  DescriptorPack(DescriptorPack&& other) noexcept;
  DescriptorPack& operator=(DescriptorPack&& other) noexcept;
  ~DescriptorPack();

  // Push-descriptor layouts require setCount == 0 and every other layout requires setCount > 0. Variable descriptor counts, when given, need one entry per set.
  VkResult Initialize(VkDevice device, const DescriptorBindings& bindings, uint32_t setCount = 1, VkDescriptorSetLayoutCreateFlags layoutFlags = 0, VkDescriptorPoolCreateFlags poolFlags = 0, std::span<const uint32_t> variableDescriptorCounts = {});
  void Destroy();

  // For push-descriptor packs only set index 0 is valid, and the write's dstSet is left null.
  [[nodiscard]] VkWriteDescriptorSet MakeWrite(uint32_t binding, uint32_t setIndex = 0, uint32_t arrayElement = 0, uint32_t descriptorCount = 1) const;
  [[nodiscard]] VkDescriptorSetLayout Layout() const noexcept { return m_Layout; }
  [[nodiscard]] const VkDescriptorSetLayout* LayoutPtr() const noexcept { return &m_Layout; }
  [[nodiscard]] const std::vector<VkDescriptorSet>& Sets() const noexcept { return m_Sets; }
  [[nodiscard]] VkDescriptorSet Set(uint32_t index = 0) const { return m_Sets.at(index); }
  [[nodiscard]] const VkDescriptorSet* SetPtr(uint32_t index = 0) const { return &m_Sets.at(index); }

private:

  // Copy of the declaration, used to build writes after initialization.
  DescriptorBindings           m_Bindings;
  // Device that owns the layout and pool.
  VkDevice                     m_Device = VK_NULL_HANDLE;
  // Layout shared by every set in the pack.
  VkDescriptorSetLayout        m_Layout = VK_NULL_HANDLE;
  // Pool sized for setCount copies of the bindings. Null for push-descriptor packs.
  VkDescriptorPool             m_Pool = VK_NULL_HANDLE;
  // Allocated sets. Freed implicitly when the pool is destroyed.
  std::vector<VkDescriptorSet> m_Sets;
  // True when the layout was created with the push-descriptor flag.
  bool                         m_PushDescriptor = false;
};

VkResult CreatePipelineLayout(VkDevice device, VkPipelineLayout& layout, std::span<const VkDescriptorSetLayout> setLayouts, std::span<const VkPushConstantRange> pushConstantRanges);

}  // namespace rtpt
