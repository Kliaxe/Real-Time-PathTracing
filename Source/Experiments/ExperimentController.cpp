#include "Experiments/ExperimentController.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include "Application.h"
#include "Experiments/ExperimentMetadataWriter.h"
#include "Experiments/ExperimentPlan.h"

#include <nvutils/logger.hpp>

namespace nvsamples
{

namespace
{

ExperimentCamera InterpolateCamera(const ExperimentRun& run, uint32_t frameInRun)
{
  if(!run.animateCamera || run.totalFrames <= 1)
  {
    return run.camera;
  }

  const float t = static_cast<float>(frameInRun) / static_cast<float>(run.totalFrames - 1u);
  ExperimentCamera camera{};
  camera.eye    = run.camera.eye + (run.endCamera.eye - run.camera.eye) * t;
  camera.center = run.camera.center + (run.endCamera.center - run.camera.center) * t;
  camera.up     = run.camera.up + (run.endCamera.up - run.camera.up) * t;
  return camera;
}

bool ValidateExperimentPlan(const ExperimentPlan& plan)
{
  std::set<std::string> runNames;
  for(const ExperimentRun& run : plan.runs)
  {
    if(run.runName.empty() || run.sceneLabel.empty())
    {
      LOGE("Experiment plan '%s' contains an empty run name or scene label.\n", plan.name.c_str());
      return false;
    }
    if(!runNames.insert(run.runName).second)
    {
      LOGE("Experiment plan '%s' contains duplicate run name '%s'.\n", plan.name.c_str(), run.runName.c_str());
      return false;
    }
    if(run.totalFrames == 0)
    {
      LOGE("Experiment run '%s' has totalFrames = 0.\n", run.runName.c_str());
      return false;
    }

    std::set<std::string> outputNames;
    for(const ExperimentCaptureFrame& capture : run.captures)
    {
      if(capture.frameIndex >= run.totalFrames)
      {
        LOGE("Experiment run '%s' captures frame %u, but totalFrames is %u.\n", run.runName.c_str(), capture.frameIndex,
             run.totalFrames);
        return false;
      }
      if(!outputNames.insert(capture.outputName).second)
      {
        LOGE("Experiment run '%s' writes duplicate capture '%s'.\n", run.runName.c_str(), capture.outputName.c_str());
        return false;
      }
    }
  }
  return true;
}

}  // namespace

ExperimentController::ExperimentController(ExperimentPlan plan)
    : m_Plan(std::move(plan))
{
}

const ExperimentPlan& ExperimentController::GetPlan() const
{
  return m_Plan;
}

uint32_t ExperimentController::GetRequiredHeadlessFrameCount() const
{
  return nvsamples::GetRequiredHeadlessFrameCount(m_Plan);
}

void ExperimentController::BeforeRender(Application& app)
{
  EnsureStarted(app);
  if(m_Complete)
  {
    app.RequestExperimentClose();
    return;
  }

  CaptureCompletedFrameIfRequested(app);

  while(!m_Complete && m_FrameInRun >= std::max(1u, m_Plan.runs[m_RunIndex].totalFrames))
  {
    ++m_RunIndex;
    if(m_RunIndex >= m_Plan.runs.size())
    {
      m_Complete = true;
      app.RequestExperimentClose();
      return;
    }
    StartRun(app, m_RunIndex);
  }

  app.SetExperimentCamera(InterpolateCamera(m_Plan.runs[m_RunIndex], m_FrameInRun));
}

void ExperimentController::AfterRender(Application& app)
{
  (void)app;
  if(!m_Complete)
  {
    ++m_FrameInRun;
  }
}

void ExperimentController::OnLastHeadlessFrame(Application& app)
{
  if(!m_Started || m_Complete)
  {
    return;
  }

  CaptureCompletedFrameIfRequested(app);
}

void ExperimentController::EnsureStarted(Application& app)
{
  if(m_Started)
  {
    return;
  }

  if(!ValidateExperimentPlan(m_Plan))
  {
    m_Complete = true;
    app.RequestExperimentClose();
    return;
  }

  std::filesystem::create_directories(m_Plan.outputRoot);
  ExperimentMetadataWriter::WriteManifest(m_Plan);
  m_Started = true;

  if(m_Plan.runs.empty())
  {
    m_Complete = true;
    app.RequestExperimentClose();
    return;
  }

  StartRun(app, 0);
}

void ExperimentController::StartRun(Application& app, size_t runIndex)
{
  m_RunIndex   = runIndex;
  m_FrameInRun = 0;
  app.ApplyExperimentRun(m_Plan.runs[m_RunIndex]);
}

void ExperimentController::CaptureCompletedFrameIfRequested(Application& app)
{
  if(m_FrameInRun == 0 || m_RunIndex >= m_Plan.runs.size())
  {
    return;
  }

  const uint32_t       completedFrame = GetCompletedFrameIndex();
  const ExperimentRun& run            = m_Plan.runs[m_RunIndex];
  for(const ExperimentCaptureFrame& capture : run.captures)
  {
    if(capture.frameIndex == completedFrame)
    {
      CaptureFrame(app, run, capture);
    }
  }
}

void ExperimentController::CaptureFrame(Application& app, const ExperimentRun& run, const ExperimentCaptureFrame& capture)
{
  const std::filesystem::path runFolder = m_Plan.outputRoot / "captures" / run.runName;
  std::filesystem::create_directories(runFolder);

  const std::filesystem::path imagePath = runFolder / capture.outputName;
  app.SaveExperimentImage(imagePath);

  const uint32_t accumulatedFrames = app.GetExperimentAccumulatedFrameCount();
  const std::optional<ExperimentGpuTimings> gpuTimings = app.ReadLastExperimentGpuTimings();
  ExperimentMetadataWriter::WriteCaptureMetadata(m_Plan, run, capture, imagePath, accumulatedFrames, gpuTimings);
  ExperimentMetadataWriter::AppendSummaryRow(m_Plan, run, capture, imagePath, accumulatedFrames, gpuTimings);
}

uint32_t ExperimentController::GetCompletedFrameIndex() const
{
  return m_FrameInRun - 1;
}

}  // namespace nvsamples
