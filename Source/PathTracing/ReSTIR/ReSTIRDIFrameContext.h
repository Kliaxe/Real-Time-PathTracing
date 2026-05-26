#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

namespace nvsamples
{

// Tiny frame-history tracker for ReSTIR. It separates "which frame is this?"
// from the renderer so buffer ping-pong and temporal invalidation stay obvious.
class ReSTIRDIFrameContext
{
public:
  // Resets temporal history and returns the next frame to frame index zero.
  void InvalidateHistory();
  // Resolution changes invalidate the current/previous surface history.
  void EnsureViewport(VkExtent2D viewportSize);
  void AdvanceFrame();

  uint32_t GetFrameIndex() const;
  // Current/previous history buffers are selected by frame parity.
  uint32_t GetCurrentHistoryIndex() const;
  uint32_t GetPreviousHistoryIndex() const;

private:
  VkExtent2D m_ViewportSize{};
  uint32_t   m_FrameIndex = 0;
};

}  // namespace nvsamples
