#include "Log.h"

#include <chrono>
#include <cstdio>
#include <mutex>

namespace rtpt
{

void Log(LogLevel level, std::string_view message)
{
  // Serializes writers so lines from different threads never interleave.
  static std::mutex mutex;

  // Severity label
  // Info is the default; the padded %-7s field below is sized for the longest label, "warning".

  const char* label = "info";

  if(level == LogLevel::Warning)
  {
    label = "warning";
  }
  else if(level == LogLevel::Error)
  {
    label = "error";
  }

  // Write
  // The timestamp is seconds on the steady clock, which is monotonic but has an unspecified epoch, so it is only useful for spacing between lines.
  // The message is printed with an explicit length because a string_view is not guaranteed to be null-terminated.

  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

  const std::scoped_lock lock(mutex);

  std::fprintf(stderr, "[%10.3f] %-7s %.*s\n", elapsed, label, static_cast<int>(message.size()), message.data());
}

}  // namespace rtpt
