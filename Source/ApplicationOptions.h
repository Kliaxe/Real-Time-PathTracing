#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "PathTracing/Common/ResolveMode.h"

namespace rtpt
{

// RenderMode
// Selects which renderer writes the HDR viewport target each frame.
// The values double as indices into the renderer combo box in DrawRendererSection (PathTracing/PathTracerUi.cpp), so their order must match that list.

enum class RenderMode
{
  eRasterizer = 0,
  ePathTracing,
  eReSTIRPTEnhanced,
};

// ApplicationOptions
// Startup configuration parsed from the command line.
// Headless runs render a fixed number of frames and can write captures, so the options that only make sense there live alongside the interactive defaults.

struct ApplicationOptions
{
  // Runs without a window, swapchain, or UI and exits after frameCount frames.
  bool                                  headless = false;

  // Requests Vulkan synchronization validation in addition to the standard validation layer.
  bool                                  synchronizationValidation = false;

  // Frames rendered before a headless run captures and exits. Parsing rejects zero.
  uint32_t                              frameCount = 1;

  // Requested viewport width. Parsing requires width and height to be given together; the default is 1920.
  std::optional<uint32_t>               width;

  // Requested viewport height. Parsing requires width and height to be given together; the default is 1080.
  std::optional<uint32_t>               height;

  // Scene catalog index that overrides the catalog's default scene. Validated against the catalog after discovery.
  std::optional<size_t>                 sceneIndex;

  // Renderer active on the first frame. The reference path tracer, with both path tracers' resolve mode left at Off, so a first run shows the unfiltered estimator; ReSTIR and the denoisers are opt-in from the UI or the command line.
  RenderMode                            renderMode = RenderMode::ePathTracing;

  // Resolve mode applied to both path tracers at startup, so headless captures can exercise raw, accumulated, or denoised output. Empty leaves each renderer's own default, which is Off.
  std::optional<RenderResolveMode>      resolveMode;

  // Bypasses ReSTIR candidate selection, reuse, and light tiles for paired reference-path validation.
  bool                                  restirReference = false;

  // Path prefix for the .linear.hdr, .final.png, and .json capture files. Parsing only accepts it with headless.
  std::optional<std::filesystem::path>  capturePrefix;

  // JSON file that receives per-scope GPU timings and CPU frame time at the end of a headless run. Parsing only accepts it with headless, because interactive runs report timings in the UI.
  std::optional<std::filesystem::path>  profileOutput;
};

// ApplicationOptionsParseResult
// Outcome of parsing the command line.
// Parsing never throws or prints, so main decides how to report an error or show help and which exit code to use.

struct ApplicationOptionsParseResult
{
  // Options collected so far. Only meaningful when error is empty.
  ApplicationOptions options;

  // Set when --help or -h appeared anywhere on the command line.
  bool               showHelp = false;

  // First parse or validation failure. Empty means the command line was accepted.
  std::string        error;
};

ApplicationOptionsParseResult ParseApplicationOptions(int argc, char** argv);
std::string                   GetApplicationUsage(const char* executableName);

// Returns the command-line spelling of a render mode, which is also written into capture metadata.
const char*                   GetRenderModeName(RenderMode renderMode);

// Returns the command-line spelling of a resolve mode. --resolve-mode is matched against these names, so the parser and the name cannot drift apart.
const char*                   GetResolveModeName(RenderResolveMode resolveMode);

}  // namespace rtpt
