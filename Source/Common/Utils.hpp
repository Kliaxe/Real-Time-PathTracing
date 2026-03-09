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
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <nvvk/resources.hpp>
#include <nvvk/staging.hpp>

namespace nvsamples
{

struct ImageDataFloat4
{
  uint32_t           width  = 0;
  uint32_t           height = 0;
  std::vector<float> pixels;
};

// Helper for building VkShaderModuleCreateInfo from SPIR-V.
inline VkShaderModuleCreateInfo GetShaderModuleCreateInfo(const std::span<const uint32_t>& spirv)
{
  return VkShaderModuleCreateInfo{
      .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = spirv.size_bytes(),
      .pCode    = spirv.data(),
  };
}

// Loads an image from disk and uploads it into a sampled VkImage via the staging uploader.
nvvk::Image LoadAndCreateImage(VkCommandBuffer cmd, nvvk::StagingUploader& staging, VkDevice device, const std::filesystem::path& filename, bool sRgb = true);
std::optional<ImageDataFloat4> LoadImageFloat4(const std::filesystem::path& filename);

}  // namespace nvsamples
