#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <volk.h>

#include "PathTracing/Common/ResolveMode.h"

namespace rtpt
{

// StreamlineRuntime
// Owns the loaded NVIDIA Streamline interposer and the process-wide Streamline session that DLSS Ray Reconstruction runs through.
// The lifetime is what makes it a class of its own: Streamline has to be initialized before the Vulkan instance exists, because its interposer becomes volk's loader entry point and adds the extensions and features DLSS needs to instance and device creation, and it has to shut down after every feature resource is freed but before the device is destroyed. That span covers every renderer, so Application owns one and lends it to the renderers' denoisers.
// It is also the only file that includes Streamline's headers. Everything else describes a Ray Reconstruction frame with the plain structs below, so a build without Streamline compiles the same renderer code and simply reports Ray Reconstruction as unavailable.
// Every failure - missing DLLs, a bad signature, a non-NVIDIA GPU, an old driver - leaves the runtime unavailable with a reason for the UI instead of throwing, because NRD remains a complete fallback.

class StreamlineRuntime
{
public:

  // CreateInfo
  // Where the Streamline DLLs are looked for. They are copied next to the executable by the build.

  struct CreateInfo
  {
    // Directory holding sl.interposer.dll and the plugins it loads.
    std::filesystem::path pluginDirectory;

    // Whether the application presents to a swapchain. Streamline does per-frame bookkeeping from its present hook, and complains when it never runs; a headless run never presents, so there that complaint is expected.
    bool                  presentsFrames = true;
  };

  // RayReconstructionImage
  // One image Ray Reconstruction reads or writes. Streamline needs the description as well as the handles on Vulkan, and every image must already be in GENERAL layout.

  struct RayReconstructionImage
  {
    // Image handle.
    VkImage           image = VK_NULL_HANDLE;
    // Full-image view.
    VkImageView       view = VK_NULL_HANDLE;
    // Pixel format of the image.
    VkFormat          format = VK_FORMAT_UNDEFINED;
    // Image size in pixels.
    VkExtent2D        extent {};
    // Usage flags the image was created with; Streamline checks them before binding the image as input or output.
    VkImageUsageFlags usage = 0;
  };

  // RayReconstructionFrame
  // Everything one Ray Reconstruction evaluation needs: the tagged images, the camera, and the per-viewport options.
  // Matrices are glm's column-major, column-vector form; the conversion to Streamline's row-major form happens inside EvaluateRayReconstruction.

  struct RayReconstructionFrame
  {
    // Command buffer the evaluation is recorded into. The host has to rebind its own pipeline state afterwards.
    VkCommandBuffer        cmd = VK_NULL_HANDLE;
    // Identifies the denoiser's history; see AllocateViewportId.
    uint32_t               viewportId = 0;

    // Noisy HDR radiance, not demodulated.
    RayReconstructionImage color;
    // Receives the denoised HDR radiance.
    RayReconstructionImage output;
    // Hardware depth in [0, 1], near plane at 0.
    RayReconstructionImage depth;
    // Screen-UV motion from the current sample to its previous position, without jitter.
    RayReconstructionImage motionVectors;
    // Diffuse reflectance of the primary surface.
    RayReconstructionImage diffuseAlbedo;
    // Pre-integrated specular reflectance of the primary surface.
    RayReconstructionImage specularAlbedo;
    // World-space shading normal in xyz and linear roughness in w.
    RayReconstructionImage normalRoughness;
    // Screen-UV motion of what the primary surface reflects.
    RayReconstructionImage specularMotionVectors;

    // World to view space for this frame.
    glm::mat4              viewMatrix { 1.0f };
    // View to clip space for this frame, without jitter.
    glm::mat4              projectionMatrix { 1.0f };
    // Current clip space to the previous frame's clip space.
    glm::mat4              clipToPreviousClip { 1.0f };
    // Sub-pixel offset of this frame's samples, in pixels, in Streamline's convention; see ComputeRayReconstructionJitter.
    glm::vec2              jitterOffset { 0.0f };
    // Distances to the near and far clip planes, taken from the projection.
    float                  nearPlane = 0.0f;
    float                  farPlane  = 0.0f;
    // Vertical field of view in radians, and width over height. Ignored for orthographic projections.
    float                  verticalFov = 0.0f;
    float                  aspectRatio = 1.0f;
    // Whether the projection is orthographic.
    bool                   orthographic = false;
    // Drops Ray Reconstruction's temporal history before this evaluation.
    bool                   resetHistory = false;
    // Which DLSS Ray Reconstruction model to run.
    RayReconstructionPreset preset = RayReconstructionPreset::eDefault;
  };

  StreamlineRuntime() = default;
  StreamlineRuntime(const StreamlineRuntime&)            = delete;
  StreamlineRuntime& operator=(const StreamlineRuntime&) = delete;
  ~StreamlineRuntime();

  void Initialize(const CreateInfo& createInfo);

  void Shutdown();

  PFN_vkGetInstanceProcAddr GetVulkanEntryPoint() const;

  void CheckRayReconstructionSupport(VkPhysicalDevice physicalDevice);

  bool IsRayReconstructionAvailable() const;

  const std::string& GetUnavailableReason() const;

  uint32_t AllocateViewportId();

  bool EvaluateRayReconstruction(const RayReconstructionFrame& frame);

  void FreeRayReconstruction(uint32_t viewportId);

private:

  void MarkUnavailable(std::string reason);

  // HMODULE of sl.interposer.dll, kept as void* so this header does not pull in windows.h. Null when Streamline was not loaded.
  void*       m_Module = nullptr;

  // slInit succeeded, so slShutdown is owed before the device is destroyed.
  bool        m_Initialized = false;

  // DLSS Ray Reconstruction is supported on the device the renderer runs on. Only set by CheckRayReconstructionSupport.
  bool        m_RayReconstructionSupported = false;

  // Why Ray Reconstruction is unavailable, for the UI and the log. Empty while it is available.
  std::string m_UnavailableReason = "Ray Reconstruction support has not been checked yet";

  // Next viewport id to hand out. Each denoiser keeps its own id, so the two renderers never share Ray Reconstruction history.
  uint32_t    m_NextViewportId = 0;

  // Frame index handed to slGetNewFrameToken. Streamline needs a new index for every evaluated frame.
  uint32_t    m_FrameIndex = 0;

  // sl::Result of the last failure that was logged, so a failure repeating every frame is reported once. Zero is eOk.
  uint32_t    m_LastReportedFailure = 0;
};

}  // namespace rtpt
