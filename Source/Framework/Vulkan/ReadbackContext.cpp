#include "ReadbackContext.h"

#include "Barriers.h"
#include "Diagnostics.h"

#include <memory>
#include <stdexcept>

namespace rtpt
{

void ReadbackContext::Initialize(ResourceAllocator& resources, GpuExecution& execution)
{
  m_Resources = &resources;
  m_Execution = &execution;
}

ReadbackTicket ReadbackContext::RecordBufferCopy(const FrameContext& frame, VkBuffer source, VkDeviceSize sourceOffset, VkDeviceSize size, AccessScope sourceAccess)
{
  // The copy is recorded into the frame's command buffer, so there must be one open to record into.
  if(m_Resources == nullptr || m_Execution == nullptr || !m_Execution->HasActiveFrame() || size == 0)
  {
    throw std::logic_error("buffer readback requires an initialized active frame and a non-empty range");
  }

  // Destination buffer
  // Mapped and randomly host-accessible, since the caller reads it in place. The mapping may not be coherent, so Bytes invalidates it before reading.

  ReadbackTicket ticket { .size = size };

  CheckVk(m_Resources->CreateBuffer(ticket.buffer, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT), "ResourceAllocator::CreateBuffer(readback)");

  // Copy
  // The barrier makes the producer's writes visible to the transfer stage before the copy reads them.

  CmdBufferBarrier(frame.commands, source, sourceOffset, size, sourceAccess, { .stages = VK_PIPELINE_STAGE_2_COPY_BIT, .access = VK_ACCESS_2_TRANSFER_READ_BIT });

  const VkBufferCopy copy { .srcOffset = sourceOffset, .dstOffset = 0, .size = size };

  vkCmdCopyBuffer(frame.commands, source, ticket.buffer.buffer, 1, &copy);

  return ticket;
}

void ReadbackContext::Commit(ReadbackTicket& ticket, CompletionPoint completion) const
{
  // A ticket bound to two submissions could wait on the wrong one, so a second commit is refused.
  if(!ticket || !completion || ticket.completion)
  {
    throw std::invalid_argument("readback ticket can be committed exactly once after a successful submission");
  }

  ticket.completion = completion;
}

std::span<const std::byte> ReadbackContext::Bytes(ReadbackTicket& ticket) const
{
  // Without a completion point there is nothing to wait on, and the mapping would expose whatever the GPU has written so far.
  if(!ticket || !ticket.completion)
  {
    throw std::logic_error("readback bytes are unavailable before submission is committed");
  }

  m_Execution->Wait(ticket.completion);

  CheckVk(m_Resources->InvalidateBuffer(ticket.buffer, 0, ticket.size), "ResourceAllocator::InvalidateBuffer(readback)");

  return { ticket.buffer.mapping, static_cast<size_t>(ticket.size) };
}

void ReadbackContext::Release(ReadbackTicket& ticket) const
{
  if(!ticket)
  {
    return;
  }

  // Deferred destruction
  // The buffer may still be a copy destination on the GPU, so it is moved into a retirement that runs once the ticket's completion point is reached.
  // The ticket is emptied first so the caller cannot touch a buffer that is queued for destruction.

  auto buffer = std::make_shared<Buffer>(std::move(ticket.buffer));

  const CompletionPoint completion = ticket.completion;

  ticket = {};

  ResourceAllocator* resources = m_Resources;

  m_Execution->Retire(completion, [resources, buffer]() { resources->DestroyBuffer(*buffer); });
}

}  // namespace rtpt
