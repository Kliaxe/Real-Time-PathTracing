#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "ApplicationOptions.h"
#include "Camera/CameraState.h"
#include "Framework/Vulkan/GpuExecution.h"
#include "Framework/Vulkan/GpuResources.h"
#include "PostProcessing/Tonemapper.h"
#include "Rendering/ViewportTargets.h"

namespace rtpt
{

// FrameCaptureMetadata
// Run state recorded in a capture's JSON sidecar that ApplicationOptions does not already hold.
// The renderer and scene are passed as they were when the last frame rendered, because the catalog can resolve them differently from the command line.

struct FrameCaptureMetadata
{
  // Renderer that wrote the captured HDR target.
  RenderMode         renderMode = RenderMode::eReSTIRPTEnhanced;

  // Resolve mode of that renderer, read from its settings because a renderer keeps its own default when --resolve-mode is absent. The rasterizer has no resolve step and reports Off.
  RenderResolveMode  resolveMode = RenderResolveMode::eOff;

  // Scene index the resolver actually loaded, and its catalog label.
  size_t             sceneIndex = 0;
  std::string        sceneLabel;

  // Camera the last frame rendered with.
  CameraState        camera;

  // Tonemapper settings that produced the LDR target.
  TonemapperSettings tonemapper;
};

// Reads back the HDR and LDR viewport targets and writes <prefix>.linear.hdr, <prefix>.final.png, and <prefix>.json.
// Blocks on the GPU, so it belongs at the end of a headless run after every frame has finished. Both targets must be in GENERAL and are left in TRANSFER_SRC_OPTIMAL.
// Throws when any of the three files is missing or empty afterwards, so a capture is never reported as written when it was not.
void WriteFrameCapture(const ResourceAllocator& resources, GpuExecution& execution, VkPhysicalDevice physicalDevice, const ViewportTargets& targets, const std::filesystem::path& prefix, const ApplicationOptions& options, const FrameCaptureMetadata& metadata);

}  // namespace rtpt
