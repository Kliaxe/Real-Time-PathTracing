#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include <volk/volk.h>

#include "Experiments/ExperimentTypes.h"

namespace nvsamples
{

class ExperimentGpuTimer
{
public:
  void Initialize(VkPhysicalDevice physicalDevice, VkDevice device);
  void Destroy();

  bool IsReady() const;

  void BeginFrame(VkCommandBuffer cmd);
  void MarkRendererStart(VkCommandBuffer cmd);
  void MarkRendererEnd(VkCommandBuffer cmd);
  void EndFrame(VkCommandBuffer cmd);

  std::optional<ExperimentGpuTimings> ReadLastFrameTimings() const;

private:
  enum TimestampIndex : uint32_t
  {
    eFrameBegin = 0,
    eRendererBegin,
    eRendererEnd,
    eFrameEnd,
    eCount,
  };

  static constexpr uint32_t kFrameSlotCount = 8;

  uint32_t QueryIndex(uint32_t frameSlot, TimestampIndex timestamp) const;
  double   TicksToMilliseconds(uint64_t start, uint64_t end) const;

  VkDevice          m_Device    = VK_NULL_HANDLE;
  VkQueryPool       m_QueryPool = VK_NULL_HANDLE;
  double            m_TimestampPeriodNanoseconds = 0.0;
  uint32_t          m_NextFrameSlot              = 0;
  uint32_t          m_ActiveFrameSlot            = 0;
  bool              m_FrameOpen                  = false;
  std::optional<uint32_t> m_LastCompletedFrameSlot;
};

}  // namespace nvsamples
