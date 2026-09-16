#pragma once

#include <cstdint>

#include <glm/vec2.hpp>
#include <volk.h>

#include "DenoiserResources.h"
#include "RayReconstructionInputPass.h"
#include "Framework/Vulkan/Diagnostics.h"
#include "Framework/Vulkan/GpuResources.h"
#include "PathTracing/Common/ResolveMode.h"
#include "Rendering/RenderTargetView.h"
#include "Shaders/ShaderIo.h"

namespace rtpt
{

class StreamlineRuntime;

// Sub-pixel camera jitter for one frame while Ray Reconstruction denoises, in pixels within [-0.5, 0.5).
// Ray Reconstruction anti-aliases by combining samples taken at different positions inside each pixel. DLSS was trained on a Halton (2, 3) pattern, and at DLAA, where render and output resolution match, NVIDIA's guide asks for 8 phases before the pattern repeats.
// The value is what the renderers subtract from the pixel center and what Streamline receives as its jitter offset.
glm::vec2 ComputeRayReconstructionJitter(uint32_t frameIndex);

// RayReconstructionDenoiser
// Denoises one renderer's frames with DLSS Ray Reconstruction, as the peer of NrdDenoiser: it takes the same DenoiserResources and noisy radiance, and writes the denoised radiance into the renderer's HDR target.
// It owns what one renderer's Ray Reconstruction needs for as long as that renderer lives: the input conversion pass and the images it writes, and a Streamline viewport id that keeps this renderer's temporal history apart from the other's.
// The Streamline session itself is shared and borrowed through StreamlineRuntime, so without it, or on hardware that cannot run it, the denoiser reports itself unavailable and records nothing.

class RayReconstructionDenoiser
{
public:

  // CreateInfo
  // Dependencies fixed for the lifetime of the denoiser.

  struct CreateInfo
  {
    // Device the input pass is created on.
    VkDevice                 device = VK_NULL_HANDLE;

    // Allocates the converted input images.
    rtpt::ResourceAllocator* resources = nullptr;

    // Optional; used only to attach debug names.
    const rtpt::Diagnostics* diagnostics = nullptr;

    // Streamline session to evaluate through. Null leaves the denoiser permanently unavailable.
    StreamlineRuntime*       streamline = nullptr;

    // Number of in-flight frame slots, forwarded to the input pass.
    uint32_t                 frameSlotCount = 0;
  };

  // FrameInput
  // Everything one denoised frame needs. Borrowed for the duration of Denoise only.

  struct FrameInput
  {
    // Command buffer the conversion and the evaluation are recorded into.
    VkCommandBuffer                cmd = VK_NULL_HANDLE;

    // The renderer's guide buffers and denoiser signals, written this frame.
    const DenoiserResources*       denoiserInputs = nullptr;

    // The renderer's noisy radiance for this frame.
    const rtpt::Image*             color = nullptr;

    // HDR target that receives the denoised radiance. Must be in GENERAL layout.
    RenderTargetView               output {};

    // CPU copy of the scene info: the camera matrices and jitter the renderer traced with.
    const shaderio::GltfSceneInfo* sceneInfo = nullptr;

    // Device address of the same scene info, for the input pass.
    VkDeviceAddress                sceneInfoAddress = 0;

    // Model and clamp settings for this frame.
    const RayReconstructionSettings* settings = nullptr;

    // Scene luminance the tonemapper maps to middle grey; the radiance clamp is a multiple of it.
    float                          greyLuminance = 1.0f;

    // Set when the renderer's history no longer matches, so Ray Reconstruction restarts.
    bool                           historyInvalidated = false;

    // Frame slot being recorded; selects the input pass's descriptor set.
    uint32_t                       frameSlot = 0;
  };

  explicit RayReconstructionDenoiser(const CreateInfo& createInfo);

  void Initialize();

  void Destroy();

  bool IsReady() const;

  bool IsAvailable() const;

  void InvalidateHistory();

  bool Denoise(const FrameInput& input);

private:

  void EnsureForViewport(VkExtent2D viewportSize);

  void DestroyViewportResources();

  rtpt::Image CreateStorageImage(VkExtent2D viewportSize, VkFormat format, const char* debugName) const;

  void InsertComputeBarrier(VkCommandBuffer cmd) const;

  // Device the input pass is created on.
  VkDevice                   m_Device = VK_NULL_HANDLE;

  // Allocates the converted input images.
  rtpt::ResourceAllocator*   m_Resources = nullptr;

  // Optional debug-name sink; may be null.
  const rtpt::Diagnostics*   m_Diagnostics = nullptr;

  // Shared Streamline session; null when the build or the caller has none.
  StreamlineRuntime*         m_Streamline = nullptr;

  // Converts the renderer's signals into Ray Reconstruction's inputs.
  RayReconstructionInputPass m_InputPass;

  // Streamline viewport this denoiser's history lives under. Allocated in Initialize.
  uint32_t                   m_ViewportId = 0;

  // Set once an evaluation has run, so Destroy knows Streamline holds resources for the viewport.
  bool                       m_HasEvaluated = false;

  // Pending history reset. Starts true so the first evaluation never blends with stale history.
  bool                       m_HistoryInvalidated = true;

  // Resolution the images were allocated at. Zero until the first frame.
  VkExtent2D                 m_ViewportSize {};

  // Sanitized, clamped noisy radiance.
  rtpt::Image                m_ColorImage;

  // Diffuse reflectance guide.
  rtpt::Image                m_DiffuseAlbedoImage;

  // Specular reflectance guide.
  rtpt::Image                m_SpecularAlbedoImage;

  // World-space normal and linear roughness guide.
  rtpt::Image                m_NormalRoughnessImage;

  // Hardware depth guide.
  rtpt::Image                m_DepthImage;

  // Reflection motion guide.
  rtpt::Image                m_SpecularMotionVectorsImage;

  // Surface motion with the background's camera rotation filled in.
  rtpt::Image                m_MotionVectorsImage;
};

}  // namespace rtpt
