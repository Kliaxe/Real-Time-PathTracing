#pragma once

#include <volk.h>

namespace rtpt
{

// PresentationSurface
// Takes ownership of a window surface created elsewhere and destroys it with the instance it belongs to.
// It must be destroyed before that instance.

class PresentationSurface
{
public:

  PresentationSurface() = default;
  PresentationSurface(const PresentationSurface&)            = delete;
  PresentationSurface& operator=(const PresentationSurface&) = delete;
  ~PresentationSurface();

  void Initialize(VkInstance instance, VkSurfaceKHR surface);
  void Destroy();

  [[nodiscard]] VkSurfaceKHR Handle() const noexcept { return m_Surface; }

private:

  // Instance the surface was created from.
  VkInstance   m_Instance = VK_NULL_HANDLE;
  // Owned surface.
  VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
};

}  // namespace rtpt
