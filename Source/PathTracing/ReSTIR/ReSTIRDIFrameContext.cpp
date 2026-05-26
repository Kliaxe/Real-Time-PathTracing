#include "ReSTIRDIFrameContext.h"

namespace nvsamples
{

void ReSTIRDIFrameContext::InvalidateHistory()
{
  // Frame zero is treated as "no previous frame" by the shader-side temporal pass.
  m_FrameIndex = 0;
}

void ReSTIRDIFrameContext::EnsureViewport(VkExtent2D viewportSize)
{
  if(m_ViewportSize.width == viewportSize.width && m_ViewportSize.height == viewportSize.height)
  {
    return;
  }

  // Changing resolution invalidates both the surface history and the reservoir history using it.
  m_ViewportSize = viewportSize;
  InvalidateHistory();
}

void ReSTIRDIFrameContext::AdvanceFrame()
{
  ++m_FrameIndex;
}

uint32_t ReSTIRDIFrameContext::GetFrameIndex() const
{
  return m_FrameIndex;
}

uint32_t ReSTIRDIFrameContext::GetCurrentHistoryIndex() const
{
  // Even/odd frame parity selects which surface buffer receives this frame.
  return m_FrameIndex & 1u;
}

uint32_t ReSTIRDIFrameContext::GetPreviousHistoryIndex() const
{
  // With two buffers, previous is always the opposite parity.
  return 1u - GetCurrentHistoryIndex();
}

}  // namespace nvsamples
