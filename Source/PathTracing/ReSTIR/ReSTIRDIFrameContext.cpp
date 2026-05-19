#include "ReSTIRDIFrameContext.h"

namespace nvsamples
{

void ReSTIRDIFrameContext::InvalidateHistory()
{
  m_FrameIndex = 0;
}

void ReSTIRDIFrameContext::EnsureViewport(VkExtent2D viewportSize)
{
  if(m_ViewportSize.width == viewportSize.width && m_ViewportSize.height == viewportSize.height)
  {
    return;
  }

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
  return m_FrameIndex & 1u;
}

uint32_t ReSTIRDIFrameContext::GetPreviousHistoryIndex() const
{
  return 1u - GetCurrentHistoryIndex();
}

}  // namespace nvsamples
