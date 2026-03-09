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

std::optional<ImageDataFloat4> LoadImageFloat4(const std::filesystem::path& filename)
{
  int         width = 0;
  int         height = 0;
  int         components = 0;
  const int   requestedComponents = 4;
  std::string filenameUtf8 = nvutils::utf8FromPath(filename);

  ImageDataFloat4 imageData{};

  if(stbi_is_hdr(filenameUtf8.c_str()) != 0)
  {
    float* data = stbi_loadf(filenameUtf8.c_str(), &width, &height, &components, requestedComponents);
    if(data == nullptr || width <= 0 || height <= 0)
    {
      return std::nullopt;
    }

    imageData.width  = static_cast<uint32_t>(width);
    imageData.height = static_cast<uint32_t>(height);
    imageData.pixels.assign(data, data + static_cast<size_t>(width) * static_cast<size_t>(height) * requestedComponents);
    stbi_image_free(data);
    return imageData;
  }

  stbi_uc* data = stbi_load(filenameUtf8.c_str(), &width, &height, &components, requestedComponents);
  if(data == nullptr || width <= 0 || height <= 0)
  {
    return std::nullopt;
  }

  const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
  imageData.width         = static_cast<uint32_t>(width);
  imageData.height        = static_cast<uint32_t>(height);
  imageData.pixels.resize(pixelCount * requestedComponents);
  for(size_t i = 0; i < pixelCount * requestedComponents; ++i)
  {
    imageData.pixels[i] = static_cast<float>(data[i]) / 255.0f;
  }

  stbi_image_free(data);
  return imageData;
}

nvvk::Image LoadAndCreateImage(VkCommandBuffer cmd, nvvk::StagingUploader& staging, VkDevice device, const std::filesystem::path& filename, bool sRgb)
{
  std::string filenameUtf8 = nvutils::utf8FromPath(filename);
  const bool  isHdr        = stbi_is_hdr(filenameUtf8.c_str()) != 0;

  // Define how to create the image.
  VkImageCreateInfo imageInfo = DEFAULT_VkImageCreateInfo;
  imageInfo.format            = isHdr ? VK_FORMAT_R32G32B32A32_SFLOAT : (sRgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
  imageInfo.usage             = VK_IMAGE_USAGE_SAMPLED_BIT;

  nvvk::ResourceAllocator* allocator = staging.getResourceAllocator();

  nvvk::Image     texture;
  if(isHdr)
  {
    const std::optional<ImageDataFloat4> imageData = LoadImageFloat4(filename);
    assert((imageData.has_value()) && "Could not load HDR texture image!");

    imageInfo.extent = {imageData->width, imageData->height, 1};
    NVVK_CHECK(allocator->createImage(texture, imageInfo, DEFAULT_VkImageViewCreateInfo));
    NVVK_CHECK(staging.appendImage(texture, std::span<const float>(imageData->pixels), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    return texture;
  }

  int       width = 0;
  int       height = 0;
  int       components = 0;
  const int requestedComponents = 4;
  stbi_uc*  data = stbi_load(filenameUtf8.c_str(), &width, &height, &components, requestedComponents);
  assert((data != nullptr) && "Could not load texture image!");

  imageInfo.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
  NVVK_CHECK(allocator->createImage(texture, imageInfo, DEFAULT_VkImageViewCreateInfo));
  NVVK_CHECK(staging.appendImage(texture,
                                 std::span<const stbi_uc>(data, static_cast<size_t>(width) * static_cast<size_t>(height) * requestedComponents),
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
  stbi_image_free(data);

  return texture;
}

}  // namespace nvsamples
