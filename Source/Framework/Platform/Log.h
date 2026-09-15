#pragma once

#include <string_view>

namespace rtpt
{

// LogLevel
// Severity label printed in front of each log line.

enum class LogLevel
{
  Info,
  Warning,
  Error,
};

// Writes one timestamped line to stderr. Safe to call from multiple threads.
void Log(LogLevel level, std::string_view message);

}  // namespace rtpt
