#include "Experiments/ExperimentPlan.h"

#include <algorithm>
#include <utility>

#include "Shaders/ShaderIo.h"

namespace nvsamples
{

namespace
{

constexpr float kEnvironmentExperimentExposure = 4.0f;
constexpr uint32_t kDepthDisocclusionDebugView = uint32_t(shaderio::eReSTIRDebugViewDepthDisocclusion);

ExperimentCamera CornellCamera()
{
  return ExperimentCamera{
      .eye    = glm::vec3(0.0f, 2.0f, 6.2f),
      .center = glm::vec3(0.0f, 1.65f, -0.25f),
      .up     = glm::vec3(0.0f, 1.0f, 0.0f),
  };
}

ExperimentCamera SponzaCamera()
{
  return ExperimentCamera{
      .eye    = glm::vec3(-2.6f, 1.25f, 0.0f),
      .center = glm::vec3(2.6f, 1.15f, 0.0f),
      .up     = glm::vec3(0.0f, 1.0f, 0.0f),
  };
}

ExperimentCaptureFrame FinalCapture(uint32_t frameIndex, std::string outputName = "final.jpg")
{
  return ExperimentCaptureFrame{.frameIndex = frameIndex, .outputName = std::move(outputName)};
}

ExperimentRun MakeReSTIRRun(std::string runName, std::string sceneLabel, ExperimentCamera camera,
                             uint32_t secondaryPathBounces, uint32_t totalFrames)
{
  ExperimentRun run{};
  run.runName     = std::move(runName);
  run.sceneLabel  = std::move(sceneLabel);
  run.renderMode  = ExperimentRenderMode::eReSTIRDI;
  run.totalFrames = totalFrames;
  run.warmupFrames = totalFrames > 0 ? totalFrames - 1 : 0;
  run.camera      = camera;
  run.environment = ExperimentEnvironment{
      .useHdri = false,
      .useSky = false,
      .backgroundColor = glm::vec3(0.0f),
  };
  run.restir.secondaryPathMaxBounces = secondaryPathBounces;
  run.restir.environmentSamples     = 0;
  run.restir.localLightSamples      = 24;
  run.restir.spatialSamples         = 5;
  run.restir.spatialRadius          = 30.0f;
  run.captures.push_back(FinalCapture(totalFrames - 1));
  return run;
}

ExperimentRun MakeCornellReSTIRRun(std::string runName, uint32_t secondaryPathBounces, uint32_t totalFrames)
{
  return MakeReSTIRRun(std::move(runName), "Cornell Box", CornellCamera(), secondaryPathBounces, totalFrames);
}

ExperimentRun MakeManyLightReSTIRRun(std::string runName, uint32_t secondaryPathBounces, uint32_t totalFrames)
{
  return MakeReSTIRRun(std::move(runName), "Cornell Many Lights", CornellCamera(), secondaryPathBounces, totalFrames);
}

ExperimentRun MakePathTracingRun(std::string runName, std::string sceneLabel, ExperimentCamera camera,
                                  uint32_t maxBounces, uint32_t totalFrames)
{
  ExperimentRun run{};
  run.runName     = std::move(runName);
  run.sceneLabel  = std::move(sceneLabel);
  run.renderMode  = ExperimentRenderMode::ePathTracing;
  run.totalFrames = totalFrames;
  run.warmupFrames = totalFrames > 0 ? totalFrames - 1 : 0;
  run.camera      = camera;
  run.environment = ExperimentEnvironment{
      .useHdri = false,
      .useSky = false,
      .backgroundColor = glm::vec3(0.0f),
  };
  run.pathTracing.resolveMode = ExperimentResolveMode::eAccumulate;
  run.pathTracing.maxBounces  = maxBounces;
  run.captures.push_back(FinalCapture(totalFrames - 1));
  return run;
}

ExperimentRun MakePathTracingBaselineRun(std::string runName, std::string sceneLabel, ExperimentCamera camera,
                                          uint32_t maxBounces, uint32_t totalFrames)
{
  ExperimentRun run = MakePathTracingRun(std::move(runName), std::move(sceneLabel), camera, maxBounces, totalFrames);
  run.pathTracing.resolveMode = ExperimentResolveMode::eOff;
  return run;
}

ExperimentRun MakePathTracingDenoisedRun(std::string runName, std::string sceneLabel, ExperimentCamera camera,
                                          uint32_t maxBounces, uint32_t totalFrames)
{
  ExperimentRun run = MakePathTracingRun(std::move(runName), std::move(sceneLabel), camera, maxBounces, totalFrames);
  run.pathTracing.resolveMode = ExperimentResolveMode::eDenoise;
  return run;
}

ExperimentRun MakeAccumulatedDirectBaselineRun(std::string runName, uint32_t accumulatedSamples)
{
  ExperimentRun run = MakePathTracingRun(std::move(runName), "Cornell Many Lights", CornellCamera(), 0, accumulatedSamples);
  run.pathTracing.resolveMode = ExperimentResolveMode::eAccumulate;
  return run;
}

ExperimentRun MakeAccumulatedSecondaryBounceRun(std::string runName, uint32_t accumulatedSamples)
{
  ExperimentRun run = MakePathTracingRun(std::move(runName), "Cornell Many Lights", CornellCamera(), 3, accumulatedSamples);
  run.pathTracing.resolveMode = ExperimentResolveMode::eAccumulate;
  return run;
}

ExperimentRun MakeReSTIRReuseRun(std::string runName, bool temporalReuse, bool spatialReuse)
{
  ExperimentRun run         = MakeCornellReSTIRRun(std::move(runName), 0, 32);
  run.restir.temporalReuse  = temporalReuse;
  run.restir.spatialReuse   = spatialReuse;
  return run;
}

ExperimentRun MakeReSTIRCandidateRun(std::string runName, uint32_t localLightSamples)
{
  ExperimentRun run            = MakeManyLightReSTIRRun(std::move(runName), 0, 32);
  run.restir.localLightSamples = localLightSamples;
  return run;
}

ExperimentRun MakeEnvironmentReSTIRRun(std::string runName, uint32_t environmentSamples)
{
  ExperimentRun run = MakeReSTIRRun(std::move(runName), "Sponza Studio", SponzaCamera(), 0, 32);
  run.environment = ExperimentEnvironment{
      .useHdri         = true,
      .useSky          = false,
      .backgroundColor = glm::vec3(0.0f),
      .hdriLabelOrPath = "HDRI/SymmetricalGarden.hdr",
  };
  run.restir.localLightSamples   = 0;
  run.restir.environmentSamples  = environmentSamples;
  run.restir.spatialRadius       = 45.0f;
  run.tonemapperExposure         = kEnvironmentExperimentExposure;
  return run;
}

ExperimentRun MakeMovingEnvironmentReSTIRRun(std::string runName, uint32_t environmentSamples)
{
  ExperimentRun run = MakeEnvironmentReSTIRRun(std::move(runName), environmentSamples);
  run.totalFrames   = 24;
  run.warmupFrames  = 0;
  run.camera        = ExperimentCamera{
      .eye    = glm::vec3(-2.95f, 1.25f, -0.45f),
      .center = glm::vec3(2.35f, 1.10f, 0.25f),
      .up     = glm::vec3(0.0f, 1.0f, 0.0f),
  };
  run.animateCamera     = true;
  run.restir.debugView  = kDepthDisocclusionDebugView;
  run.endCamera     = ExperimentCamera{
      .eye    = glm::vec3(-2.25f, 1.25f, 0.45f),
      .center = glm::vec3(2.85f, 1.10f, -0.25f),
      .up     = glm::vec3(0.0f, 1.0f, 0.0f),
  };
  run.captures.clear();
  run.captures.push_back(FinalCapture(7, "frame-007.jpg"));
  run.captures.push_back(FinalCapture(12, "frame-012.jpg"));
  run.captures.push_back(FinalCapture(18, "frame-018.jpg"));
  return run;
}

ExperimentPlan CreateSmokePlan(const std::filesystem::path& outputRoot)
{
  ExperimentPlan plan{};
  plan.name       = "restir-smoke";
  plan.outputRoot = outputRoot;
  plan.runs.push_back(MakeCornellReSTIRRun("restir-smoke", 0, 16));
  return plan;
}

ExperimentPlan CreateMinimalPlan(const std::filesystem::path& outputRoot)
{
  ExperimentPlan plan{};
  plan.name       = "restir-di-minimal";
  plan.outputRoot = outputRoot;
  plan.runs.push_back(MakePathTracingRun("path-tracing-reference", "Cornell Box", CornellCamera(), 1, 64));
  plan.runs.push_back(MakePathTracingBaselineRun("path-tracing-direct-baseline", "Cornell Box", CornellCamera(), 0, 32));
  plan.runs.push_back(MakeCornellReSTIRRun("restir-di-pure", 0, 32));
  plan.runs.push_back(MakeCornellReSTIRRun("restir-hybrid", 3, 32));
  return plan;
}

ExperimentPlan CreateReuseModesPlan(const std::filesystem::path& outputRoot)
{
  ExperimentPlan plan{};
  plan.name       = "restir-reuse-modes";
  plan.outputRoot = outputRoot;
  plan.runs.push_back(MakeReSTIRReuseRun("restir-no-reuse", false, false));
  plan.runs.push_back(MakeReSTIRReuseRun("restir-temporal-only", true, false));
  plan.runs.push_back(MakeReSTIRReuseRun("restir-spatial-only", false, true));
  plan.runs.push_back(MakeReSTIRReuseRun("restir-temporal-spatial", true, true));
  return plan;
}

ExperimentPlan CreateCandidateSweepPlan(const std::filesystem::path& outputRoot)
{
  ExperimentPlan plan{};
  plan.name       = "restir-candidate-sweep";
  plan.outputRoot = outputRoot;
  plan.runs.push_back(MakeReSTIRCandidateRun("restir-1-local-light-sample", 1));
  plan.runs.push_back(MakeReSTIRCandidateRun("restir-4-local-light-samples", 4));
  plan.runs.push_back(MakeReSTIRCandidateRun("restir-8-local-light-samples", 8));
  plan.runs.push_back(MakeReSTIRCandidateRun("restir-24-local-light-samples", 24));
  return plan;
}

ExperimentPlan CreateManyLightPlan(const std::filesystem::path& outputRoot)
{
  ExperimentPlan plan{};
  plan.name       = "restir-many-light";
  plan.outputRoot = outputRoot;
  plan.runs.push_back(MakePathTracingRun("path-tracing-many-light-reference", "Cornell Many Lights", CornellCamera(), 1, 96));
  plan.runs.push_back(
      MakePathTracingBaselineRun("path-tracing-many-light-direct-baseline", "Cornell Many Lights", CornellCamera(), 0, 32));
  plan.runs.push_back(MakeManyLightReSTIRRun("restir-many-light-1-sample", 0, 32));
  plan.runs.push_back(MakeManyLightReSTIRRun("restir-many-light-8-samples", 0, 32));
  plan.runs.push_back(MakeManyLightReSTIRRun("restir-many-light-24-samples", 0, 32));
  plan.runs.back().restir.localLightSamples = 24;
  plan.runs[3].restir.localLightSamples     = 8;
  plan.runs[2].restir.localLightSamples     = 1;
  return plan;
}

ExperimentPlan CreateEqualQualityPlan(const std::filesystem::path& outputRoot)
{
  ExperimentPlan plan{};
  plan.name       = "restir-equal-quality";
  plan.outputRoot = outputRoot;

  plan.runs.push_back(MakeAccumulatedDirectBaselineRun("direct-baseline-1-sample", 1));
  plan.runs.push_back(MakeAccumulatedDirectBaselineRun("direct-baseline-8-samples", 8));
  plan.runs.push_back(MakeAccumulatedDirectBaselineRun("direct-baseline-24-samples", 24));
  plan.runs.push_back(MakeAccumulatedDirectBaselineRun("direct-baseline-96-samples", 96));
  plan.runs.push_back(MakeReSTIRCandidateRun("restir-1-local-light-sample", 1));
  plan.runs.push_back(MakeReSTIRCandidateRun("restir-8-local-light-samples", 8));
  plan.runs.push_back(MakeReSTIRCandidateRun("restir-24-local-light-samples", 24));
  return plan;
}

ExperimentPlan CreateSecondaryBounceQualityPlan(const std::filesystem::path& outputRoot)
{
  ExperimentPlan plan{};
  plan.name       = "restir-secondary-bounce-quality";
  plan.outputRoot = outputRoot;

  // Compare warmed hybrid ReSTIR against brute-force path tracing with the same bounce count.
  plan.runs.push_back(MakeAccumulatedSecondaryBounceRun("path-tracing-3-bounce-1-frame", 1));
  plan.runs.push_back(MakeAccumulatedSecondaryBounceRun("path-tracing-3-bounce-8-frames", 8));
  plan.runs.push_back(MakeAccumulatedSecondaryBounceRun("path-tracing-3-bounce-24-frames", 24));
  plan.runs.push_back(MakeAccumulatedSecondaryBounceRun("path-tracing-3-bounce-48-frames", 48));
  plan.runs.push_back(MakeAccumulatedSecondaryBounceRun("path-tracing-3-bounce-96-frames", 96));

  ExperimentRun restir = MakeManyLightReSTIRRun("hybrid-restir-3-bounce-8-local-light-samples", 3, 32);
  restir.restir.localLightSamples = 8;
  plan.runs.push_back(restir);

  ExperimentRun restirHighCandidate = MakeManyLightReSTIRRun("hybrid-restir-3-bounce-24-local-light-samples", 3, 32);
  restirHighCandidate.restir.localLightSamples = 24;
  plan.runs.push_back(restirHighCandidate);
  return plan;
}

ExperimentPlan CreateDenoisingComparisonPlan(const std::filesystem::path& outputRoot)
{
  ExperimentPlan plan{};
  plan.name       = "restir-denoising-comparison";
  plan.outputRoot = outputRoot;

  plan.runs.push_back(
      MakePathTracingBaselineRun("path-tracing-direct-raw", "Cornell Many Lights", CornellCamera(), 0, 32));
  plan.runs.push_back(
      MakePathTracingDenoisedRun("path-tracing-direct-denoised", "Cornell Many Lights", CornellCamera(), 0, 32));

  ExperimentRun restirDirectRaw = MakeManyLightReSTIRRun("restir-direct-raw", 0, 32);
  restirDirectRaw.restir.localLightSamples = 8;
  plan.runs.push_back(restirDirectRaw);

  ExperimentRun restirDirectDenoised = MakeManyLightReSTIRRun("restir-direct-denoised", 0, 32);
  restirDirectDenoised.restir.resolveMode       = ExperimentResolveMode::eDenoise;
  restirDirectDenoised.restir.localLightSamples = 8;
  plan.runs.push_back(restirDirectDenoised);

  plan.runs.push_back(
      MakePathTracingBaselineRun("path-tracing-3-bounce-raw", "Cornell Many Lights", CornellCamera(), 3, 32));
  plan.runs.push_back(
      MakePathTracingDenoisedRun("path-tracing-3-bounce-denoised", "Cornell Many Lights", CornellCamera(), 3, 32));

  ExperimentRun restirHybridRaw = MakeManyLightReSTIRRun("hybrid-restir-3-bounce-raw", 3, 32);
  restirHybridRaw.restir.localLightSamples = 8;
  plan.runs.push_back(restirHybridRaw);

  ExperimentRun restirHybridDenoised = MakeManyLightReSTIRRun("hybrid-restir-3-bounce-denoised", 3, 32);
  restirHybridDenoised.restir.resolveMode       = ExperimentResolveMode::eDenoise;
  restirHybridDenoised.restir.localLightSamples = 8;
  plan.runs.push_back(restirHybridDenoised);

  return plan;
}

ExperimentPlan CreateSecondaryPathPlan(const std::filesystem::path& outputRoot)
{
  ExperimentPlan plan{};
  plan.name       = "restir-secondary-path";
  plan.outputRoot = outputRoot;
  plan.runs.push_back(MakePathTracingRun("path-tracing-3-bounce-reference", "Cornell Box", CornellCamera(), 3, 96));
  plan.runs.push_back(
      MakePathTracingBaselineRun("path-tracing-3-bounce-baseline", "Cornell Box", CornellCamera(), 3, 32));
  plan.runs.push_back(MakeCornellReSTIRRun("restir-direct-only", 0, 32));
  plan.runs.push_back(MakeCornellReSTIRRun("restir-1-secondary-path-bounce", 1, 32));
  plan.runs.push_back(MakeCornellReSTIRRun("restir-3-secondary-path-bounces", 3, 32));
  return plan;
}

ExperimentPlan CreateEnvironmentPlan(const std::filesystem::path& outputRoot)
{
  ExperimentPlan plan{};
  plan.name       = "restir-environment";
  plan.outputRoot = outputRoot;

  ExperimentRun reference = MakePathTracingRun("path-tracing-hdri-reference", "Sponza Studio", SponzaCamera(), 1, 96);
  reference.environment = ExperimentEnvironment{
      .useHdri         = true,
      .useSky          = false,
      .backgroundColor = glm::vec3(0.0f),
      .hdriLabelOrPath = "HDRI/SymmetricalGarden.hdr",
  };
  reference.tonemapperExposure = kEnvironmentExperimentExposure;
  plan.runs.push_back(reference);
  ExperimentRun baseline =
      MakePathTracingBaselineRun("path-tracing-hdri-direct-baseline", "Sponza Studio", SponzaCamera(), 0, 32);
  baseline.environment = reference.environment;
  baseline.tonemapperExposure = kEnvironmentExperimentExposure;
  plan.runs.push_back(baseline);
  plan.runs.push_back(MakeEnvironmentReSTIRRun("restir-hdri-1-sample", 1));
  plan.runs.push_back(MakeEnvironmentReSTIRRun("restir-hdri-4-samples", 4));
  plan.runs.push_back(MakeEnvironmentReSTIRRun("restir-hdri-8-samples", 8));
  return plan;
}

ExperimentPlan CreateEnvironmentTemporalPlan(const std::filesystem::path& outputRoot)
{
  ExperimentPlan plan{};
  plan.name       = "restir-environment-temporal";
  plan.outputRoot = outputRoot;
  plan.runs.push_back(MakeMovingEnvironmentReSTIRRun("restir-hdri-temporal-2-samples", 2));
  plan.runs.push_back(MakeMovingEnvironmentReSTIRRun("restir-hdri-temporal-3-samples", 3));
  return plan;
}

ExperimentPlan CreateTemporalCameraPlan(const std::filesystem::path& outputRoot)
{
  ExperimentPlan plan{};
  plan.name       = "restir-temporal-camera";
  plan.outputRoot = outputRoot;

  ExperimentRun run = MakeManyLightReSTIRRun("restir-temporal-camera", 0, 24);
  run.warmupFrames  = 0;
  run.camera        = ExperimentCamera{
      .eye    = glm::vec3(-1.55f, 1.45f, 3.15f),
      .center = glm::vec3(0.0f, 1.10f, -0.45f),
      .up     = glm::vec3(0.0f, 1.0f, 0.0f),
  };
  run.animateCamera     = true;
  run.restir.debugView  = kDepthDisocclusionDebugView;
  run.endCamera     = ExperimentCamera{
      .eye    = glm::vec3(1.55f, 1.45f, 3.15f),
      .center = glm::vec3(0.0f, 1.10f, -0.45f),
      .up     = glm::vec3(0.0f, 1.0f, 0.0f),
  };
  run.captures.clear();
  run.captures.push_back(FinalCapture(7, "frame-007.jpg"));
  run.captures.push_back(FinalCapture(12, "frame-012.jpg"));
  run.captures.push_back(FinalCapture(18, "frame-018.jpg"));
  plan.runs.push_back(run);
  return plan;
}

}  // namespace

std::optional<ExperimentPlan> CreateNamedExperimentPlan(std::string_view name, const std::filesystem::path& outputRoot)
{
  if(name == "restir-smoke")
  {
    return CreateSmokePlan(outputRoot);
  }
  if(name == "restir-di-minimal")
  {
    return CreateMinimalPlan(outputRoot);
  }
  if(name == "restir-reuse-modes")
  {
    return CreateReuseModesPlan(outputRoot);
  }
  if(name == "restir-candidate-sweep")
  {
    return CreateCandidateSweepPlan(outputRoot);
  }
  if(name == "restir-many-light")
  {
    return CreateManyLightPlan(outputRoot);
  }
  if(name == "restir-equal-quality")
  {
    return CreateEqualQualityPlan(outputRoot);
  }
  if(name == "restir-secondary-bounce-quality")
  {
    return CreateSecondaryBounceQualityPlan(outputRoot);
  }
  if(name == "restir-denoising-comparison")
  {
    return CreateDenoisingComparisonPlan(outputRoot);
  }
  if(name == "restir-secondary-path")
  {
    return CreateSecondaryPathPlan(outputRoot);
  }
  if(name == "restir-environment")
  {
    return CreateEnvironmentPlan(outputRoot);
  }
  if(name == "restir-environment-temporal")
  {
    return CreateEnvironmentTemporalPlan(outputRoot);
  }
  if(name == "restir-temporal-camera")
  {
    return CreateTemporalCameraPlan(outputRoot);
  }
  return std::nullopt;
}

std::vector<std::string_view> GetAvailableExperimentPlanNames()
{
  return {"restir-smoke",           "restir-di-minimal",       "restir-reuse-modes",
          "restir-candidate-sweep", "restir-many-light",       "restir-secondary-path",
          "restir-equal-quality",   "restir-secondary-bounce-quality",
          "restir-denoising-comparison",
          "restir-environment",     "restir-environment-temporal", "restir-temporal-camera"};
}

uint32_t GetRequiredHeadlessFrameCount(const ExperimentPlan& plan)
{
  uint32_t frameCount = 0;
  for(const ExperimentRun& run : plan.runs)
  {
    frameCount += std::max(1u, run.totalFrames);
  }
  return std::max(1u, frameCount);
}

}  // namespace nvsamples
