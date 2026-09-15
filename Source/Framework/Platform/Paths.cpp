#include "Paths.h"

#include <cerrno>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace rtpt
{

std::filesystem::path ExecutablePath()
{
#if defined(_WIN32)
  // Windows
  // GetModuleFileNameW truncates silently when the buffer is too small, so a result that fills the buffer is treated as possibly truncated and retried with double the size.

  std::vector<wchar_t> path(512);

  for(;;)
  {
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));

    if(length == 0)
    {
      throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetModuleFileNameW");
    }

    if(length < path.size() - 1)
    {
      return std::filesystem::path(std::wstring_view(path.data(), length));
    }

    path.resize(path.size() * 2);
  }
#elif defined(__linux__)
  // Linux
  // readlink does not null-terminate and truncates to the buffer size, so only a result shorter than the buffer is known to be complete.

  std::vector<char> path(512);

  for(;;)
  {
    const ssize_t length = readlink("/proc/self/exe", path.data(), path.size());

    if(length < 0)
    {
      throw std::system_error(errno, std::generic_category(), "readlink(/proc/self/exe)");
    }

    if(static_cast<size_t>(length) < path.size())
    {
      return std::filesystem::path(std::string_view(path.data(), static_cast<size_t>(length)));
    }

    path.resize(path.size() * 2);
  }
#else
#error ExecutablePath is not implemented for this platform
#endif
}

std::filesystem::path ExecutableDirectory()
{
  return ExecutablePath().parent_path();
}

std::vector<std::filesystem::path> ContentDirectories()
{
  // Search order
  // Content next to the executable comes first, then the working directory.
  // Builds that define RTPT_SOURCE_ROOT also fall back to the source tree, so binaries run straight from the build output find assets without copying them.

  std::vector<std::filesystem::path> roots {
      ExecutableDirectory() / "Content",
      std::filesystem::current_path() / "Content",
  };

#if defined(RTPT_SOURCE_ROOT)
  roots.emplace_back(std::filesystem::path(RTPT_SOURCE_ROOT) / "Content");
#endif

  return roots;
}

}  // namespace rtpt
