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

#include "Utils.hpp"

#include <cassert>

#include <nvutils/file_operations.hpp>
#include <nvutils/timers.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/default_structs.hpp>
#include <nvvk/staging.hpp>

#include <stb/stb_image.h>

namespace nvsamples
{

nvvk::Image LoadAndCreateImage(VkCommandBuffer cmd, nvvk::StagingUploader& staging, VkDevice device, const std::filesystem::path& filename, bool sRgb)
{
  // Load the image from disk.
  int         w = 0, h = 0, comp = 0, req_comp = 4;
  std::string filenameUtf8 = nvutils::utf8FromPath(filename);
  stbi_uc*    data         = stbi_load(filenameUtf8.c_str(), &w, &h, &comp, req_comp);
  assert((data != nullptr) && "Could not load texture image!");

  // Define how to create the image.
  VkImageCreateInfo imageInfo = DEFAULT_VkImageCreateInfo;
  imageInfo.format            = sRgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
  imageInfo.usage             = VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.extent            = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};

  nvvk::ResourceAllocator* allocator = staging.getResourceAllocator();

  // Create image + stage upload.
  const std::span dataSpan(data, static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(req_comp));
  nvvk::Image     texture;
  NVVK_CHECK(allocator->createImage(texture, imageInfo, DEFAULT_VkImageViewCreateInfo));
  NVVK_CHECK(staging.appendImage(texture, dataSpan, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));

  // stb_image uses heap memory for the load result.
  stbi_image_free(data);

  return texture;
}

}  // namespace nvsamples
