#include "ReSTIRFrameContext.h"

namespace rtpt
{

void ReSTIRFrameContext::InvalidateHistory()
{
  // Frame zero is treated as "no previous frame" by the shader-side temporal pass.
  m_FrameIndex = 0;
}

void ReSTIRFrameContext::EnsureViewport(VkExtent2D viewportSize)
{
  if(m_ViewportSize.width == viewportSize.width && m_ViewportSize.height == viewportSize.height)
  {
    return;
  }

  // Changing resolution invalidates both the surface history and the reservoir history using it.
  m_ViewportSize = viewportSize;

  InvalidateHistory();
}

void ReSTIRFrameContext::AdvanceFrame()
{
  ++m_FrameIndex;
}

uint32_t ReSTIRFrameContext::GetFrameIndex() const
{
  return m_FrameIndex;
}

uint32_t ReSTIRFrameContext::GetCurrentHistoryIndex() const
{
  // Even/odd frame parity selects which surface buffer receives this frame.
  return m_FrameIndex & 1u;
}

uint32_t ReSTIRFrameContext::GetPreviousHistoryIndex() const
{
  // With two buffers, previous is always the opposite parity.
  return 1u - GetCurrentHistoryIndex();
}

}  // namespace rtpt
