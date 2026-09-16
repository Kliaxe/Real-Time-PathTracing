#pragma once

#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <vulkan/vulkan_core.h>

#include "PathTracing/Common/ResolveMode.h"
#include "Shaders/ShaderIo.h"

namespace rtpt
{

// ResolveHistoryTracker
// The pure bookkeeping half of ResolveHistory: builds and compares the history signatures, counts accumulated frames, and takes the per-frame history decisions.
// It holds no Vulkan runtime, NRD, or allocator state, so the rules that decide when accumulation and NRD history restart can be exercised by CPU tests without a GPU.
// ResolveHistory owns one and carries out what it reports on NRD, such as dropping NRD's temporal history after a frame that did not denoise.

class ResolveHistoryTracker
{
public:

  // FrameInput
  // The part of one frame's state the history decisions depend on. Borrowed for the duration of BeginFrame only.

  struct FrameInput
  {
    // CPU copy of the scene info: camera and environment for the signatures. Must not be null.
    const shaderio::GltfSceneInfo* sceneInfo                = nullptr;
    // TLAS address, copied into both signatures. It is not what catches a scene rebuild; see AccumulationSignature::topLevelAsAddress.
    VkDeviceAddress                topLevelAsAddress        = 0;
    // Resolution this frame is rendered at.
    VkExtent2D                     viewportSize {};
    // Decides whether this frame accumulates, denoises, or neither.
    RenderResolveMode              resolveMode              = RenderResolveMode::eOff;
    // False when the renderer cannot produce NRD signals that describe its beauty image this frame, such as ReSTIR PT's reference path tracer view.
    // Denoising then stands down for the frame and NRD history is dropped, exactly as if denoising were off.
    bool                           denoiserSignalsAvailable = true;
  };

  // AccumulationSignature
  // Compact CPU-side key for "can old accumulated pixels still be trusted?".
  // Accumulation averages pixels visually, so it depends on the camera as well as the lighting environment; any change here means the average is of two different images and must restart.
  // Compared with memcmp, so every byte must be deterministic: all padding is written out as named members, and the static_assert after the struct proves the compiler added none of its own.

  struct AccumulationSignature
  {
    // Camera matrices. Any camera move changes what an accumulated pixel shows.
    glm::mat4                     viewProjMatrix {};
    glm::mat4                     viewProjInvMatrix {};
    glm::mat4                     viewInvMatrix {};
    glm::vec3                     cameraPosition {};
    // Environment selection.
    int                           useSky                  = 0;
    int                           useHdrEnv               = 0;
    int                           environmentTextureIndex = -1;
    // Explicit padding, so no implicit padding bytes reach the memcmp.
    int                           _pad0                   = 0;
    // Constant background radiance used when no sky or HDR environment is active.
    glm::vec3                     backgroundColor {};
    // Explicit padding, so no implicit padding bytes reach the memcmp.
    int                           _pad1                   = 0;
    // Procedural sky parameters.
    shaderio::SkySimpleParameters skySimpleParam {};
    // Explicit padding up to topLevelAsAddress's 8-byte alignment. Without it the compiler inserted four indeterminate bytes here, and memcmp compared them.
    int                           _pad2                   = 0;
    // TLAS address. Only a cheap extra guard: a rebuilt TLAS can land at the same address, so scene rebuilds are caught by Application::CreateScene invalidating render history right after SceneRuntime::RebuildScene.
    VkDeviceAddress               topLevelAsAddress       = 0;
    // Resolution. Accumulated pixels are meaningless at another size.
    VkExtent2D                    viewportSize {};
  };

  // The size equals the sum of the member sizes only if the compiler inserted no padding of its own. SkySimpleParameters is all floats, and SkyRenderer asserts its 112-byte size.
  static_assert(sizeof(AccumulationSignature) == 3 * sizeof(glm::mat4) + 2 * sizeof(glm::vec3) + 6 * sizeof(int) + sizeof(shaderio::SkySimpleParameters) + sizeof(VkDeviceAddress) + sizeof(VkExtent2D), "AccumulationSignature must have no implicit padding");

  // DenoiserSignature
  // Answers the narrower question "can NRD's temporal history still be trusted?".
  // Deliberately smaller than the accumulation signature: both denoisers reproject with the motion vectors the renderer writes, so a camera move is not a reason to drop their history - only a change of denoiser, of what is being lit, or of the buffer sizes is.
  // Compared with memcmp, so every byte must be deterministic: all padding is written out as named members, and the static_assert after the struct proves the compiler added none of its own.

  struct DenoiserSignature
  {
    // Environment selection.
    int                           useSky                  = 0;
    int                           useHdrEnv               = 0;
    int                           environmentTextureIndex = -1;
    // Which resolve mode ran. NRD and Ray Reconstruction keep separate temporal histories, so switching between them must restart the one taking over. It also fills the four bytes before backgroundColor that would otherwise be implicit padding.
    RenderResolveMode             resolveMode             = RenderResolveMode::eOff;
    // Constant background radiance used when no sky or HDR environment is active.
    glm::vec3                     backgroundColor {};
    // Explicit padding, so no implicit padding bytes reach the memcmp.
    int                           _pad1                   = 0;
    // Procedural sky parameters.
    shaderio::SkySimpleParameters skySimpleParam {};
    // TLAS address. Only a cheap extra guard: a rebuilt TLAS can land at the same address, so scene rebuilds are caught by Application::CreateScene invalidating render history right after SceneRuntime::RebuildScene.
    VkDeviceAddress               topLevelAsAddress       = 0;
    // Resolution. NRD's history images are sized to it.
    VkExtent2D                    viewportSize {};
  };

  // Same proof as for AccumulationSignature: the size equals the sum of the member sizes only without compiler-inserted padding.
  static_assert(sizeof(DenoiserSignature) == sizeof(glm::vec3) + 4 * sizeof(int) + sizeof(RenderResolveMode) + sizeof(shaderio::SkySimpleParameters) + sizeof(VkDeviceAddress) + sizeof(VkExtent2D), "DenoiserSignature must have no implicit padding");

  // FrameState
  // Snapshot of the history decisions for one recorded frame.
  // Taken once in BeginFrame so every later step, including FinishFrame, works from the same assumptions the passes used.

  struct FrameState
  {
    // Resolution this frame is rendered at.
    VkExtent2D            viewportSize {};
    // Resolve mode this frame runs, which also says which denoiser owns the output when denoiseEnabled is set.
    RenderResolveMode     resolveMode = RenderResolveMode::eOff;
    // Accumulation key for this frame, stored by FinishFrame for the next comparison.
    AccumulationSignature accumulationSignature {};
    // Denoiser history key for this frame, stored by FinishFrame when denoising ran.
    DenoiserSignature     denoiserSignature {};
    // The shader blends this frame into the accumulation history.
    bool                  accumulateEnabled          = false;
    // The shader writes NRD signals and Denoise runs after the passes.
    bool                  denoiseEnabled             = false;
    // Accumulation restarted this frame, either explicitly or because the accumulation signature changed while accumulating.
    // Renderers restart any history of their own from this. It is gated on accumulate mode so a camera move with accumulation off does not wipe history that temporal reuse still depends on.
    bool                  accumulationRestarted      = false;
    // NRD must restart its temporal history this frame.
    bool                  denoiserHistoryInvalidated = false;
  };

  // FinishResult
  // What the owner must do on the GPU side once FinishFrame has stored a frame's history.
  // The tracker cannot touch NRD itself, so decisions about NRD's own history are handed back here instead.

  struct FinishResult
  {
    // The frame did not denoise, so NRD's temporal history no longer follows the image sequence and must be dropped now.
    bool dropDenoiserHistory = false;
  };

  FrameState   BeginFrame(const FrameInput& input);
  FinishResult FinishFrame(const FrameState& frameState);
  void         InvalidateHistory();

  // Raw counter, valid between BeginFrame and FinishFrame. Callers that report it gate it on their own resolve mode.
  uint32_t GetAccumulatedFrameCount() const;

  // Signature construction and comparison are public so tests can prove the bytes the comparison sees are deterministic.
  static AccumulationSignature MakeAccumulationSignature(const FrameInput& input);
  static DenoiserSignature     MakeDenoiserSignature(const FrameInput& input);
  static bool                  SignaturesMatch(const AccumulationSignature& left, const AccumulationSignature& right);
  static bool                  SignaturesMatch(const DenoiserSignature& left, const DenoiserSignature& right);

private:

  // Accumulation is presentation/reference history, separate from NRD history and from any reuse history the renderer keeps.
  uint32_t              m_AccumulatedFrames        = 0;
  // Set by InvalidateHistory; forces the next frame to restart both accumulation and NRD history.
  bool                  m_HistoryInvalidated       = true;
  // False until a frame has stored m_LastAccumulationSignature, so the first comparison always counts as changed.
  bool                  m_HasAccumulationSignature = false;
  // False until a denoised frame has stored m_LastDenoiserSignature, so the first comparison always counts as changed.
  bool                  m_HasDenoiserSignature     = false;
  // Accumulation key of the last recorded frame.
  AccumulationSignature m_LastAccumulationSignature {};
  // NRD history key of the last denoised frame.
  DenoiserSignature     m_LastDenoiserSignature {};
};

}  // namespace rtpt
