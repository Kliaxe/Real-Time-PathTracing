#include "Rendering/FrameTimingStatistics.h"

#include <algorithm>
#include <iterator>
#include <numeric>

namespace rtpt
{

FrameTimingStatistics::FrameTimingStatistics(const Settings& settings) : m_Settings(settings)
{
}

void FrameTimingStatistics::AddCpuFrameBoundary(std::chrono::steady_clock::time_point now)
{
  // The first boundary only starts the clock; there is no earlier frame to measure.
  if(!m_LastCpuBoundary)
  {
    m_LastCpuBoundary = now;
    return;
  }

  const double milliseconds = std::chrono::duration<double, std::milli>(now - *m_LastCpuBoundary).count();
  const uint64_t frame      = m_CpuFrameCount++;

  m_LastCpuBoundary = now;

  AddSample(m_CpuFrames, frame, milliseconds);
  PruneWindow(m_CpuFrames, frame);
}

void FrameTimingStatistics::AddGpuFrame(std::span<const ScopeSample> scopes)
{
  // A frame slot that has not completed a profiled frame yet reads back nothing, which is not a frame.
  if(scopes.empty())
  {
    return;
  }

  const uint64_t frame = m_GpuFrameCount++;

  // Samples
  // Scopes are matched by name, and a new name is appended so summaries keep first-seen order.

  for(const ScopeSample& scope : scopes)
  {
    auto series = std::find_if(m_Scopes.begin(), m_Scopes.end(), [&](const ScopeSeries& candidate) { return candidate.name == scope.name; });

    if(series == m_Scopes.end())
    {
      m_Scopes.push_back({ .name = std::string(scope.name) });
      series = std::prev(m_Scopes.end());
    }

    AddSample(series->samples, frame, scope.milliseconds);
  }

  // Window
  // Every series is pruned, including scopes this frame did not run, so a pass that stopped running ages out of the window.

  for(ScopeSeries& series : m_Scopes)
  {
    PruneWindow(series.samples, frame);
  }
}

TimingSummary FrameTimingStatistics::SummarizeCpuFrames() const
{
  return Summarize(m_CpuFrames);
}

std::vector<FrameTimingStatistics::ScopeSummary> FrameTimingStatistics::SummarizeScopes() const
{
  std::vector<ScopeSummary> summaries;

  for(const ScopeSeries& series : m_Scopes)
  {
    if(!series.samples.empty())
    {
      summaries.push_back({ .name = series.name, .timing = Summarize(series.samples) });
    }
  }

  return summaries;
}

uint32_t FrameTimingStatistics::GetWarmupFrameCount() const
{
  return m_Settings.warmupFrames;
}

void FrameTimingStatistics::AddSample(std::vector<Sample>& samples, uint64_t frame, double milliseconds) const
{
  if(frame >= m_Settings.warmupFrames)
  {
    samples.push_back({ .frame = frame, .milliseconds = milliseconds });
  }
}

void FrameTimingStatistics::PruneWindow(std::vector<Sample>& samples, uint64_t newestFrame) const
{
  if(m_Settings.windowFrames == 0)
  {
    return;
  }

  // Samples are appended in frame order, so everything outside the window is a prefix.
  const auto firstKept = std::find_if(samples.begin(), samples.end(), [&](const Sample& sample) { return sample.frame + m_Settings.windowFrames > newestFrame; });

  samples.erase(samples.begin(), firstKept);
}

TimingSummary FrameTimingStatistics::Summarize(const std::vector<Sample>& samples)
{
  if(samples.empty())
  {
    return {};
  }

  std::vector<double> sorted;

  sorted.reserve(samples.size());

  for(const Sample& sample : samples)
  {
    sorted.push_back(sample.milliseconds);
  }

  std::sort(sorted.begin(), sorted.end());

  // Order statistics
  // The percentile uses the nearest-rank definition, ceil(0.95 * n) as a 1-based rank, computed in integers so no rounding error moves it across a sample.

  const size_t count      = sorted.size();
  const size_t middle     = count / 2;
  const size_t p95Rank    = (count * 95 + 99) / 100;
  const double sum        = std::accumulate(sorted.begin(), sorted.end(), 0.0);
  const double median     = (count % 2 == 1) ? sorted[middle] : 0.5 * (sorted[middle - 1] + sorted[middle]);

  return TimingSummary {
      .count  = count,
      .mean   = sum / static_cast<double>(count),
      .median = median,
      .p95    = sorted[p95Rank - 1],
      .min    = sorted.front(),
      .max    = sorted.back(),
  };
}

uint32_t GetHeadlessWarmupFrameCount(uint32_t frameCount)
{
  return std::max<uint32_t>(32, frameCount / 10);
}

}  // namespace rtpt
