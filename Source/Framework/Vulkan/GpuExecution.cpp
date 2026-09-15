#include "GpuExecution.h"

#include "Diagnostics.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace rtpt
{

void GpuExecution::Initialize(VkDevice device, VkQueue queue, uint32_t queueFamily, uint32_t frameSlotCount)
{
  if(m_Device != VK_NULL_HANDLE || device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE || frameSlotCount == 0)
  {
    throw std::invalid_argument("invalid GpuExecution initialization");
  }

  m_Device      = device;
  m_Queue       = queue;
  m_QueueFamily = queueFamily;
  m_Slots.resize(frameSlotCount);

  // Timeline and slots
  // The timeline semaphore starts at zero, which is also the "nothing submitted" completion point.
  // Each slot gets its own pool so a slot can be reset wholesale while other slots' frames are still executing.
  // Any failure tears down whatever was already created before rethrowing.

  try
  {
    const VkSemaphoreTypeCreateInfo timelineType {
      .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
      .initialValue  = 0,
    };

    const VkSemaphoreCreateInfo semaphoreInfo {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &timelineType,
    };

    CheckVk(vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &m_Timeline), "vkCreateSemaphore(render timeline)");

    for(Slot& slot : m_Slots)
    {
      const VkCommandPoolCreateInfo poolInfo {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = m_QueueFamily,
      };

      CheckVk(vkCreateCommandPool(m_Device, &poolInfo, nullptr, &slot.commandPool), "vkCreateCommandPool(frame)");

      const VkCommandBufferAllocateInfo allocateInfo {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = slot.commandPool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
      };

      CheckVk(vkAllocateCommandBuffers(m_Device, &allocateInfo, &slot.commandBuffer), "vkAllocateCommandBuffers(frame)");
    }
  }
  catch(...)
  {
    Destroy();
    throw;
  }
}

void GpuExecution::Destroy()
{
  if(m_Device == VK_NULL_HANDLE)
  {
    return;
  }

  // Finish outstanding work
  // Draining needs the timeline semaphore, which may be missing if Initialize failed early. Every retirement runs here, while the device is still alive.

  if(m_FrameActive)
  {
    CancelFrame();
  }

  if(m_Timeline != VK_NULL_HANDLE)
  {
    Drain();
  }

  // Release

  for(Slot& slot : m_Slots)
  {
    if(slot.commandPool != VK_NULL_HANDLE)
    {
      vkDestroyCommandPool(m_Device, slot.commandPool, nullptr);
    }
  }

  if(m_Timeline != VK_NULL_HANDLE)
  {
    vkDestroySemaphore(m_Device, m_Timeline, nullptr);
  }

  m_Slots.clear();
  m_Retirements.clear();
  m_PendingRetirements.clear();

  m_Timeline           = VK_NULL_HANDLE;
  m_Queue              = VK_NULL_HANDLE;
  m_Device             = VK_NULL_HANDLE;
  m_QueueFamily        = 0;
  m_NextSlot           = 0;
  m_ActiveSlot         = 0;
  m_LastSubmittedValue = 0;
}

FrameContext GpuExecution::BeginFrame()
{
  if(m_Device == VK_NULL_HANDLE || m_FrameActive)
  {
    throw std::logic_error("BeginFrame requires an initialized idle GpuExecution");
  }

  // Reclaim the slot
  // The slot's command buffer from its previous frame may still be executing, so wait for it before resetting the pool.
  // This wait is what limits the number of frames in flight to the slot count.

  m_ActiveSlot = m_NextSlot;
  Slot& slot = m_Slots[m_ActiveSlot];

  Wait(slot.completion);
  CollectRetiredResources();

  CheckVk(vkResetCommandPool(m_Device, slot.commandPool, 0), "vkResetCommandPool(frame)");

  // Begin recording

  const VkCommandBufferBeginInfo beginInfo {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };

  CheckVk(vkBeginCommandBuffer(slot.commandBuffer, &beginInfo), "vkBeginCommandBuffer(frame)");

  m_FrameActive = true;

  return FrameContext { .commands = slot.commandBuffer, .slot = FrameSlot { m_ActiveSlot } };
}

CompletionPoint GpuExecution::SubmitFrame(const SubmissionSync& synchronization)
{
  if(!m_FrameActive)
  {
    throw std::logic_error("SubmitFrame requires an active frame");
  }

  Slot& slot = m_Slots[m_ActiveSlot];
  CheckVk(vkEndCommandBuffer(slot.commandBuffer), "vkEndCommandBuffer(frame)");

  // Resources retired while this frame was recording may be referenced by its commands, so they wait for this submission.
  const CompletionPoint completion = Submit(slot.commandBuffer, synchronization);

  slot.completion = completion;
  m_FrameActive   = false;
  m_NextSlot      = (m_ActiveSlot + 1) % static_cast<uint32_t>(m_Slots.size());

  PublishPendingRetirements(completion);

  return completion;
}

void GpuExecution::CancelFrame()
{
  if(!m_FrameActive)
  {
    return;
  }

  // The cancelled commands never reach the GPU, so pending retirements only need to wait for work submitted before this frame.
  Slot& slot = m_Slots[m_ActiveSlot];
  CheckVk(vkResetCommandPool(m_Device, slot.commandPool, 0), "vkResetCommandPool(cancel frame)");

  m_FrameActive = false;

  PublishPendingRetirements(CompletionPoint { m_LastSubmittedValue });
  CollectRetiredResources();
}

CompletionPoint GpuExecution::ExecuteAndWait(const std::function<void(VkCommandBuffer)>& record)
{
  if(m_Device == VK_NULL_HANDLE || m_FrameActive)
  {
    throw std::logic_error("ExecuteAndWait requires an initialized idle GpuExecution");
  }

  // One-shot pool
  // A temporary pool keeps one-shot work out of the frame slots, whose pools are reset on their own schedule.
  // The pool is destroyed on both the success and the exception path.

  VkCommandPool commandPool = VK_NULL_HANDLE;

  const VkCommandPoolCreateInfo poolInfo {
    .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
    .queueFamilyIndex = m_QueueFamily,
  };

  CheckVk(vkCreateCommandPool(m_Device, &poolInfo, nullptr, &commandPool), "vkCreateCommandPool(one-shot)");

  try
  {
    // Record

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    const VkCommandBufferAllocateInfo allocateInfo {
      .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool        = commandPool,
      .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
    };

    CheckVk(vkAllocateCommandBuffers(m_Device, &allocateInfo, &commandBuffer), "vkAllocateCommandBuffers(one-shot)");

    const VkCommandBufferBeginInfo beginInfo {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    CheckVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer(one-shot)");
    record(commandBuffer);
    CheckVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer(one-shot)");

    // Submit and wait
    // The command buffer must finish executing before its pool is destroyed.

    const CompletionPoint completion = Submit(commandBuffer, {});

    PublishPendingRetirements(completion);
    Wait(completion);
    CollectRetiredResources();

    vkDestroyCommandPool(m_Device, commandPool, nullptr);

    return completion;
  }
  catch(...)
  {
    vkDestroyCommandPool(m_Device, commandPool, nullptr);
    throw;
  }
}

CompletionPoint GpuExecution::SubmitSynchronizationAndWait(const SubmissionSync& synchronization)
{
  if(m_Device == VK_NULL_HANDLE || m_FrameActive)
  {
    throw std::logic_error("SubmitSynchronizationAndWait requires an initialized idle GpuExecution");
  }

  const CompletionPoint completion = Submit(VK_NULL_HANDLE, synchronization);

  Wait(completion);
  CollectRetiredResources();

  return completion;
}

CompletionPoint GpuExecution::Submit(VkCommandBuffer commandBuffer, const SubmissionSync& synchronization)
{
  // Semaphores
  // The caller's waits and signals are passed through, and the timeline signal for this submission is appended to the signals.

  const uint64_t proposedValue = m_LastSubmittedValue + 1;

  std::vector<VkSemaphoreSubmitInfo> waits;
  std::vector<VkSemaphoreSubmitInfo> signals;
  waits.reserve(synchronization.waits.size());
  signals.reserve(synchronization.signals.size() + 1);

  for(const SemaphoreWait& wait : synchronization.waits)
  {
    waits.push_back(VkSemaphoreSubmitInfo {
      .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = wait.semaphore,
      .value     = wait.value,
      .stageMask = wait.stages,
    });
  }

  for(const SemaphoreSignal& signal : synchronization.signals)
  {
    signals.push_back(VkSemaphoreSubmitInfo {
      .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = signal.semaphore,
      .value     = signal.value,
      .stageMask = signal.stages,
    });
  }

  signals.push_back(VkSemaphoreSubmitInfo {
    .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
    .semaphore = m_Timeline,
    .value     = proposedValue,
    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  });

  // Submit
  // A null command buffer produces a synchronization-only submission. The timeline value is only committed once the queue accepts the submission.

  const VkCommandBufferSubmitInfo commandInfo {
    .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
    .commandBuffer = commandBuffer,
  };

  const VkSubmitInfo2 submitInfo {
    .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
    .waitSemaphoreInfoCount   = static_cast<uint32_t>(waits.size()),
    .pWaitSemaphoreInfos      = waits.data(),
    .commandBufferInfoCount   = commandBuffer == VK_NULL_HANDLE ? 0u : 1u,
    .pCommandBufferInfos      = commandBuffer == VK_NULL_HANDLE ? nullptr : &commandInfo,
    .signalSemaphoreInfoCount = static_cast<uint32_t>(signals.size()),
    .pSignalSemaphoreInfos    = signals.data(),
  };

  CheckVk(vkQueueSubmit2(m_Queue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit2");

  m_LastSubmittedValue = proposedValue;

  return CompletionPoint { proposedValue };
}

bool GpuExecution::IsComplete(CompletionPoint point) const
{
  if(!point)
  {
    return true;
  }

  uint64_t value = 0;
  CheckVk(vkGetSemaphoreCounterValue(m_Device, m_Timeline, &value), "vkGetSemaphoreCounterValue");

  return value >= point.value;
}

void GpuExecution::Wait(CompletionPoint point) const
{
  if(!point)
  {
    return;
  }

  const VkSemaphoreWaitInfo waitInfo {
    .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
    .semaphoreCount = 1,
    .pSemaphores    = &m_Timeline,
    .pValues        = &point.value,
  };

  CheckVk(vkWaitSemaphores(m_Device, &waitInfo, UINT64_MAX), "vkWaitSemaphores");
}

void GpuExecution::CollectRetiredResources()
{
  if(m_Device == VK_NULL_HANDLE || m_Timeline == VK_NULL_HANDLE)
  {
    return;
  }

  // Partition
  // Retirements whose completion point the timeline has reached are split from those still waiting.

  uint64_t completedValue = 0;
  CheckVk(vkGetSemaphoreCounterValue(m_Device, m_Timeline, &completedValue), "vkGetSemaphoreCounterValue(retirement)");

  std::vector<Retirement> pending;
  std::vector<Retirement> ready;
  pending.reserve(m_Retirements.size());
  ready.reserve(m_Retirements.size());

  for(Retirement& retirement : m_Retirements)
  {
    if(retirement.completion.value > completedValue)
    {
      pending.push_back(std::move(retirement));
    }
    else
    {
      ready.push_back(std::move(retirement));
    }
  }

  m_Retirements = std::move(pending);

  // Release
  // A release may queue more work. Invoke it only after the source entries have left m_Retirements so those additions cannot invalidate this iteration.

  for(Retirement& retirement : ready)
  {
    retirement.release();
  }
}

void GpuExecution::Drain()
{
  if(m_Device == VK_NULL_HANDLE)
  {
    return;
  }

  Wait(CompletionPoint { m_LastSubmittedValue });
  CollectRetiredResources();
}

void GpuExecution::Retire(std::function<void()> release)
{
  if(!release)
  {
    return;
  }

  // The open frame's commands may still use the resource, and that frame has no completion point until it is submitted.
  if(m_FrameActive)
  {
    m_PendingRetirements.push_back(std::move(release));
    return;
  }

  m_Retirements.push_back(Retirement { CompletionPoint { m_LastSubmittedValue }, std::move(release) });
  CollectRetiredResources();
}

void GpuExecution::Retire(CompletionPoint completion, std::function<void()> release)
{
  if(!release)
  {
    return;
  }

  if(m_FrameActive)
  {
    throw std::logic_error("explicit retirement cannot be queued while a frame is active");
  }

  // A value this instance never submitted would never be reached, so the release would never run.
  if(completion.value > m_LastSubmittedValue)
  {
    throw std::invalid_argument("retirement completion was not published by this execution instance");
  }

  m_Retirements.push_back(Retirement { completion, std::move(release) });
  CollectRetiredResources();
}

void GpuExecution::PublishPendingRetirements(CompletionPoint completion)
{
  for(std::function<void()>& release : m_PendingRetirements)
  {
    m_Retirements.push_back(Retirement { completion, std::move(release) });
  }

  m_PendingRetirements.clear();
}

}  // namespace rtpt
