#pragma once

#include <cstdint>

#include <volk.h>

namespace rtpt
{

// CompletionPoint
// A value on GpuExecution's timeline semaphore. The GPU has finished a submission once the semaphore reaches its value.
// Zero means nothing was submitted, so waiting on it returns immediately.

struct CompletionPoint
{
  // Timeline value signalled by the submission.
  uint64_t value = 0;

  [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
};

// FrameSlot
// Index of the rotating per-frame resources a frame uses: command pool, descriptor sets, profiler queries, and swapchain acquire semaphore.
// Slots let several frames be in flight at once without sharing mutable GPU state.

struct FrameSlot
{
  // Slot index, less than GpuExecution::FrameSlotCount().
  uint32_t index = 0;
};

// FrameContext
// What recording code needs for the frame that GpuExecution::BeginFrame opened.

struct FrameContext
{
  // Frame command buffer, already in the recording state.
  VkCommandBuffer commands = VK_NULL_HANDLE;
  // Slot whose per-frame resources this frame may use.
  FrameSlot       slot {};
};

}  // namespace rtpt
