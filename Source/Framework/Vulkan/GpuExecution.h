#pragma once

#include "FrameContext.h"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include <volk.h>

namespace rtpt
{

// SemaphoreWait
// A semaphore a submission waits on before executing, such as the swapchain's image-available semaphore.

struct SemaphoreWait
{
  // Semaphore to wait on.
  VkSemaphore          semaphore = VK_NULL_HANDLE;
  // Stages that must not start until the semaphore is signalled.
  VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  // Timeline value to wait for. Ignored for binary semaphores.
  uint64_t             value = 0;
};

// SemaphoreSignal
// A semaphore a submission signals on completion, such as the per-image render-finished semaphore that presentation waits on.

struct SemaphoreSignal
{
  // Semaphore to signal.
  VkSemaphore          semaphore = VK_NULL_HANDLE;
  // Stages whose completion signals the semaphore.
  VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  // Timeline value to signal. Ignored for binary semaphores.
  uint64_t             value = 0;
};

// SubmissionSync
// Extra semaphores for one submission, on top of the timeline signal GpuExecution always adds.

struct SubmissionSync
{
  // Semaphores to wait on.
  std::span<const SemaphoreWait>   waits {};
  // Semaphores to signal.
  std::span<const SemaphoreSignal> signals {};
};

// GpuExecution
// Owns submission to the render queue and tracks GPU progress with one timeline semaphore.
// Every submission signals the next timeline value, which becomes its CompletionPoint, so any code can wait for or poll a specific submission.
// Frames rotate through a fixed number of slots, each with its own command pool. BeginFrame waits for the slot's previous frame, which caps how many frames are in flight.
// Resource destruction is deferred through Retire until the GPU has passed every submission that could still reference the resource.

class GpuExecution
{
public:

  GpuExecution() = default;
  GpuExecution(const GpuExecution&)            = delete;
  GpuExecution& operator=(const GpuExecution&) = delete;

  void Initialize(VkDevice device, VkQueue queue, uint32_t queueFamily, uint32_t frameSlotCount);

  // Cancels an open frame, then waits for all submitted work and runs every retirement before releasing the queue objects.
  void Destroy();

  // Waits for the next slot's previous frame, resets its pool, and returns its command buffer ready for recording.
  [[nodiscard]] FrameContext BeginFrame();
  [[nodiscard]] CompletionPoint SubmitFrame(const SubmissionSync& synchronization = {});

  // Discards the open frame's commands without submitting them.
  void CancelFrame();

  // Records, submits, and waits for a one-shot command buffer. Not allowed while a frame is open.
  [[nodiscard]] CompletionPoint ExecuteAndWait(const std::function<void(VkCommandBuffer)>& record);

  // Submits no commands, only semaphore waits and signals, and waits for it. Used to consume a signalled semaphore that no frame will wait on.
  [[nodiscard]] CompletionPoint SubmitSynchronizationAndWait(const SubmissionSync& synchronization);

  [[nodiscard]] bool IsComplete(CompletionPoint point) const;
  void Wait(CompletionPoint point) const;

  // Runs every retirement whose completion point the GPU has reached.
  void CollectRetiredResources();

  // Waits for the last submission and then collects retirements.
  void Drain();

  // Defers a release until the GPU passes the last submission. While a frame is open, the release instead waits for that frame's submission, since its commands may use the resource.
  void Retire(std::function<void()> release);

  // Defers a release until an explicit, already published completion point. Not allowed while a frame is open.
  void Retire(CompletionPoint completion, std::function<void()> release);

  [[nodiscard]] bool HasActiveFrame() const noexcept { return m_FrameActive; }
  [[nodiscard]] uint32_t FrameSlotCount() const noexcept { return static_cast<uint32_t>(m_Slots.size()); }
  [[nodiscard]] CompletionPoint LastSubmission() const noexcept { return CompletionPoint { m_LastSubmittedValue }; }
  [[nodiscard]] VkQueue Queue() const noexcept { return m_Queue; }
  [[nodiscard]] uint32_t QueueFamily() const noexcept { return m_QueueFamily; }

private:

  // Slot
  // The recording resources owned by one frame slot.

  struct Slot
  {
    // Pool reset wholesale at the start of each frame on this slot.
    VkCommandPool    commandPool = VK_NULL_HANDLE;
    // The slot's single frame command buffer.
    VkCommandBuffer  commandBuffer = VK_NULL_HANDLE;
    // The last frame submitted from this slot. Its pool cannot be reset before it completes.
    CompletionPoint  completion {};
  };

  // Retirement
  // A deferred release waiting for the GPU to reach its completion point.

  struct Retirement
  {
    // Timeline value the GPU must reach before release runs.
    CompletionPoint         completion {};
    // Destroys the retired resource.
    std::function<void()>   release;
  };

  [[nodiscard]] CompletionPoint Submit(VkCommandBuffer commandBuffer, const SubmissionSync& synchronization);

  // Attaches releases queued during an open frame to the completion point they must wait for.
  void PublishPendingRetirements(CompletionPoint completion);

  // Logical device.
  VkDevice                m_Device = VK_NULL_HANDLE;
  // Render queue every submission goes to.
  VkQueue                 m_Queue = VK_NULL_HANDLE;
  // Family of m_Queue, used to create command pools.
  uint32_t                m_QueueFamily = 0;
  // Timeline semaphore signalled by every submission.
  VkSemaphore             m_Timeline = VK_NULL_HANDLE;
  // Per-slot recording resources.
  std::vector<Slot>       m_Slots;
  // Releases with a known completion point.
  std::vector<Retirement> m_Retirements;
  // Releases queued while a frame was open, waiting for that frame's completion point.
  std::vector<std::function<void()>> m_PendingRetirements;
  // Slot the next BeginFrame will use.
  uint32_t                m_NextSlot = 0;
  // Slot of the open frame. Meaningful only while m_FrameActive.
  uint32_t                m_ActiveSlot = 0;
  // Timeline value of the most recent successful submission.
  uint64_t                m_LastSubmittedValue = 0;
  // True between BeginFrame and SubmitFrame or CancelFrame.
  bool                    m_FrameActive = false;
};

}  // namespace rtpt
