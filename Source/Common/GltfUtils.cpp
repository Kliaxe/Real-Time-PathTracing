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

#include "GltfUtils.hpp"

#include <algorithm>
#include <functional>
#include <span>

#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vulkan/vulkan_core.h>

#include "nvutils/logger.hpp"
#include "nvutils/timers.hpp"
#include "nvvk/check_error.hpp"
#include "nvvk/debug_util.hpp"

namespace nvsamples
{

void PrimitiveMeshToResource(GltfSceneResource& sceneResource, nvvk::StagingUploader& stagingUploader, const nvutils::PrimitiveMesh& primMesh)
{
  nvvk::ResourceAllocator* allocator = stagingUploader.getResourceAllocator();

  // Calculate buffer sizes.
  const size_t verticesSize  = std::span(primMesh.vertices).size_bytes();
  const size_t trianglesSize = std::span(primMesh.triangles).size_bytes();

  // Create buffer for the geometry data (vertices + triangles).
  nvvk::Buffer gltfData;
  allocator->createBuffer(gltfData, verticesSize + trianglesSize,
                          VK_BUFFER_USAGE_2_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_2_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT
                              | VK_BUFFER_USAGE_2_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
  const uint32_t bufferIndex = static_cast<uint32_t>(sceneResource.bGltfDatas.size());
  sceneResource.bGltfDatas.push_back(gltfData);

  // Upload vertices first (offset 0).
  stagingUploader.appendBuffer(gltfData, 0, std::span(primMesh.vertices));

  // Upload triangles after vertices.
  stagingUploader.appendBuffer(gltfData, verticesSize, std::span(primMesh.triangles));

  // Fill out TriangleMesh buffer views.
  shaderio::GltfMesh mesh;
  mesh.triMesh.positions = {
      .offset     = 0,
      .count      = static_cast<uint32_t>(primMesh.vertices.size()),
      .byteStride = sizeof(nvutils::PrimitiveVertex),
  };
  mesh.triMesh.normals = {
      .offset     = offsetof(nvutils::PrimitiveVertex, nrm),
      .count      = static_cast<uint32_t>(primMesh.vertices.size()),
      .byteStride = sizeof(nvutils::PrimitiveVertex),
  };
  mesh.triMesh.texCoords = {
      .offset     = offsetof(nvutils::PrimitiveVertex, tex),
      .count      = static_cast<uint32_t>(primMesh.vertices.size()),
      .byteStride = sizeof(nvutils::PrimitiveVertex),
  };
  mesh.triMesh.indices = {
      .offset     = static_cast<uint32_t>(verticesSize),
      .count      = static_cast<uint32_t>(primMesh.triangles.size() * 3),
      .byteStride = sizeof(uint32_t),
  };

  mesh.gltfBuffer = reinterpret_cast<uint8_t*>(gltfData.address);
  mesh.indexType  = VK_INDEX_TYPE_UINT32;
  sceneResource.meshes.push_back(mesh);

  // Maintain mapping for draw-time lookups.
  sceneResource.meshToBufferIndex.push_back(bufferIndex);
  sceneResource.meshMaterialIndices.push_back(0);
}

tinygltf::Model LoadGltfResources(const std::filesystem::path& filename)
{
  nvutils::ScopedTimer Timer(__FUNCTION__);

  tinygltf::TinyGLTF tinyLoader;
  tinygltf::Model    model;
  std::string        err, warn;

  if(filename.extension() == ".gltf")
  {
    if(!tinyLoader.LoadASCIIFromFile(&model, &err, &warn, filename.string()))
    {
      LOGE("Error loading glTF file: %s\n", err.c_str());
      assert(0 && "No fallback");
      return {};
    }
  }
  else if(filename.extension() == ".glb")
  {
    if(!tinyLoader.LoadBinaryFromFile(&model, &err, &warn, filename.string()))
    {
      LOGE("Error loading glTF file: %s\n", err.c_str());
      assert(0 && "No fallback");
      return {};
    }
  }
  else
  {
    LOGE("Unsupported file format: %s\n", filename.extension().string().c_str());
    assert(0 && "No fallback");
    return {};
  }

  LOGI("%s", fmt::format("\n{}Loaded glTF file: {}", Timer.indent(), filename.string()).c_str());
  return model;
}

void ImportGltfData(GltfSceneResource& sceneResource, const tinygltf::Model& model, nvvk::StagingUploader& stagingUploader,
                    bool importInstance, uint32_t materialOffset, uint32_t fallbackMaterialIndex)
{
  SCOPED_TIMER(__FUNCTION__);

  // Lambda: component byte size.
  auto GetElementByteSize = [](int type) -> uint32_t {
    return type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT ? 2U :
           type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT   ? 4U :
           type == TINYGLTF_COMPONENT_TYPE_FLOAT          ? 4U :
                                                            0U;
  };

  // Lambda: vector/matrix element count.
  auto GetTypeSize = [](int type) -> uint32_t {
    return type == TINYGLTF_TYPE_VEC2 ? 2U :
           type == TINYGLTF_TYPE_VEC3 ? 3U :
           type == TINYGLTF_TYPE_VEC4 ? 4U :
           type == TINYGLTF_TYPE_MAT2 ? 4U * 2U :
           type == TINYGLTF_TYPE_MAT3 ? 4U * 3U :
           type == TINYGLTF_TYPE_MAT4 ? 4U * 4U :
                                        0U;
  };

  // Lambda: extract a named vertex attribute from a primitive.
  auto ExtractAttribute = [&](const std::string& name, shaderio::BufferView& attr, const tinygltf::Primitive& primitive) {
    if(!primitive.attributes.contains(name))
    {
      attr.offset = static_cast<uint32_t>(-1);
      return;
    }

    const tinygltf::Accessor&   acc = model.accessors[primitive.attributes.at(name)];
    const tinygltf::BufferView& bv  = model.bufferViews[acc.bufferView];
    assert((acc.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) && "Should be floats");

    attr = {
        .offset     = static_cast<uint32_t>(bv.byteOffset + acc.byteOffset),
        .count      = static_cast<uint32_t>(acc.count),
        .byteStride = static_cast<uint32_t>(bv.byteStride ? static_cast<uint32_t>(bv.byteStride) : GetTypeSize(acc.type) * GetElementByteSize(acc.componentType)),
    };
  };

  // Upload the GLTF binary buffer (geometry blob) to the GPU.
  nvvk::Buffer bGltfData;
  uint32_t     bufferIndex = 0U;
  {
    nvvk::ResourceAllocator* allocator = stagingUploader.getResourceAllocator();

    NVVK_CHECK(allocator->createBuffer(bGltfData, std::span<const unsigned char>(model.buffers[0].data).size_bytes(),
                                       VK_BUFFER_USAGE_2_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_2_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT
                                           | VK_BUFFER_USAGE_2_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR));
    NVVK_CHECK(stagingUploader.appendBuffer(bGltfData, 0, std::span<const unsigned char>(model.buffers[0].data)));
    NVVK_DBG_NAME(bGltfData.buffer);

    bufferIndex = static_cast<uint32_t>(sceneResource.bGltfDatas.size());
    sceneResource.bGltfDatas.push_back(bGltfData);
  }

  // Map source mesh index -> one or more imported scene mesh indices (one per supported primitive).
  std::vector<std::vector<uint32_t>> meshToSceneMeshIndices(model.meshes.size());

  // Extract meshes (supports multiple triangle primitives per mesh).
  for(size_t meshIdx = 0; meshIdx < model.meshes.size(); ++meshIdx)
  {
    const tinygltf::Mesh& tinyMesh = model.meshes[meshIdx];
    for(const tinygltf::Primitive& primitive : tinyMesh.primitives)
    {
      if(primitive.mode != TINYGLTF_MODE_TRIANGLES)
      {
        continue;
      }
      if(primitive.indices < 0)
      {
        continue;
      }

      const tinygltf::Accessor& accessor = model.accessors[primitive.indices];
      if(accessor.bufferView < 0)
      {
        continue;
      }

      if(accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT
         && accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
      {
        continue;
      }

      shaderio::GltfMesh         mesh{};
      const tinygltf::BufferView bufferView = model.bufferViews[accessor.bufferView];
      assert((accessor.count % 3 == 0) && "Triangle indices should be a multiple of 3");

      mesh.triMesh.indices = {
          .offset     = static_cast<uint32_t>(bufferView.byteOffset + accessor.byteOffset),
          .count      = static_cast<uint32_t>(accessor.count),
          .byteStride = static_cast<uint32_t>(bufferView.byteStride ? bufferView.byteStride : GetElementByteSize(accessor.componentType)),
      };
      mesh.indexType = accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

      // Raw buffer base address.
      mesh.gltfBuffer = reinterpret_cast<uint8_t*>(bGltfData.address);

      // Attributes.
      ExtractAttribute("POSITION", mesh.triMesh.positions, primitive);
      ExtractAttribute("NORMAL", mesh.triMesh.normals, primitive);
      ExtractAttribute("COLOR_0", mesh.triMesh.colorVert, primitive);
      ExtractAttribute("TEXCOORD_0", mesh.triMesh.texCoords, primitive);
      ExtractAttribute("TANGENT", mesh.triMesh.tangents, primitive);

      const uint32_t sceneMeshIndex = static_cast<uint32_t>(sceneResource.meshes.size());
      uint32_t       materialIndex  = fallbackMaterialIndex;
      if(primitive.material >= 0 && primitive.material < static_cast<int>(model.materials.size()))
      {
        materialIndex = materialOffset + static_cast<uint32_t>(primitive.material);
      }

      sceneResource.meshes.emplace_back(mesh);
      sceneResource.meshToBufferIndex.push_back(bufferIndex);
      sceneResource.meshMaterialIndices.push_back(materialIndex);
      meshToSceneMeshIndices[meshIdx].push_back(sceneMeshIndex);
    }
  }

  if(!importInstance)
  {
    return;
  }

  // Extract instances with hierarchical transform accumulation.
  std::function<void(const tinygltf::Node&, const glm::mat4&)> ProcessNode =
      [&](const tinygltf::Node& node, const glm::mat4& parentTransform) {
        glm::mat4 nodeTransform = parentTransform;

        if(!node.matrix.empty())
        {
          glm::mat4 matrix = glm::make_mat4(node.matrix.data());
          nodeTransform    = parentTransform * matrix;
        }
        else
        {
          if(!node.translation.empty())
          {
            glm::vec3 translation = glm::make_vec3(node.translation.data());
            nodeTransform         = glm::translate(nodeTransform, translation);
          }
          if(!node.rotation.empty())
          {
            glm::quat rotation = glm::make_quat(node.rotation.data());
            nodeTransform      = nodeTransform * glm::mat4_cast(rotation);
          }
          if(!node.scale.empty())
          {
            glm::vec3 scale = glm::make_vec3(node.scale.data());
            nodeTransform   = glm::scale(nodeTransform, scale);
          }
        }

        if(node.mesh != -1)
        {
          if(node.mesh >= 0 && node.mesh < static_cast<int>(meshToSceneMeshIndices.size()))
          {
            for(const uint32_t sceneMeshIndex : meshToSceneMeshIndices[node.mesh])
            {
              shaderio::GltfInstance instance{};
              instance.meshIndex = sceneMeshIndex;
              instance.transform = nodeTransform;
              if(sceneMeshIndex < sceneResource.meshMaterialIndices.size())
              {
                instance.materialIndex = sceneResource.meshMaterialIndices[sceneMeshIndex];
              }
              else
              {
                instance.materialIndex = fallbackMaterialIndex;
              }
              sceneResource.instances.push_back(instance);
            }
          }
        }

        for(int childIdx : node.children)
        {
          if(childIdx >= 0 && childIdx < static_cast<int>(model.nodes.size()))
          {
            ProcessNode(model.nodes[childIdx], nodeTransform);
          }
        }
      };

  // Process all root nodes.
  for(size_t nodeIdx = 0; nodeIdx < model.nodes.size(); ++nodeIdx)
  {
    const tinygltf::Node& node = model.nodes[nodeIdx];

    bool isRootNode = true;
    for(const auto& otherNode : model.nodes)
    {
      for(int childIdx : otherNode.children)
      {
        if(childIdx == static_cast<int>(nodeIdx))
        {
          isRootNode = false;
          break;
        }
      }
      if(!isRootNode)
      {
        break;
      }
    }

    if(isRootNode)
    {
      ProcessNode(node, glm::mat4(1.0f));
    }
  }
}

void CreateGltfSceneInfoBuffer(GltfSceneResource& sceneResource, nvvk::StagingUploader& stagingUploader)
{
  SCOPED_TIMER(__FUNCTION__);

  nvvk::ResourceAllocator* allocator = stagingUploader.getResourceAllocator();

  // Mesh buffer.
  allocator->createBuffer(sceneResource.bMeshes, std::span(sceneResource.meshes).size_bytes(),
                          VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT | VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT);
  NVVK_DBG_NAME(sceneResource.bMeshes.buffer);
  NVVK_CHECK(stagingUploader.appendBuffer(sceneResource.bMeshes, 0, std::span<const shaderio::GltfMesh>(sceneResource.meshes)));

  // Instance buffer.
  allocator->createBuffer(sceneResource.bInstances, std::span(sceneResource.instances).size_bytes(),
                          VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT | VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT);
  NVVK_DBG_NAME(sceneResource.bInstances.buffer);
  NVVK_CHECK(stagingUploader.appendBuffer(sceneResource.bInstances, 0, std::span<const shaderio::GltfInstance>(sceneResource.instances)));

  // Material buffer.
  allocator->createBuffer(sceneResource.bMaterials, std::span(sceneResource.materials).size_bytes(),
                          VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT | VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT);
  NVVK_DBG_NAME(sceneResource.bMaterials.buffer);
  NVVK_CHECK(stagingUploader.appendBuffer(sceneResource.bMaterials, 0, std::span<const shaderio::GltfMetallicRoughness>(sceneResource.materials)));

  if(!sceneResource.emissiveTriangles.empty())
  {
    allocator->createBuffer(sceneResource.bEmissiveTriangles, std::span(sceneResource.emissiveTriangles).size_bytes(),
                            VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT | VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT);
    NVVK_DBG_NAME(sceneResource.bEmissiveTriangles.buffer);
    NVVK_CHECK(stagingUploader.appendBuffer(sceneResource.bEmissiveTriangles, 0,
                                            std::span<const shaderio::EmissiveTriangleLight>(sceneResource.emissiveTriangles)));
  }

  if(!sceneResource.emissiveTriangleCdf.empty())
  {
    allocator->createBuffer(sceneResource.bEmissiveTriangleCdf, std::span(sceneResource.emissiveTriangleCdf).size_bytes(),
                            VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT | VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT);
    NVVK_DBG_NAME(sceneResource.bEmissiveTriangleCdf.buffer);
    NVVK_CHECK(
        stagingUploader.appendBuffer(sceneResource.bEmissiveTriangleCdf, 0, std::span<const float>(sceneResource.emissiveTriangleCdf)));
  }

  if(!sceneResource.environmentCdf.empty())
  {
    allocator->createBuffer(sceneResource.bEnvironmentCdf, std::span(sceneResource.environmentCdf).size_bytes(),
                            VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT | VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT);
    NVVK_DBG_NAME(sceneResource.bEnvironmentCdf.buffer);
    NVVK_CHECK(stagingUploader.appendBuffer(sceneResource.bEnvironmentCdf, 0, std::span<const float>(sceneResource.environmentCdf)));
  }

  if(!sceneResource.environmentPdf.empty())
  {
    allocator->createBuffer(sceneResource.bEnvironmentPdf, std::span(sceneResource.environmentPdf).size_bytes(),
                            VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT | VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT);
    NVVK_DBG_NAME(sceneResource.bEnvironmentPdf.buffer);
    NVVK_CHECK(stagingUploader.appendBuffer(sceneResource.bEnvironmentPdf, 0, std::span<const float>(sceneResource.environmentPdf)));
  }

  // SceneInfo buffer.
  NVVK_CHECK(allocator->createBuffer(sceneResource.bSceneInfo,
                                     std::span<const shaderio::GltfSceneInfo>(&sceneResource.sceneInfo, 1).size_bytes(),
                                     VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT));
  NVVK_DBG_NAME(sceneResource.bSceneInfo.buffer);
  NVVK_CHECK(stagingUploader.appendBuffer(sceneResource.bSceneInfo, 0, std::span<const shaderio::GltfSceneInfo>(&sceneResource.sceneInfo, 1)));
}

}  // namespace nvsamples

