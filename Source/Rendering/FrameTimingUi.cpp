#include "Rendering/FrameTimingUi.h"

#include <imgui.h>

#include "Framework/Presentation/UiControls.h"

namespace rtpt
{
namespace
{

// Writes one table row: a label, then the mean and 95th percentile in milliseconds.
void DrawTimingRow(const char* label, const TimingSummary& timing, const char* explanation)
{
  ImGui::TableNextRow();

  ImGui::TableNextColumn();
  ImGui::TextUnformatted(label);
  DrawTooltip(explanation);

  ImGui::TableNextColumn();
  ImGui::Text("%.2f", timing.mean);

  ImGui::TableNextColumn();
  ImGui::Text("%.2f", timing.p95);
}

}  // namespace

void DrawFrameTimingSection(const FrameTimingStatistics& statistics, bool profilerAvailable)
{
  const bool open = ImGui::CollapsingHeader("Profiler");

  DrawTooltip("Where the frame time goes. CPU frame time comes from wall-clock measurement; the rows below it are GPU timestamps around each pass.");

  if(!open) return;

  if(!profilerAvailable)
  {
    ImGui::TextDisabled("GPU timestamps unavailable on this device");
  }

  // Table
  // CPU frame time leads because it bounds everything below it. GPU scopes follow in first-seen order, which is recording order; their "Group/Pass" names show which renderer each pass belongs to.

  constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;

  if(!ImGui::BeginTable("FrameTiming", 3, tableFlags)) return;

  ImGui::TableSetupColumn("Scope (ms)", ImGuiTableColumnFlags_WidthStretch, 3.0F);
  ImGui::TableSetupColumn("Mean", ImGuiTableColumnFlags_WidthStretch, 1.0F);
  ImGui::TableSetupColumn("P95", ImGuiTableColumnFlags_WidthStretch, 1.0F);
  ImGui::TableHeadersRow();

  DrawTimingRow("CPU frame", statistics.SummarizeCpuFrames(), "Wall-clock time between successive frames, which bounds every GPU scope below it. Mean is the average over the sampling window; P95 is the slowest one frame in twenty, where stutter shows up that an average hides.");

  for(const FrameTimingStatistics::ScopeSummary& scope : statistics.SummarizeScopes())
  {
    DrawTimingRow(scope.name.c_str(), scope.timing, "GPU time for one pass, measured with timestamps around it and named Group/Pass. The rows are in recording order, so they read as the frame is built; they need not add up to the CPU frame time, since passes can overlap and the CPU also waits on presentation.");
  }

  ImGui::EndTable();
}

}  // namespace rtpt
