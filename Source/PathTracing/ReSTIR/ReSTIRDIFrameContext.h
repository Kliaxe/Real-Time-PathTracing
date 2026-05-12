#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

namespace nvsamples
{

class ReSTIRDIFrameContext
{
public:
  void InvalidateHistory();
  void EnsureViewport(VkExtent2D viewportSize);
  void AdvanceFrame();

  uint32_t GetFrameIndex() const;
  uint32_t GetCurrentHistoryIndex() const;
  uint32_t GetPreviousHistoryIndex() const;
  bool     HasHistory() const;

private:
  VkExtent2D m_ViewportSize{};
  uint32_t   m_FrameIndex = 0;
  bool       m_HasHistory = false;
};

}  // namespace nvsamples
