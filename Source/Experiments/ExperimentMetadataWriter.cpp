#include "Experiments/ExperimentMetadataWriter.h"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace nvsamples
{

namespace
{

std::string EscapeJson(std::string_view text)
{
  std::string escaped;
  escaped.reserve(text.size());
  for(char c : text)
  {
    if(c == '\\' || c == '"')
    {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  return escaped;
}

const char* ToString(ExperimentRenderMode mode)
{
  switch(mode)
  {
    case ExperimentRenderMode::ePathTracing:
      return "PathTracing";
    case ExperimentRenderMode::eReSTIRDI:
      return "ReSTIRDI";
  }
  return "Unknown";
}

const char* ToString(ExperimentResolveMode mode)
{
  switch(mode)
  {
    case ExperimentResolveMode::eOff:
      return "Off";
    case ExperimentResolveMode::eAccumulate:
      return "Accumulate";
    case ExperimentResolveMode::eDenoise:
      return "Denoise";
  }
  return "Unknown";
}

std::string GenericPath(const std::filesystem::path& path)
{
  return path.generic_string();
}

std::filesystem::path RunOutputFolder(const ExperimentPlan& plan, const ExperimentRun& run)
{
  return plan.outputRoot / "captures" / run.runName;
}

void WriteVec3(std::ofstream& file, const char* name, const glm::vec3& value, const char* suffix)
{
  file << "  \"" << name << "\": [" << value.x << ", " << value.y << ", " << value.z << "]" << suffix << "\n";
}

void WriteTimingValue(std::ofstream& file, const char* name, const std::optional<ExperimentGpuTimings>& timings,
                      double ExperimentGpuTimings::*member, const char* suffix)
{
  file << "    \"" << name << "\": ";
  if(timings)
  {
    file << std::fixed << std::setprecision(4) << ((*timings).*member);
  }
  else
  {
    file << "null";
  }
  file << suffix << "\n";
}

void WriteCsvTimingValue(std::ofstream& file, const std::optional<ExperimentGpuTimings>& timings,
                         double ExperimentGpuTimings::*member)
{
  if(timings)
  {
    file << std::fixed << std::setprecision(4) << ((*timings).*member);
  }
}

}  // namespace

void ExperimentMetadataWriter::WriteManifest(const ExperimentPlan& plan)
{
  std::filesystem::create_directories(plan.outputRoot);

  std::ofstream file(plan.outputRoot / "manifest.json", std::ios::trunc);
  file << "{\n";
  file << "  \"schemaVersion\": 2,\n";
  file << "  \"plan\": \"" << EscapeJson(plan.name) << "\",\n";
  file << "  \"runCount\": " << plan.runs.size() << ",\n";
  file << "  \"runs\": [\n";
  for(size_t i = 0; i < plan.runs.size(); ++i)
  {
    const ExperimentRun& run = plan.runs[i];
    file << "    {\n";
    file << "      \"name\": \"" << EscapeJson(run.runName) << "\",\n";
    file << "      \"scene\": \"" << EscapeJson(run.sceneLabel) << "\",\n";
    file << "      \"renderMode\": \"" << ToString(run.renderMode) << "\",\n";
    file << "      \"totalFrames\": " << run.totalFrames << ",\n";
    file << "      \"warmupFrames\": " << run.warmupFrames << "\n";
    file << "    }" << (i + 1 == plan.runs.size() ? "\n" : ",\n");
  }
  file << "  ]\n";
  file << "}\n";

  std::ofstream summary(plan.outputRoot / "summary.csv", std::ios::trunc);
  summary << "run,scene,render_mode,resolve_mode,capture_frame,image,accumulated_frames,total_frames,warmup_frames,"
             "restir_local_light_samples,restir_environment_samples,restir_brdf_samples,restir_temporal_reuse,"
             "restir_spatial_reuse,restir_spatial_samples,restir_spatial_radius,restir_continuation_bounces,"
             "restir_debug_view,path_tracing_max_bounces,gpu_frame_ms,gpu_renderer_ms,gpu_post_process_ms\n";
}

void ExperimentMetadataWriter::WriteCaptureMetadata(const ExperimentPlan& plan,
                                                    const ExperimentRun& run,
                                                    const ExperimentCaptureFrame& capture,
                                                    const std::filesystem::path& imagePath,
                                                    uint32_t accumulatedFrames,
                                                    const std::optional<ExperimentGpuTimings>& gpuTimings)
{
  const std::filesystem::path runFolder = RunOutputFolder(plan, run);
  std::filesystem::create_directories(runFolder);

  const std::filesystem::path metadataPath = runFolder / (imagePath.stem().string() + ".metadata.json");
  std::ofstream               file(metadataPath, std::ios::trunc);
  file << "{\n";
  file << "  \"schemaVersion\": 2,\n";
  file << "  \"plan\": \"" << EscapeJson(plan.name) << "\",\n";
  file << "  \"run\": \"" << EscapeJson(run.runName) << "\",\n";
  file << "  \"scene\": \"" << EscapeJson(run.sceneLabel) << "\",\n";
  file << "  \"renderMode\": \"" << ToString(run.renderMode) << "\",\n";
  file << "  \"captureFrame\": " << capture.frameIndex << ",\n";
  file << "  \"totalFrames\": " << run.totalFrames << ",\n";
  file << "  \"warmupFrames\": " << run.warmupFrames << ",\n";
  file << "  \"accumulatedFrames\": " << accumulatedFrames << ",\n";
  file << "  \"image\": \"" << EscapeJson(GenericPath(imagePath)) << "\",\n";
  file << "  \"environment\": {\n";
  file << "    \"useHdri\": " << (run.environment.useHdri ? "true" : "false") << ",\n";
  file << "    \"useSky\": " << (run.environment.useSky ? "true" : "false") << ",\n";
  file << "    \"hdriLabelOrPath\": \"" << EscapeJson(run.environment.hdriLabelOrPath) << "\",\n";
  WriteVec3(file, "backgroundColor", run.environment.backgroundColor, "\n");
  file << "  },\n";
  file << "  \"tonemapper\": {\n";
  file << "    \"exposure\": " << run.tonemapperExposure << "\n";
  file << "  },\n";
  const ExperimentCamera endCamera = run.animateCamera ? run.endCamera : run.camera;
  file << "  \"camera\": {\n";
  file << "    \"animate\": " << (run.animateCamera ? "true" : "false") << ",\n";
  WriteVec3(file, "startEye", run.camera.eye, ",");
  WriteVec3(file, "startCenter", run.camera.center, ",");
  WriteVec3(file, "startUp", run.camera.up, ",");
  WriteVec3(file, "endEye", endCamera.eye, ",");
  WriteVec3(file, "endCenter", endCamera.center, ",");
  WriteVec3(file, "endUp", endCamera.up, "\n");
  file << "  },\n";
  if(run.renderMode == ExperimentRenderMode::ePathTracing)
  {
    file << "  \"pathTracing\": {\n";
    file << "    \"resolveMode\": \"" << ToString(run.pathTracing.resolveMode) << "\",\n";
    file << "    \"maxBounces\": " << run.pathTracing.maxBounces << "\n";
    file << "  },\n";
  }
  else
  {
    file << "  \"pathTracing\": null,\n";
  }
  if(run.renderMode == ExperimentRenderMode::eReSTIRDI)
  {
    file << "  \"restir\": {\n";
    file << "    \"resolveMode\": \"" << ToString(run.restir.resolveMode) << "\",\n";
    file << "    \"localLightSamples\": " << run.restir.localLightSamples << ",\n";
    file << "    \"environmentSamples\": " << run.restir.environmentSamples << ",\n";
    file << "    \"brdfSamples\": " << run.restir.brdfSamples << ",\n";
    file << "    \"temporalReuse\": " << (run.restir.temporalReuse ? "true" : "false") << ",\n";
    file << "    \"spatialReuse\": " << (run.restir.spatialReuse ? "true" : "false") << ",\n";
    file << "    \"spatialSamples\": " << run.restir.spatialSamples << ",\n";
    file << "    \"spatialRadius\": " << run.restir.spatialRadius << ",\n";
    file << "    \"initialVisibility\": " << (run.restir.initialVisibility ? "true" : "false") << ",\n";
    file << "    \"finalVisibility\": " << (run.restir.finalVisibility ? "true" : "false") << ",\n";
    file << "    \"reuseFinalVisibility\": " << (run.restir.reuseFinalVisibility ? "true" : "false") << ",\n";
    file << "    \"debugView\": " << run.restir.debugView << ",\n";
    file << "    \"continuationMaxBounces\": " << run.restir.continuationMaxBounces << "\n";
    file << "  },\n";
  }
  else
  {
    file << "  \"restir\": null,\n";
  }
  file << "  \"gpuTimings\": {\n";
  WriteTimingValue(file, "frameMs", gpuTimings, &ExperimentGpuTimings::frameMs, ",");
  WriteTimingValue(file, "rendererMs", gpuTimings, &ExperimentGpuTimings::rendererMs, ",");
  WriteTimingValue(file, "postProcessMs", gpuTimings, &ExperimentGpuTimings::postProcessMs, "\n");
  file << "  }\n";
  file << "}\n";
}

void ExperimentMetadataWriter::AppendSummaryRow(const ExperimentPlan& plan,
                                                const ExperimentRun& run,
                                                const ExperimentCaptureFrame& capture,
                                                const std::filesystem::path& imagePath,
                                                uint32_t accumulatedFrames,
                                                const std::optional<ExperimentGpuTimings>& gpuTimings)
{
  std::ofstream summary(plan.outputRoot / "summary.csv", std::ios::app);
  summary << '"' << EscapeJson(run.runName) << "\",";
  summary << '"' << EscapeJson(run.sceneLabel) << "\",";
  summary << ToString(run.renderMode) << ",";
  summary << (run.renderMode == ExperimentRenderMode::ePathTracing ? ToString(run.pathTracing.resolveMode) : ToString(run.restir.resolveMode)) << ",";
  summary << capture.frameIndex << ",";
  summary << '"' << EscapeJson(GenericPath(imagePath)) << "\",";
  summary << accumulatedFrames << ",";
  summary << run.totalFrames << ",";
  summary << run.warmupFrames << ",";
  summary << run.restir.localLightSamples << ",";
  summary << run.restir.environmentSamples << ",";
  summary << run.restir.brdfSamples << ",";
  summary << (run.restir.temporalReuse ? "true" : "false") << ",";
  summary << (run.restir.spatialReuse ? "true" : "false") << ",";
  summary << run.restir.spatialSamples << ",";
  summary << run.restir.spatialRadius << ",";
  summary << run.restir.continuationMaxBounces << ",";
  summary << run.restir.debugView << ",";
  summary << run.pathTracing.maxBounces << ",";
  WriteCsvTimingValue(summary, gpuTimings, &ExperimentGpuTimings::frameMs);
  summary << ",";
  WriteCsvTimingValue(summary, gpuTimings, &ExperimentGpuTimings::rendererMs);
  summary << ",";
  WriteCsvTimingValue(summary, gpuTimings, &ExperimentGpuTimings::postProcessMs);
  summary << "\n";
}

}  // namespace nvsamples
