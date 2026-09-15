#pragma once

#include <filesystem>
#include <vector>

namespace rtpt
{

// Absolute path of the running executable, queried from the OS rather than argv[0] so it is correct regardless of how the process was launched.
[[nodiscard]] std::filesystem::path ExecutablePath();

[[nodiscard]] std::filesystem::path ExecutableDirectory();

// Every candidate Content directory, in search order. Entries are not checked for existence.
[[nodiscard]] std::vector<std::filesystem::path> ContentDirectories();

}  // namespace rtpt
