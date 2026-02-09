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

#include <glm/glm.hpp>

#include "IoGltf.h"  // GLTF shader-side structs (shared between C++ and Slang)

#include "nvutils/bounding_box.hpp"
#include "nvvk/resources.hpp"
#include "nvvk/staging.hpp"
#include "nvutils/primitives.hpp"
#include "tinygltf/tiny_gltf.h"

namespace nvsamples
{

// Holds CPU-side scene arrays + the corresponding GPU buffers.
struct GltfSceneResource
{
  std::vector<shaderio::GltfMesh>              meshes;     // Mesh descriptions
  std::vector<shaderio::GltfInstance>          instances;  // Instances
  std::vector<shaderio::GltfMetallicRoughness> materials;  // Materials
  shaderio::GltfSceneInfo                      sceneInfo;  // Scene parameters

  // GPU buffers
  std::vector<nvvk::Buffer> bGltfDatas;  // One per imported GLTF buffer blob
  nvvk::Buffer              bMeshes;     // Packed mesh array
  nvvk::Buffer              bInstances;  // Packed instance array
  nvvk::Buffer              bMaterials;  // Packed material array
  nvvk::Buffer              bSceneInfo;  // Scene info struct

  // meshToBufferIndex[meshIndex] = bufferIndex into bGltfDatas
  std::vector<uint32_t> meshToBufferIndex;
  // meshMaterialIndices[meshIndex] = material index used by that mesh primitive
  std::vector<uint32_t> meshMaterialIndices;
};

// Loads a GLTF/GLB file from disk.
tinygltf::Model LoadGltfResources(const std::filesystem::path& filename);

// Imports GLTF geometry into GPU-friendly buffers.
void ImportGltfData(GltfSceneResource& sceneResource, const tinygltf::Model& model, nvvk::StagingUploader& stagingUploader,
                    bool importInstance = false, uint32_t materialOffset = 0, uint32_t fallbackMaterialIndex = 0);

// Creates GPU buffers for meshes/instances/materials/sceneInfo.
void CreateGltfSceneInfoBuffer(GltfSceneResource& sceneResource, nvvk::StagingUploader& stagingUploader);

// Converts a primitive mesh into the same GLTF resource format.
void PrimitiveMeshToResource(GltfSceneResource& sceneResource, nvvk::StagingUploader& stagingUploader, const nvutils::PrimitiveMesh& primMesh);

}  // namespace nvsamples

