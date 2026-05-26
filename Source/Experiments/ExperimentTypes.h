#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

namespace nvsamples
{

enum class ExperimentRenderMode
{
  ePathTracing,
  eReSTIRDI,
};

enum class ExperimentResolveMode
{
  eOff,
  eAccumulate,
  eDenoise,
};

struct ExperimentCamera
{
  glm::vec3 eye    = glm::vec3(0.0f, 0.5f, 5.0f);
  glm::vec3 center = glm::vec3(0.0f, 0.0f, 0.0f);
  glm::vec3 up     = glm::vec3(0.0f, 1.0f, 0.0f);
};

struct ExperimentCaptureFrame
{
  uint32_t    frameIndex = 0;
  std::string outputName = "final.jpg";
};

struct ExperimentGpuTimings
{
  double frameMs       = 0.0;
  double rendererMs    = 0.0;
  double postProcessMs = 0.0;
};

struct ExperimentPathTracerSettings
{
  ExperimentResolveMode resolveMode = ExperimentResolveMode::eOff;
  uint32_t              maxBounces  = 1;
};

struct ExperimentReSTIRSettings
{
  ExperimentResolveMode resolveMode           = ExperimentResolveMode::eOff;
  uint32_t              localLightSamples     = 24;
  uint32_t              environmentSamples    = 0;
  uint32_t              brdfSamples           = 0;
  bool                  temporalReuse         = true;
  bool                  spatialReuse          = true;
  uint32_t              spatialSamples        = 5;
  float                 spatialRadius         = 30.0f;
  bool                  initialVisibility     = true;
  bool                  finalVisibility       = true;
  bool                  reuseFinalVisibility  = true;
  uint32_t              debugView             = 0;
  uint32_t              secondaryPathMaxBounces = 0;
};

struct ExperimentEnvironment
{
  bool      useHdri         = false;
  bool      useSky          = false;
  glm::vec3 backgroundColor = glm::vec3(0.0f);
  std::string hdriLabelOrPath;
};

struct ExperimentRun
{
  std::string runName;
  std::string sceneLabel;

  ExperimentRenderMode renderMode = ExperimentRenderMode::eReSTIRDI;
  ExperimentCamera     camera;
  bool                 animateCamera = false;
  ExperimentCamera     endCamera;
  ExperimentEnvironment environment;

  uint32_t totalFrames  = 16;
  uint32_t warmupFrames = 15;
  float    tonemapperExposure = 1.0f;

  ExperimentPathTracerSettings pathTracing;
  ExperimentReSTIRSettings     restir;
  std::vector<ExperimentCaptureFrame> captures;
};

struct ExperimentPlan
{
  std::string name;
  std::filesystem::path outputRoot;
  std::vector<ExperimentRun> runs;
};

}  // namespace nvsamples
