#include "GpuProfiler.h"

#include "Diagnostics.h"

#include <limits>
#include <stdexcept>

namespace rtpt
{

GpuProfiler::~GpuProfiler()
{
  Destroy();
}

VkResult GpuProfiler::Initialize(VkDevice device, float timestampPeriodNanoseconds, uint32_t frameSlotCount, uint32_t maxScopesPerFrame)
{
  // Each scope needs a begin and an end query in every slot. The count is computed in 64 bits so an oversized request is rejected instead of wrapping.
  const uint64_t queryCount = uint64_t { frameSlotCount } * maxScopesPerFrame * 2;

  if(m_Device != VK_NULL_HANDLE || device == VK_NULL_HANDLE || timestampPeriodNanoseconds <= 0.0F || frameSlotCount == 0 || maxScopesPerFrame == 0 || queryCount > std::numeric_limits<uint32_t>::max())
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkQueryPoolCreateInfo createInfo {
    .sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
    .queryType  = VK_QUERY_TYPE_TIMESTAMP,
    .queryCount = static_cast<uint32_t>(queryCount),
  };

  const VkResult result = vkCreateQueryPool(device, &createInfo, nullptr, &m_QueryPool);

  if(result == VK_SUCCESS)
  {
    m_Device                     = device;
    m_TimestampPeriodNanoseconds = timestampPeriodNanoseconds;
    m_MaxScopes                  = maxScopesPerFrame;
    m_Slots.resize(frameSlotCount);
  }

  return result;
}

void GpuProfiler::Destroy()
{
  if(m_QueryPool != VK_NULL_HANDLE)
  {
    vkDestroyQueryPool(m_Device, m_QueryPool, nullptr);
  }

  m_QueryPool                  = VK_NULL_HANDLE;
  m_Device                     = VK_NULL_HANDLE;
  m_TimestampPeriodNanoseconds = 0.0;
  m_MaxScopes                  = 0;
  m_Slots.clear();
}

void GpuProfiler::BeginFrame(VkCommandBuffer commandBuffer, FrameSlot slot)
{
  if(commandBuffer == VK_NULL_HANDLE)
  {
    throw std::invalid_argument("GPU profiler command buffer is null");
  }

  SlotState& state = GetSlot(slot);
  state.names.clear();
  state.ended.clear();

  // The reset is recorded rather than done from the host, so it only takes effect once the slot's previous frame has been replaced on the GPU.
  vkCmdResetQueryPool(commandBuffer, m_QueryPool, QueryIndex(slot, 0, 0), m_MaxScopes * 2);
}

GpuProfileScope GpuProfiler::BeginScope(VkCommandBuffer commandBuffer, FrameSlot slot, std::string_view name, VkPipelineStageFlags2 stage)
{
  if(commandBuffer == VK_NULL_HANDLE)
  {
    throw std::invalid_argument("GPU profiler command buffer is null");
  }

  SlotState& state = GetSlot(slot);

  if(state.names.size() >= m_MaxScopes || name.empty())
  {
    throw std::out_of_range("GPU profile scope capacity exceeded or name is empty");
  }

  const uint32_t index = static_cast<uint32_t>(state.names.size());
  state.names.emplace_back(name);
  state.ended.push_back(false);

  vkCmdWriteTimestamp2(commandBuffer, stage, m_QueryPool, QueryIndex(slot, index, 0));

  return { .slot = slot, .index = index };
}

void GpuProfiler::EndScope(VkCommandBuffer commandBuffer, GpuProfileScope scope, VkPipelineStageFlags2 stage)
{
  if(commandBuffer == VK_NULL_HANDLE)
  {
    throw std::invalid_argument("GPU profiler command buffer is null");
  }

  SlotState& state = GetSlot(scope.slot);

  if(scope.index >= state.names.size() || state.ended[scope.index])
  {
    throw std::logic_error("GPU profile scope is invalid or already ended");
  }

  vkCmdWriteTimestamp2(commandBuffer, stage, m_QueryPool, QueryIndex(scope.slot, scope.index, 1));
  state.ended[scope.index] = true;
}

std::vector<GpuProfileResult> GpuProfiler::Read(FrameSlot slot) const
{
  const SlotState& state = GetSlot(slot);

  if(state.names.empty())
  {
    return {};
  }

  // An open scope has no end timestamp, so its query would never become available and the wait below would not return.
  for(bool ended : state.ended)
  {
    if(!ended)
    {
      throw std::logic_error("GPU profile results requested with an open scope");
    }
  }

  // Query results
  // The wait flag blocks until the GPU has written every timestamp. Results arrive as begin, end pairs in scope order.

  std::vector<uint64_t> timestamps(state.names.size() * 2);
  CheckVk(vkGetQueryPoolResults(m_Device, m_QueryPool, QueryIndex(slot, 0, 0), static_cast<uint32_t>(timestamps.size()), timestamps.size() * sizeof(uint64_t), timestamps.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "vkGetQueryPoolResults(GPU profiler)");

  // Conversion
  // Timestamp ticks are scaled by the device's timestamp period to nanoseconds, then to milliseconds.

  std::vector<GpuProfileResult> results;
  results.reserve(state.names.size());

  for(size_t index = 0; index < state.names.size(); ++index)
  {
    const uint64_t elapsedTicks = timestamps[index * 2 + 1] - timestamps[index * 2];
    results.push_back({ .name = state.names[index], .milliseconds = static_cast<double>(elapsedTicks) * m_TimestampPeriodNanoseconds / 1'000'000.0 });
  }

  return results;
}

uint32_t GpuProfiler::QueryIndex(FrameSlot slot, uint32_t scopeIndex, uint32_t endpoint) const
{
  // Slots occupy consecutive blocks of m_MaxScopes scopes, and each scope occupies two queries.
  return (slot.index * m_MaxScopes + scopeIndex) * 2 + endpoint;
}

GpuProfiler::SlotState& GpuProfiler::GetSlot(FrameSlot slot)
{
  if(m_Device == VK_NULL_HANDLE || slot.index >= m_Slots.size())
  {
    throw std::out_of_range("GPU profiler frame slot is unavailable");
  }

  return m_Slots[slot.index];
}

const GpuProfiler::SlotState& GpuProfiler::GetSlot(FrameSlot slot) const
{
  if(m_Device == VK_NULL_HANDLE || slot.index >= m_Slots.size())
  {
    throw std::out_of_range("GPU profiler frame slot is unavailable");
  }

  return m_Slots[slot.index];
}

}  // namespace rtpt
