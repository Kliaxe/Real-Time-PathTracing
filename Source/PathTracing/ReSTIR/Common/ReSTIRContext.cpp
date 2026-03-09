#include "ReSTIRContext.h"

namespace nvsamples
{

void ReSTIRContext::InvalidateHistory()
{
  m_FrameIndex = 0;
  m_HasHistory = false;
}

void ReSTIRContext::EnsureViewport(VkExtent2D viewportSize)
{
  if(m_ViewportSize.width == viewportSize.width && m_ViewportSize.height == viewportSize.height)
  {
    return;
  }

  m_ViewportSize = viewportSize;
  InvalidateHistory();
}

void ReSTIRContext::AdvanceFrame()
{
  m_HasHistory = true;
  ++m_FrameIndex;
}

uint32_t ReSTIRContext::GetFrameIndex() const
{
  return m_FrameIndex;
}

uint32_t ReSTIRContext::GetCurrentHistoryIndex() const
{
  return m_FrameIndex & 1u;
}

uint32_t ReSTIRContext::GetPreviousHistoryIndex() const
{
  return 1u - GetCurrentHistoryIndex();
}

bool ReSTIRContext::HasHistory() const
{
  return m_HasHistory;
}

}  // namespace nvsamples
