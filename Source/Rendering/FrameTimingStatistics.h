#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rtpt
{

// TimingSummary
// Distribution of one timing series in milliseconds. Every field is zero when count is zero.

struct TimingSummary
{
  // Samples the summary was computed from.
  size_t count = 0;

  // Arithmetic mean.
  double mean = 0.0;

  // Middle sample, or the average of the two middle samples for an even count.
  double median = 0.0;

  // Nearest-rank 95th percentile: the smallest sample that at least 95% of the samples do not exceed.
  double p95 = 0.0;

  // Smallest and largest sample.
  double min = 0.0;
  double max = 0.0;
};

// FrameTimingStatistics
// Collects per-frame timings - GPU scope durations and CPU frame time - and summarizes them, so a claim about where frame time goes rests on a distribution rather than one frame.
// It deliberately knows nothing about Vulkan: Application converts GpuProfiler results into ScopeSamples, which keeps the math testable on the CPU.
// Headless runs keep every sample after a warm-up, because the first frames pay for pipeline warm-up, allocation, and history filling. Interactive runs keep a rolling window of recent frames instead, so the numbers follow what is on screen.

class FrameTimingStatistics
{
public:

  // Settings
  // How samples are retained. The two modes are exclusive in practice: headless runs set warmupFrames, interactive runs set windowFrames.

  struct Settings
  {
    // Leading frames whose samples are discarded.
    uint32_t warmupFrames = 0;

    // Most recent frames whose samples are kept. Zero keeps every sample.
    uint32_t windowFrames = 0;
  };

  // ScopeSample
  // One named GPU scope measured in one frame.

  struct ScopeSample
  {
    // Scope name; copied when the scope is first seen.
    std::string_view name;

    // Measured duration.
    double milliseconds = 0.0;
  };

  // ScopeSummary
  // Summary of one scope, reported in the order scopes were first seen.

  struct ScopeSummary
  {
    // Scope name.
    std::string   name;

    // Distribution of the scope's retained samples.
    TimingSummary timing;
  };

  explicit FrameTimingStatistics(const Settings& settings);

  // Marks the start of a frame. The time since the previous boundary becomes the previous frame's CPU frame time, so the first boundary records nothing.
  void AddCpuFrameBoundary(std::chrono::steady_clock::time_point now);

  // Adds every scope measured in one GPU frame. Frames must arrive in the order they were rendered. An empty frame carries no measurement and does not count as a frame.
  void AddGpuFrame(std::span<const ScopeSample> scopes);

  [[nodiscard]] TimingSummary SummarizeCpuFrames() const;

  // Scopes with no retained sample, such as passes that stopped running before the window, are left out.
  [[nodiscard]] std::vector<ScopeSummary> SummarizeScopes() const;

  [[nodiscard]] uint32_t GetWarmupFrameCount() const;

private:

  // Sample
  // One measurement tagged with the frame it came from, so warm-up and window rules can be applied per frame rather than per sample.

  struct Sample
  {
    // Index of the frame, counted from the first frame this series type received.
    uint64_t frame = 0;

    // Measured duration.
    double   milliseconds = 0.0;
  };

  // ScopeSeries
  // Every retained sample of one named scope.

  struct ScopeSeries
  {
    // Scope name.
    std::string         name;

    // Retained samples, oldest first.
    std::vector<Sample> samples;
  };

  // Appends a sample unless it falls inside the warm-up.
  void AddSample(std::vector<Sample>& samples, uint64_t frame, double milliseconds) const;

  // Drops samples that left the rolling window now that newestFrame is the latest frame.
  void PruneWindow(std::vector<Sample>& samples, uint64_t newestFrame) const;

  [[nodiscard]] static TimingSummary Summarize(const std::vector<Sample>& samples);

  // Retention rules, fixed for the life of the statistics.
  Settings                 m_Settings;

  // Scopes in first-seen order, which is also the order passes are recorded in a frame.
  std::vector<ScopeSeries> m_Scopes;

  // CPU frame times, indexed by the frame whose duration they measure.
  std::vector<Sample>      m_CpuFrames;

  // GPU frames received so far.
  uint64_t                 m_GpuFrameCount = 0;

  // CPU frame times recorded so far.
  uint64_t                 m_CpuFrameCount = 0;

  // Time of the most recent frame boundary. Empty before the first one.
  std::optional<std::chrono::steady_clock::time_point> m_LastCpuBoundary;
};

// Warm-up frames a headless run of frameCount frames discards: at least 32, and a tenth of longer runs.
[[nodiscard]] uint32_t GetHeadlessWarmupFrameCount(uint32_t frameCount);

}  // namespace rtpt
