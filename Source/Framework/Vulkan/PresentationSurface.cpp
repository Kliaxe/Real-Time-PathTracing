#include "PresentationSurface.h"

#include <stdexcept>

namespace rtpt
{

PresentationSurface::~PresentationSurface()
{
  Destroy();
}

void PresentationSurface::Initialize(VkInstance instance, VkSurfaceKHR surface)
{
  if(m_Surface != VK_NULL_HANDLE || instance == VK_NULL_HANDLE || surface == VK_NULL_HANDLE)
  {
    throw std::invalid_argument("invalid PresentationSurface initialization");
  }

  m_Instance = instance;
  m_Surface  = surface;
}

void PresentationSurface::Destroy()
{
  if(m_Surface != VK_NULL_HANDLE)
  {
    vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
  }

  m_Surface  = VK_NULL_HANDLE;
  m_Instance = VK_NULL_HANDLE;
}

}  // namespace rtpt
