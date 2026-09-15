#pragma once

#include "FrameContext.h"
#include "Barriers.h"
#include "GpuExecution.h"
#include "GpuResources.h"

#include <span>

namespace rtpt
{

// ReadbackTicket
// Handle for one in-flight buffer readback. It owns the host-visible destination buffer and remembers which submission must finish before the bytes are valid.

struct ReadbackTicket
{
  // Mapped host-visible buffer the GPU copies into.
  Buffer          buffer {};
  // Completion point of the submission that recorded the copy. Empty until Commit.
  CompletionPoint completion {};
  // Bytes copied, which is also the size of the buffer.
  VkDeviceSize    size = 0;

  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(buffer); }
};

// ReadbackContext
// Copies device buffer contents back to the host inside an open frame, so tests can inspect what a submission produced.
// The copy is recorded into the frame, the ticket is committed with that frame's completion point, and Bytes blocks until the GPU is done.

class ReadbackContext
{
public:

  void Initialize(ResourceAllocator& resources, GpuExecution& execution);

  // Records a barrier from sourceAccess and a copy into a fresh host-visible buffer. Requires an active frame.
  [[nodiscard]] ReadbackTicket RecordBufferCopy(const FrameContext& frame, VkBuffer source, VkDeviceSize sourceOffset, VkDeviceSize size, AccessScope sourceAccess);
  // Binds the ticket to the completion point of the submission that carried its copy. Allowed exactly once.
  void Commit(ReadbackTicket& ticket, CompletionPoint completion) const;
  // Waits for the committed submission and returns the copied bytes, valid until Release.
  [[nodiscard]] std::span<const std::byte> Bytes(ReadbackTicket& ticket) const;
  // Retires the destination buffer once its submission has completed and empties the ticket.
  void Release(ReadbackTicket& ticket) const;

private:

  // Allocates and destroys readback buffers.
  ResourceAllocator* m_Resources = nullptr;
  // Provides the active frame, waits on completions, and defers buffer destruction.
  GpuExecution*      m_Execution = nullptr;
};

}  // namespace rtpt
