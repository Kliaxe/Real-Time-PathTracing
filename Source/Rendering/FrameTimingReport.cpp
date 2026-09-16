#include "Rendering/FrameTimingReport.h"

#include <fstream>
#include <stdexcept>
#include <string_view>

#include <fmt/format.h>

namespace rtpt
{
namespace
{

// Escapes the characters JSON forbids inside a string. Scene labels and device names come from content and the driver, so they are not trusted to be clean.
std::string EscapeJson(std::string_view value)
{
  std::string escaped;

  escaped.reserve(value.size());

  for(const char character : value)
  {
    const unsigned char code = static_cast<unsigned char>(character);

    if(character == '"' || character == '\\')
    {
      escaped += '\\';
      escaped += character;
    }
    else if(code < 0x20)
    {
      escaped += fmt::format("\\u{:04x}", code);
    }
    else
    {
      escaped += character;
    }
  }

  return escaped;
}

// Formats a summary's fields as the body of a JSON object, without braces, so scopes can prepend their name.
std::string FormatSummaryFields(const TimingSummary& timing)
{
  return fmt::format("\"count\": {}, \"mean\": {:.6f}, \"median\": {:.6f}, \"p95\": {:.6f}, \"min\": {:.6f}, \"max\": {:.6f}", timing.count, timing.mean, timing.median, timing.p95, timing.min, timing.max);
}

// Formats one fixed-width table row.
std::string FormatTableRow(std::string_view label, const TimingSummary& timing)
{
  return fmt::format("{:<34} {:>6} {:>9.3f} {:>9.3f} {:>9.3f} {:>9.3f} {:>9.3f}\n", label, timing.count, timing.mean, timing.median, timing.p95, timing.min, timing.max);
}

}  // namespace

void WriteFrameTimingReport(const std::filesystem::path& path, const FrameTimingReportMetadata& metadata, const FrameTimingStatistics& statistics)
{
  // Document
  // Built as one string so a failed write can never leave a half-formed report that parses as something else.

  std::string json;

  json += "{\n";
  json += "  \"schema_version\": 1,\n";
  json += fmt::format("  \"renderer\": \"{}\",\n", EscapeJson(metadata.renderer));
  json += fmt::format("  \"resolve_mode\": \"{}\",\n", EscapeJson(metadata.resolveMode));
  json += fmt::format("  \"scene\": {{\"index\": {}, \"label\": \"{}\"}},\n", metadata.sceneIndex, EscapeJson(metadata.sceneLabel));
  json += fmt::format("  \"viewport\": {{\"width\": {}, \"height\": {}}},\n", metadata.viewportWidth, metadata.viewportHeight);
  json += fmt::format("  \"frames\": {},\n", metadata.frameCount);
  json += fmt::format("  \"warmup_frames\": {},\n", statistics.GetWarmupFrameCount());
  json += fmt::format("  \"gpu\": {{\"device_name\": \"{}\", \"driver_version\": {}}},\n", EscapeJson(metadata.deviceName), metadata.driverVersion);
  json += fmt::format("  \"cpu_frame_ms\": {{{}}},\n", FormatSummaryFields(statistics.SummarizeCpuFrames()));
  json += "  \"scopes\": [";

  const std::vector<FrameTimingStatistics::ScopeSummary> scopes = statistics.SummarizeScopes();

  for(size_t index = 0; index < scopes.size(); ++index)
  {
    json += fmt::format("{}\n    {{\"name\": \"{}\", {}}}", index == 0 ? "" : ",", EscapeJson(scopes[index].name), FormatSummaryFields(scopes[index].timing));
  }

  json += scopes.empty() ? "]\n}\n" : "\n  ]\n}\n";

  // Write
  // The stream does not throw, so its state is checked after closing, when buffered data has actually reached the file.

  std::ofstream stream(path, std::ios::binary | std::ios::trunc);

  stream << json;
  stream.close();

  if(!stream)
  {
    throw std::runtime_error("could not write profile output: " + path.string());
  }
}

std::string FormatFrameTimingTable(const FrameTimingReportMetadata& metadata, const FrameTimingStatistics& statistics)
{
  std::string table;

  table += fmt::format("GPU profile: {} ({}), scene {} \"{}\", {}x{}, {} frames ({} warm-up), {}\n", metadata.renderer, metadata.resolveMode, metadata.sceneIndex, metadata.sceneLabel, metadata.viewportWidth, metadata.viewportHeight, metadata.frameCount, statistics.GetWarmupFrameCount(), metadata.deviceName);
  table += fmt::format("{:<34} {:>6} {:>9} {:>9} {:>9} {:>9} {:>9}\n", "Scope (ms)", "count", "mean", "median", "p95", "min", "max");
  table += FormatTableRow("CPU frame", statistics.SummarizeCpuFrames());

  for(const FrameTimingStatistics::ScopeSummary& scope : statistics.SummarizeScopes())
  {
    table += FormatTableRow(scope.name, scope.timing);
  }

  return table;
}

}  // namespace rtpt
