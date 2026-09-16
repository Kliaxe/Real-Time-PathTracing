#include "RayReconstructionDenoiser.h"

#include <cmath>

#include <glm/glm.hpp>

#include "Denoising/StreamlineRuntime.h"
#include "Framework/Vulkan/Barriers.h"

namespace rtpt
{

namespace
{

// Formats
// Half floats hold every guide with room to spare; the input pass clamps colour into that range. Depth keeps full precision, because DLSS linearizes it and half-float steps would show as banding in its disocclusion tests.

constexpr VkFormat kColorFormat         = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kAlbedoFormat        = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kNormalFormat        = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat         = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kMotionVectorsFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Usage of every image this denoiser owns: written as storage by the input pass, read by DLSS.
constexpr VkImageUsageFlags kImageUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

// Phases of the jitter pattern at DLAA; see ComputeRayReconstructionJitter.
constexpr uint32_t kJitterPhaseCount = 8;

// Radical inverse of index in the given base: the Halton sequence's value for that index, in [0, 1).
float Halton(uint32_t index, uint32_t base)
{
  float result   = 0.0f;
  float fraction = 1.0f / static_cast<float>(base);

  while(index > 0)
  {
    result   += fraction * static_cast<float>(index % base);
    index    /= base;
    fraction /= static_cast<float>(base);
  }

  return result;
}

StreamlineRuntime::RayReconstructionImage DescribeImage(const rtpt::Image& image)
{
  return StreamlineRuntime::RayReconstructionImage {
      .image  = image.image,
      .view   = image.descriptor.imageView,
      .format = image.format,
      .extent = { image.extent.width, image.extent.height },
      .usage  = kImageUsage,
  };
}

}  // namespace

glm::vec2 ComputeRayReconstructionJitter(uint32_t frameIndex)
{
  // Index 0 of a Halton sequence is 0 in every base, a sample exactly at the pixel corner, so the pattern starts at 1.
  const uint32_t phase = frameIndex % kJitterPhaseCount + 1;

  return glm::vec2(Halton(phase, 2), Halton(phase, 3)) - 0.5f;
}

RayReconstructionDenoiser::RayReconstructionDenoiser(const CreateInfo& createInfo)
    : m_Device(createInfo.device)
    , m_Resources(createInfo.resources)
    , m_Diagnostics(createInfo.diagnostics)
    , m_Streamline(createInfo.streamline)
    , m_InputPass(RayReconstructionInputPass::CreateInfo { .device = createInfo.device, .frameSlotCount = createInfo.frameSlotCount })
{
}

void RayReconstructionDenoiser::Initialize()
{
  // Without Streamline nothing could ever be evaluated, so no pipeline is built for it.
  if(m_Streamline == nullptr || m_Resources == nullptr)
  {
    return;
  }

  m_InputPass.Initialize();

  m_ViewportId         = m_Streamline->AllocateViewportId();
  m_HistoryInvalidated = true;
}

void RayReconstructionDenoiser::Destroy()
{
  // Streamline's resources for this viewport reference the images below only during evaluation, but they are still released first so nothing outlives the device work that used them.
  if(m_Streamline != nullptr && m_HasEvaluated)
  {
    m_Streamline->FreeRayReconstruction(m_ViewportId);
  }

  m_HasEvaluated = false;

  DestroyViewportResources();

  m_InputPass.Destroy();
}

bool RayReconstructionDenoiser::IsReady() const
{
  return m_InputPass.IsReady();
}

bool RayReconstructionDenoiser::IsAvailable() const
{
  return m_Streamline != nullptr && m_Streamline->IsRayReconstructionAvailable();
}

void RayReconstructionDenoiser::InvalidateHistory()
{
  m_HistoryInvalidated = true;
}

bool RayReconstructionDenoiser::Denoise(const FrameInput& input)
{
  const VkExtent2D viewportSize = input.output.extent;

  if(!IsReady() || !IsAvailable() || input.cmd == VK_NULL_HANDLE || input.denoiserInputs == nullptr || input.color == nullptr || !input.output || input.sceneInfo == nullptr || input.settings == nullptr)
  {
    return false;
  }

  EnsureForViewport(viewportSize);

  // Image layouts
  // The images owned here only need their first transition out of UNDEFINED; after that they stay in GENERAL.

  TransitionStorageImageForWrite(input.cmd, m_ColorImage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, StorageImageWriteOrdering::eFirstTransitionOnly);
  TransitionStorageImageForWrite(input.cmd, m_DiffuseAlbedoImage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, StorageImageWriteOrdering::eFirstTransitionOnly);
  TransitionStorageImageForWrite(input.cmd, m_SpecularAlbedoImage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, StorageImageWriteOrdering::eFirstTransitionOnly);
  TransitionStorageImageForWrite(input.cmd, m_NormalRoughnessImage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, StorageImageWriteOrdering::eFirstTransitionOnly);
  TransitionStorageImageForWrite(input.cmd, m_DepthImage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, StorageImageWriteOrdering::eFirstTransitionOnly);
  TransitionStorageImageForWrite(input.cmd, m_SpecularMotionVectorsImage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, StorageImageWriteOrdering::eFirstTransitionOnly);
  TransitionStorageImageForWrite(input.cmd, m_MotionVectorsImage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, StorageImageWriteOrdering::eFirstTransitionOnly);

  // Inputs
  // The barrier publishes the pass's writes to DLSS's first dispatch.

  const RayReconstructionInputPass::RecordInput passInput {
      .denoiserInputs   = input.denoiserInputs,
      .color            = input.color,
      .outputs          = {
          .color                 = &m_ColorImage,
          .diffuseAlbedo         = &m_DiffuseAlbedoImage,
          .specularAlbedo        = &m_SpecularAlbedoImage,
          .normalRoughness       = &m_NormalRoughnessImage,
          .depth                 = &m_DepthImage,
          .specularMotionVectors = &m_SpecularMotionVectorsImage,
          .motionVectors         = &m_MotionVectorsImage,
      },
      .sceneInfoAddress = input.sceneInfoAddress,
      .radianceClamp    = ComputeRayReconstructionRadianceClamp(*input.settings, input.greyLuminance),
      .viewportSize     = viewportSize,
      .frameSlot        = input.frameSlot,
  };

  m_InputPass.Record(input.cmd, passInput);

  InsertComputeBarrier(input.cmd);

  // Camera
  // The scene info carries view-projection products, so the projection is recovered as viewProj * viewInv and the clip-to-previous-clip transform as prevViewProj * viewProjInv.
  // The clip planes, field of view, and aspect ratio are read back from the projection's own terms, which covers both of the camera's projections without a second source of camera state.

  const shaderio::GltfSceneInfo& sceneInfo = *input.sceneInfo;

  const glm::mat4 projection   = sceneInfo.viewProjMatrix * sceneInfo.viewInvMatrix;
  const bool      orthographic = projection[2][3] == 0.0f;

  // For glm's right-handed [0, 1] depth projections, P[2][2] and P[3][2] encode the planes: near is P32 / P22 for both, and far is P32 / (P22 + 1) for a perspective or (P32 - 1) / P22 for an orthographic projection.
  const float nearPlane = projection[3][2] / projection[2][2];
  const float farPlane  = orthographic ? (projection[3][2] - 1.0f) / projection[2][2] : projection[3][2] / (projection[2][2] + 1.0f);

  // |P11| is the vertical focal length, whatever sign the Y flip gave it, and P00 the horizontal one.
  const float verticalFocal = std::abs(projection[1][1]);

  StreamlineRuntime::RayReconstructionFrame frame {
      .cmd                   = input.cmd,
      .viewportId            = m_ViewportId,
      .color                 = DescribeImage(m_ColorImage),
      .output                = { .image = input.output.image, .view = input.output.view, .format = input.output.format, .extent = input.output.extent, .usage = input.output.usage },
      .depth                 = DescribeImage(m_DepthImage),
      .motionVectors         = DescribeImage(m_MotionVectorsImage),
      .diffuseAlbedo         = DescribeImage(m_DiffuseAlbedoImage),
      .specularAlbedo        = DescribeImage(m_SpecularAlbedoImage),
      .normalRoughness       = DescribeImage(m_NormalRoughnessImage),
      .specularMotionVectors = DescribeImage(m_SpecularMotionVectorsImage),
      .viewMatrix            = sceneInfo.viewMatrix,
      .projectionMatrix      = projection,
      .clipToPreviousClip    = sceneInfo.prevViewProjMatrix * sceneInfo.viewProjInvMatrix,
      .jitterOffset          = sceneInfo.pixelJitter,
      .nearPlane             = nearPlane,
      .farPlane              = farPlane,
      .verticalFov           = 2.0f * std::atan(1.0f / verticalFocal),
      .aspectRatio           = verticalFocal / projection[0][0],
      .orthographic          = orthographic,
      .resetHistory          = m_HistoryInvalidated || input.historyInvalidated,
      .preset                = input.settings->preset,
  };

  // Evaluate
  // A failed evaluation leaves the output with the noisy radiance the renderer already wrote there, and keeps the reset pending so the next successful frame does not blend with history from before the failure.

  const bool evaluated = m_Streamline->EvaluateRayReconstruction(frame);

  if(evaluated)
  {
    m_HasEvaluated       = true;
    m_HistoryInvalidated = false;
  }

  // Makes the denoised output visible to the compute work recorded next, such as the tonemapper.
  InsertComputeBarrier(input.cmd);

  return evaluated;
}

void RayReconstructionDenoiser::EnsureForViewport(VkExtent2D viewportSize)
{
  // Reallocating at the same size would only discard the images for no reason.
  if(m_ColorImage.image != VK_NULL_HANDLE && m_ViewportSize.width == viewportSize.width && m_ViewportSize.height == viewportSize.height)
  {
    return;
  }

  // Allocate
  // Every image is created before any is swapped in, so a failed allocation leaves the previous set intact.
  // New images hold no history, and Streamline has to be told its viewport changed size, so the history restarts.

  rtpt::Image color                 = CreateStorageImage(viewportSize, kColorFormat, "RayReconstructionColorImage");
  rtpt::Image diffuseAlbedo         = CreateStorageImage(viewportSize, kAlbedoFormat, "RayReconstructionDiffuseAlbedoImage");
  rtpt::Image specularAlbedo        = CreateStorageImage(viewportSize, kAlbedoFormat, "RayReconstructionSpecularAlbedoImage");
  rtpt::Image normalRoughness       = CreateStorageImage(viewportSize, kNormalFormat, "RayReconstructionNormalRoughnessImage");
  rtpt::Image depth                 = CreateStorageImage(viewportSize, kDepthFormat, "RayReconstructionDepthImage");
  rtpt::Image specularMotionVectors = CreateStorageImage(viewportSize, kMotionVectorsFormat, "RayReconstructionSpecularMotionVectorsImage");
  rtpt::Image motionVectors         = CreateStorageImage(viewportSize, kMotionVectorsFormat, "RayReconstructionMotionVectorsImage");

  m_ColorImage                 = std::move(color);
  m_DiffuseAlbedoImage         = std::move(diffuseAlbedo);
  m_SpecularAlbedoImage        = std::move(specularAlbedo);
  m_NormalRoughnessImage       = std::move(normalRoughness);
  m_DepthImage                 = std::move(depth);
  m_SpecularMotionVectorsImage = std::move(specularMotionVectors);
  m_MotionVectorsImage         = std::move(motionVectors);

  m_ViewportSize       = viewportSize;
  m_HistoryInvalidated = true;
}

void RayReconstructionDenoiser::DestroyViewportResources()
{
  m_ColorImage.Reset();
  m_DiffuseAlbedoImage.Reset();
  m_SpecularAlbedoImage.Reset();
  m_NormalRoughnessImage.Reset();
  m_DepthImage.Reset();
  m_SpecularMotionVectorsImage.Reset();
  m_MotionVectorsImage.Reset();

  m_ViewportSize = {};
}

rtpt::Image RayReconstructionDenoiser::CreateStorageImage(VkExtent2D viewportSize, VkFormat format, const char* debugName) const
{
  const VkImageCreateInfo imageInfo {
      .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType     = VK_IMAGE_TYPE_2D,
      .format        = format,
      .extent        = { viewportSize.width, viewportSize.height, 1 },
      .mipLevels     = 1,
      .arrayLayers   = 1,
      .samples       = VK_SAMPLE_COUNT_1_BIT,
      .tiling        = VK_IMAGE_TILING_OPTIMAL,
      .usage         = kImageUsage,
      .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  const VkImageViewCreateInfo viewInfo {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = format,
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
  };

  // descriptor.imageLayout is the CPU-side record the transition helpers read, so it starts as UNDEFINED.

  rtpt::Image image;

  rtpt::CheckVk(m_Resources->CreateImage(image, imageInfo, &viewInfo), "ResourceAllocator::CreateImage(Ray Reconstruction input)");

  image.descriptor.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  if(m_Diagnostics != nullptr)
  {
    m_Diagnostics->SetObjectName(m_Resources->Device(), VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(image.image), debugName);
  }

  return image;
}

void RayReconstructionDenoiser::InsertComputeBarrier(VkCommandBuffer cmd) const
{
  // Declares that storage writes from the previous compute dispatch are read by the next one, so the ordering between passes is explicit.
  rtpt::CmdMemoryBarrier(cmd, { .stages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT }, { .stages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT });
}

}  // namespace rtpt
