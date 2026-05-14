#include "Experiments/ExperimentGpuTimer.h"

#include <algorithm>

namespace nvsamples
{

void ExperimentGpuTimer::Initialize(VkPhysicalDevice physicalDevice, VkDevice device)
{
  m_Device = device;

  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(physicalDevice, &properties);
  m_TimestampPeriodNanoseconds = static_cast<double>(properties.limits.timestampPeriod);

  const VkQueryPoolCreateInfo queryPoolInfo{
      .sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType  = VK_QUERY_TYPE_TIMESTAMP,
      .queryCount = kFrameSlotCount * static_cast<uint32_t>(TimestampIndex::eCount),
  };
  vkCreateQueryPool(m_Device, &queryPoolInfo, nullptr, &m_QueryPool);
}

void ExperimentGpuTimer::Destroy()
{
  if(m_QueryPool != VK_NULL_HANDLE)
  {
    vkDestroyQueryPool(m_Device, m_QueryPool, nullptr);
  }

  m_Device                 = VK_NULL_HANDLE;
  m_QueryPool              = VK_NULL_HANDLE;
  m_LastCompletedFrameSlot = std::nullopt;
  m_FrameOpen              = false;
}

bool ExperimentGpuTimer::IsReady() const
{
  return m_Device != VK_NULL_HANDLE && m_QueryPool != VK_NULL_HANDLE && m_TimestampPeriodNanoseconds > 0.0;
}

void ExperimentGpuTimer::BeginFrame(VkCommandBuffer cmd)
{
  if(!IsReady() || cmd == VK_NULL_HANDLE)
  {
    return;
  }

  m_ActiveFrameSlot = m_NextFrameSlot;
  const uint32_t firstQuery = QueryIndex(m_ActiveFrameSlot, TimestampIndex::eFrameBegin);

  vkCmdResetQueryPool(cmd, m_QueryPool, firstQuery, static_cast<uint32_t>(TimestampIndex::eCount));
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_QueryPool,
                      QueryIndex(m_ActiveFrameSlot, TimestampIndex::eFrameBegin));
  m_FrameOpen = true;
}

void ExperimentGpuTimer::MarkRendererStart(VkCommandBuffer cmd)
{
  if(!m_FrameOpen || cmd == VK_NULL_HANDLE)
  {
    return;
  }

  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_QueryPool,
                      QueryIndex(m_ActiveFrameSlot, TimestampIndex::eRendererBegin));
}

void ExperimentGpuTimer::MarkRendererEnd(VkCommandBuffer cmd)
{
  if(!m_FrameOpen || cmd == VK_NULL_HANDLE)
  {
    return;
  }

  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_QueryPool,
                      QueryIndex(m_ActiveFrameSlot, TimestampIndex::eRendererEnd));
}

void ExperimentGpuTimer::EndFrame(VkCommandBuffer cmd)
{
  if(!m_FrameOpen || cmd == VK_NULL_HANDLE)
  {
    return;
  }

  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_QueryPool,
                      QueryIndex(m_ActiveFrameSlot, TimestampIndex::eFrameEnd));

  m_LastCompletedFrameSlot = m_ActiveFrameSlot;
  m_NextFrameSlot          = (m_NextFrameSlot + 1) % kFrameSlotCount;
  m_FrameOpen              = false;
}

std::optional<ExperimentGpuTimings> ExperimentGpuTimer::ReadLastFrameTimings() const
{
  if(!IsReady() || !m_LastCompletedFrameSlot)
  {
    return std::nullopt;
  }

  std::array<uint64_t, static_cast<size_t>(TimestampIndex::eCount)> timestamps{};
  const uint32_t firstQuery = QueryIndex(*m_LastCompletedFrameSlot, TimestampIndex::eFrameBegin);
  const VkResult result     = vkGetQueryPoolResults(m_Device, m_QueryPool, firstQuery,
                                                    static_cast<uint32_t>(timestamps.size()),
                                                    sizeof(uint64_t) * timestamps.size(), timestamps.data(),
                                                    sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
  if(result != VK_SUCCESS)
  {
    return std::nullopt;
  }

  ExperimentGpuTimings timings{};
  timings.frameMs       = TicksToMilliseconds(timestamps[TimestampIndex::eFrameBegin], timestamps[TimestampIndex::eFrameEnd]);
  timings.rendererMs    = TicksToMilliseconds(timestamps[TimestampIndex::eRendererBegin], timestamps[TimestampIndex::eRendererEnd]);
  timings.postProcessMs = TicksToMilliseconds(timestamps[TimestampIndex::eRendererEnd], timestamps[TimestampIndex::eFrameEnd]);
  return timings;
}

uint32_t ExperimentGpuTimer::QueryIndex(uint32_t frameSlot, TimestampIndex timestamp) const
{
  return frameSlot * static_cast<uint32_t>(TimestampIndex::eCount) + static_cast<uint32_t>(timestamp);
}

double ExperimentGpuTimer::TicksToMilliseconds(uint64_t start, uint64_t end) const
{
  const uint64_t elapsedTicks = end >= start ? end - start : 0;
  return static_cast<double>(elapsedTicks) * m_TimestampPeriodNanoseconds / 1'000'000.0;
}

}  // namespace nvsamples
