/*
 * Copyright (c) 2024-2026, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2024-2026, NVIDIA CORPORATION.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <filesystem>
#include <vector>

#include <nvutils/file_operations.hpp>

namespace nvsamples
{

// WARNING:
// - These helpers rely on CMake-provided macros from nvpro_core2's add_project_definitions().
// - Include this header only from .cpp files that are built as part of the app target.

inline std::vector<std::filesystem::path> GetContentDirs()
{
  const std::filesystem::path exePath = nvutils::getExecutablePath().parent_path();

  return {
      std::filesystem::absolute(exePath / "Content"),
      std::filesystem::absolute(exePath / TARGET_EXE_TO_ROOT_DIRECTORY / "Content"),
  };
}

inline std::vector<std::filesystem::path> GetShaderDirs()
{
  const std::filesystem::path exePath = nvutils::getExecutablePath().parent_path();

  return {
      std::filesystem::absolute(exePath / "Shaders"),          // App shaders copied next to the exe
      std::filesystem::absolute(exePath / "ShaderIncludes"),   // Shared includes copied next to the exe
      std::filesystem::absolute(exePath / TARGET_EXE_TO_SOURCE_DIRECTORY / "Shaders"),
      std::filesystem::absolute(exePath / TARGET_EXE_TO_SOURCE_DIRECTORY / "ShaderIncludes"),
      std::filesystem::absolute(exePath / TARGET_EXE_TO_NVSHADERS_DIRECTORY),
      std::filesystem::absolute(exePath / TARGET_EXE_TO_ROOT_DIRECTORY),
      std::filesystem::absolute(exePath / TARGET_EXE_TO_ROOT_DIRECTORY / "Source" / "ShaderIncludes"),
      std::filesystem::absolute(NVSHADERS_DIR),
      std::filesystem::absolute(exePath / TARGET_NAME "_files" / "shaders"),
      std::filesystem::absolute(exePath),
  };
}

}  // namespace nvsamples

