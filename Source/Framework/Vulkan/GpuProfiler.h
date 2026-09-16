#pragma once

#include "FrameContext.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <volk.h>

namespace rtpt
{

// GpuProfileScope
// Handle for an open timestamp scope, returned by BeginScope and passed back to EndScope.

struct GpuProfileScope
{
  // Frame slot whose queries the scope uses.
  FrameSlot slot {};
  // Scope index within the slot. UINT32_MAX marks an invalid scope.
  uint32_t  index = UINT32_MAX;
};

// GpuProfileResult
// Measured GPU duration of one named scope.

struct GpuProfileResult
{
  // Name given to BeginScope.
  std::string name;
  // Elapsed GPU time between the begin and end timestamps.
  double      milliseconds = 0.0;
};

// GpuProfiler
// Measures GPU time for named scopes with timestamp queries.
// Queries are partitioned by frame slot so reading a finished frame's results never touches queries an in-flight frame is still writing.
// Scopes may nest: each owns its own begin and end query, so an enclosing scope simply spans the timestamps of the scopes inside it.
// A slot's results live until its next BeginFrame forgets them, so they are read after the slot's previous submission completes and before the slot is reused.

class GpuProfiler
{
public:

  // Zone
  // Times the enclosing C++ scope: the constructor records the begin timestamp and the destructor the end timestamp.
  // A null profiler records nothing, so callers pass an optional profiler straight through and the timed commands are identical either way.

  class Zone
  {
  public:

    Zone(GpuProfiler* profiler, VkCommandBuffer commandBuffer, FrameSlot slot, std::string_view name);
    ~Zone();

    Zone(const Zone&)            = delete;
    Zone& operator=(const Zone&) = delete;

  private:

    // Profiler the scope was opened on. Null when profiling is off.
    GpuProfiler*    m_Profiler = nullptr;
    // Command buffer that receives the end timestamp.
    VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;
    // Scope opened by the constructor.
    GpuProfileScope m_Scope {};
  };

  GpuProfiler() = default;
  GpuProfiler(const GpuProfiler&)            = delete;
  GpuProfiler& operator=(const GpuProfiler&) = delete;
  ~GpuProfiler();

  // Returns VK_ERROR_FEATURE_NOT_PRESENT when the queue family writes no timestamp bits or the device reports no timestamp period, so the caller can run without profiling.
  VkResult Initialize(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamily, uint32_t frameSlotCount, uint32_t maxScopesPerFrame);
  void Destroy();

  [[nodiscard]] bool IsReady() const noexcept { return m_QueryPool != VK_NULL_HANDLE; }

  // Records a reset of the slot's queries and forgets its previous scopes. Call once per frame before any scope.
  void BeginFrame(VkCommandBuffer commandBuffer, FrameSlot slot);

  // Forgets the slot's scopes when its frame is cancelled. Those queries never reach the GPU, so reading them would wait forever.
  void DiscardFrame(FrameSlot slot);

  [[nodiscard]] GpuProfileScope BeginScope(VkCommandBuffer commandBuffer, FrameSlot slot, std::string_view name, VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
  void EndScope(VkCommandBuffer commandBuffer, GpuProfileScope scope, VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);

  // Blocks until the slot's timestamps are available, so the frame that recorded them must have been submitted. Every scope must be ended.
  [[nodiscard]] std::vector<GpuProfileResult> Read(FrameSlot slot) const;

private:

  // SlotState
  // Scope bookkeeping for one frame slot, indexed by scope index.

  struct SlotState
  {
    // Scope names in the order they began.
    std::vector<std::string> names;
    // Whether each scope has recorded its end timestamp.
    std::vector<bool>        ended;
  };

  // Endpoint 0 is the begin timestamp and 1 is the end timestamp.
  [[nodiscard]] uint32_t QueryIndex(FrameSlot slot, uint32_t scopeIndex, uint32_t endpoint) const;
  [[nodiscard]] SlotState& GetSlot(FrameSlot slot);
  [[nodiscard]] const SlotState& GetSlot(FrameSlot slot) const;

  // Device that owns the query pool.
  VkDevice               m_Device = VK_NULL_HANDLE;
  // Two timestamp queries per scope per slot.
  VkQueryPool            m_QueryPool = VK_NULL_HANDLE;
  // Nanoseconds per timestamp tick, from the device limits.
  double                 m_TimestampPeriodNanoseconds = 0.0;
  // Bits the queue family actually writes. Timestamps wrap at this width, so elapsed ticks are masked to it.
  uint64_t               m_TimestampMask = 0;
  // Scope capacity of each slot.
  uint32_t               m_MaxScopes = 0;
  // Per-slot scope bookkeeping.
  std::vector<SlotState> m_Slots;
};

}  // namespace rtpt
