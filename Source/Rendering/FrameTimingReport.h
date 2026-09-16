#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "Rendering/FrameTimingStatistics.h"

namespace rtpt
{

// FrameTimingReportMetadata
// Run configuration written next to the timings, so a profile can be compared against another run and reproduced from the command line.
// Plain values rather than Vulkan or renderer types, so the report stays independent of the runtime that produced it.

struct FrameTimingReportMetadata
{
  // Command-line spelling of the renderer that was active for the run.
  std::string renderer;

  // Command-line spelling of that renderer's resolve mode.
  std::string resolveMode;

  // Scene index the resolver actually loaded, and its catalog label.
  size_t      sceneIndex = 0;
  std::string sceneLabel;

  // Viewport resolution the frames were rendered at.
  uint32_t    viewportWidth  = 0;
  uint32_t    viewportHeight = 0;

  // Frames the run rendered, including warm-up.
  uint32_t    frameCount = 0;

  // GPU name and the vendor-encoded driver version, because timings are only comparable on the same device and driver.
  std::string deviceName;
  uint32_t    driverVersion = 0;
};

// Writes the timing summaries as JSON (schema_version 1). Throws when the file cannot be written.
void WriteFrameTimingReport(const std::filesystem::path& path, const FrameTimingReportMetadata& metadata, const FrameTimingStatistics& statistics);

// Formats the same summaries as a fixed-width text table for the console.
[[nodiscard]] std::string FormatFrameTimingTable(const FrameTimingReportMetadata& metadata, const FrameTimingStatistics& statistics);

}  // namespace rtpt
