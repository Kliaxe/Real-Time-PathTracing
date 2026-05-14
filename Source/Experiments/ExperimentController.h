#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>

#include "Experiments/ExperimentTypes.h"

namespace nvsamples
{

class Application;

class ExperimentController
{
public:
  explicit ExperimentController(ExperimentPlan plan);

  const ExperimentPlan& GetPlan() const;
  uint32_t              GetRequiredHeadlessFrameCount() const;

  void BeforeRender(Application& app);
  void AfterRender(Application& app);
  void OnLastHeadlessFrame(Application& app);

private:
  void EnsureStarted(Application& app);
  void StartRun(Application& app, size_t runIndex);
  void CaptureCompletedFrameIfRequested(Application& app);
  void CaptureFrame(Application& app, const ExperimentRun& run, const ExperimentCaptureFrame& capture);
  uint32_t GetCompletedFrameIndex() const;

  ExperimentPlan m_Plan;
  size_t         m_RunIndex       = 0;
  uint32_t       m_FrameInRun     = 0;
  bool           m_Started        = false;
  bool           m_Complete       = false;
};

}  // namespace nvsamples
