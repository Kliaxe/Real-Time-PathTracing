#pragma once

#include <filesystem>
#include <optional>

#include "Experiments/ExperimentTypes.h"

namespace nvsamples
{

class ExperimentMetadataWriter
{
public:
  static void WriteManifest(const ExperimentPlan& plan);
  static void WriteCaptureMetadata(const ExperimentPlan& plan,
                                   const ExperimentRun& run,
                                   const ExperimentCaptureFrame& capture,
                                   const std::filesystem::path& imagePath,
                                   uint32_t accumulatedFrames,
                                   const std::optional<ExperimentGpuTimings>& gpuTimings);
  static void AppendSummaryRow(const ExperimentPlan& plan,
                               const ExperimentRun& run,
                               const ExperimentCaptureFrame& capture,
                               const std::filesystem::path& imagePath,
                               uint32_t accumulatedFrames,
                               const std::optional<ExperimentGpuTimings>& gpuTimings);
};

}  // namespace nvsamples
