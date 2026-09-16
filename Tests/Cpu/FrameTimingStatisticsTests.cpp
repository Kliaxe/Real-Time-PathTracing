#include "Rendering/FrameTimingStatistics.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using rtpt::FrameTimingStatistics;
using rtpt::TimingSummary;

// Reports a failed expectation and returns the condition, so every check in a test runs and a test passes only if all of them hold.
bool Expect(bool condition, std::string_view message)
{
  if(!condition)
  {
    std::cerr << message << '\n';
  }

  return condition;
}

// Summaries are computed in floating point, so they are compared with a tolerance far below any timing resolution.
bool Near(double actual, double expected)
{
  return std::abs(actual - expected) < 1e-9;
}

// Adds one GPU frame holding a single scope.
void AddSingleScopeFrame(FrameTimingStatistics& statistics, std::string_view name, double milliseconds)
{
  const FrameTimingStatistics::ScopeSample sample { .name = name, .milliseconds = milliseconds };

  statistics.AddGpuFrame(std::span(&sample, 1));
}

// Returns the summary of the named scope, or an empty summary when the scope was not reported.
TimingSummary FindScope(const FrameTimingStatistics& statistics, std::string_view name)
{
  for(const FrameTimingStatistics::ScopeSummary& scope : statistics.SummarizeScopes())
  {
    if(scope.name == name)
    {
      return scope.timing;
    }
  }

  return {};
}

bool TestSummaryMath()
{
  bool passed = true;

  // Even count
  // 1..20: the median averages the two middle samples, and the nearest-rank p95 is rank ceil(0.95 * 20) = 19.

  FrameTimingStatistics even(FrameTimingStatistics::Settings {});

  for(int value = 20; value >= 1; --value)
  {
    AddSingleScopeFrame(even, "Pass", value);
  }

  const TimingSummary evenSummary = FindScope(even, "Pass");

  passed &= Expect(evenSummary.count == 20, "every sample must be counted");
  passed &= Expect(Near(evenSummary.mean, 10.5), "mean of 1..20 must be 10.5");
  passed &= Expect(Near(evenSummary.median, 10.5), "median of an even count must average the middle samples");
  passed &= Expect(Near(evenSummary.p95, 19.0), "p95 of 1..20 must be the 19th sample");
  passed &= Expect(Near(evenSummary.min, 1.0) && Near(evenSummary.max, 20.0), "min and max must be the extremes regardless of arrival order");

  // Odd count
  // Three samples: the median is the middle one, and rank ceil(2.85) = 3 makes p95 the largest.

  FrameTimingStatistics odd(FrameTimingStatistics::Settings {});

  AddSingleScopeFrame(odd, "Pass", 5.0);
  AddSingleScopeFrame(odd, "Pass", 1.0);
  AddSingleScopeFrame(odd, "Pass", 3.0);

  const TimingSummary oddSummary = FindScope(odd, "Pass");

  passed &= Expect(Near(oddSummary.median, 3.0), "median of an odd count must be the middle sample");
  passed &= Expect(Near(oddSummary.p95, 5.0), "p95 of three samples must be the largest");
  passed &= Expect(Near(oddSummary.mean, 3.0), "mean of 1, 3, 5 must be 3");

  // Empty
  // No samples must summarize to zeros rather than read out of range.

  const FrameTimingStatistics empty(FrameTimingStatistics::Settings {});
  const TimingSummary emptySummary = empty.SummarizeCpuFrames();

  passed &= Expect(emptySummary.count == 0 && emptySummary.mean == 0.0 && emptySummary.p95 == 0.0, "an empty series must summarize to zeros");
  passed &= Expect(empty.SummarizeScopes().empty(), "no scopes must be reported before any frame");

  return passed;
}

bool TestWarmupAndScopeOrder()
{
  bool passed = true;

  // Warm-up
  // Three warm-up frames of ten: every-frame scopes keep seven samples, and a scope that only ran during warm-up is not reported.
  // The frame index counts GPU frames, so an empty read-back must not advance it.

  FrameTimingStatistics statistics({ .warmupFrames = 3 });

  statistics.AddGpuFrame({});

  for(int frame = 0; frame < 10; ++frame)
  {
    std::vector<FrameTimingStatistics::ScopeSample> samples { { .name = "Frame", .milliseconds = 10.0 + frame } };

    // A pass that only runs on even frames, first seen after "Frame".
    if(frame % 2 == 0)
    {
      samples.push_back({ .name = "Frame/Even", .milliseconds = 1.0 });
    }

    // A pass that only runs before warm-up ends.
    if(frame < 3)
    {
      samples.push_back({ .name = "Frame/Startup", .milliseconds = 99.0 });
    }

    statistics.AddGpuFrame(samples);
  }

  const std::vector<FrameTimingStatistics::ScopeSummary> scopes = statistics.SummarizeScopes();

  passed &= Expect(statistics.GetWarmupFrameCount() == 3, "the warm-up count must be reported as configured");
  passed &= Expect(scopes.size() == 2 && scopes[0].name == "Frame" && scopes[1].name == "Frame/Even", "scopes must be reported in first-seen order without warm-up-only scopes");
  passed &= Expect(FindScope(statistics, "Frame").count == 7, "an every-frame scope must keep frames minus warm-up");
  passed &= Expect(Near(FindScope(statistics, "Frame").min, 13.0), "the first kept frame must be the one after warm-up");
  passed &= Expect(FindScope(statistics, "Frame/Even").count == 3, "an even-frame scope must keep frames 4, 6, and 8");

  // Headless warm-up rule
  // At least 32 frames, and a tenth of long runs.

  passed &= Expect(rtpt::GetHeadlessWarmupFrameCount(1) == 32, "short runs must warm up for 32 frames");
  passed &= Expect(rtpt::GetHeadlessWarmupFrameCount(200) == 32, "a 200-frame run must warm up for 32 frames");
  passed &= Expect(rtpt::GetHeadlessWarmupFrameCount(1000) == 100, "a 1000-frame run must warm up for a tenth of its frames");

  return passed;
}

bool TestRollingWindow()
{
  bool passed = true;

  // Window
  // A four-frame window over ten frames keeps frames 6..9, and a scope that stopped running at frame 4 ages out entirely.

  FrameTimingStatistics statistics({ .windowFrames = 4 });

  for(int frame = 0; frame < 10; ++frame)
  {
    std::vector<FrameTimingStatistics::ScopeSample> samples { { .name = "Frame", .milliseconds = static_cast<double>(frame) } };

    if(frame < 5)
    {
      samples.push_back({ .name = "Stopped", .milliseconds = 1.0 });
    }

    statistics.AddGpuFrame(samples);
  }

  const TimingSummary frame = FindScope(statistics, "Frame");

  passed &= Expect(frame.count == 4, "the window must keep exactly its frame count");
  passed &= Expect(Near(frame.min, 6.0) && Near(frame.max, 9.0), "the window must keep the most recent frames");
  passed &= Expect(statistics.SummarizeScopes().size() == 1, "a scope that stopped running must leave the window");

  return passed;
}

bool TestCpuFrameTime()
{
  bool passed = true;

  // Boundaries
  // Boundaries at 0, 10, 30, and 60 ms measure three frames of 10, 20, and 30 ms; the first boundary measures nothing.

  using Clock = std::chrono::steady_clock;

  const Clock::time_point start {};

  FrameTimingStatistics all(FrameTimingStatistics::Settings {});
  FrameTimingStatistics warm({ .warmupFrames = 1 });
  FrameTimingStatistics windowed({ .windowFrames = 2 });

  for(const int milliseconds : { 0, 10, 30, 60 })
  {
    const Clock::time_point now = start + std::chrono::milliseconds(milliseconds);

    all.AddCpuFrameBoundary(now);
    warm.AddCpuFrameBoundary(now);
    windowed.AddCpuFrameBoundary(now);
  }

  passed &= Expect(all.SummarizeCpuFrames().count == 3 && Near(all.SummarizeCpuFrames().mean, 20.0), "three boundaries after the first must measure three frames averaging 20 ms");
  passed &= Expect(warm.SummarizeCpuFrames().count == 2 && Near(warm.SummarizeCpuFrames().min, 20.0), "CPU warm-up must drop the first measured frame");
  passed &= Expect(windowed.SummarizeCpuFrames().count == 2 && Near(windowed.SummarizeCpuFrames().max, 30.0) && Near(windowed.SummarizeCpuFrames().min, 20.0), "the CPU window must keep the most recent frames");

  return passed;
}

}  // namespace

int main()
{
  // Every test runs even after an earlier one fails, so one run reports every broken rule.

  bool passed = true;

  passed &= TestSummaryMath();
  passed &= TestWarmupAndScopeOrder();
  passed &= TestRollingWindow();
  passed &= TestCpuFrameTime();

  return passed ? 0 : 1;
}
