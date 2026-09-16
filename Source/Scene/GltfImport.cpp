// Implementations
// This translation unit compiles tinygltf and the stb image codecs it uses. STB_IMAGE_STATIC keeps these stb symbols private, because ImageData.cpp compiles its own copy of stb.

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "GltfImport.h"

#include <algorithm>
#include <functional>
#include <span>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vulkan/vulkan_core.h>

#include "Framework/Platform/Log.h"
#include "Framework/Vulkan/Diagnostics.h"

namespace rtpt
{

tinygltf::Model LoadGltfResources(const std::filesystem::path& filename)
{
  tinygltf::TinyGLTF tinyLoader;
  tinygltf::Model    model;
  std::string        err, warn;

  // The extension picks the parser: .gltf is JSON with external or embedded buffers, .glb is the binary container.
  if(filename.extension() == ".gltf")
  {
    if(!tinyLoader.LoadASCIIFromFile(&model, &err, &warn, filename.string()))
    {
      throw std::runtime_error("failed to load glTF file: " + err);
    }
  }
  else if(filename.extension() == ".glb")
  {
    if(!tinyLoader.LoadBinaryFromFile(&model, &err, &warn, filename.string()))
    {
      throw std::runtime_error("failed to load GLB file: " + err);
    }
  }
  else
  {
    throw std::invalid_argument("unsupported glTF file extension: " + filename.extension().string());
  }

  rtpt::Log(rtpt::LogLevel::Info, "Loaded glTF file: " + filename.string());

  return model;
}

void SetInstanceTransform(shaderio::GltfInstance& instance, const glm::mat4& transform)
{
  instance.transform = transform;

  // Inverted in double precision: the shader used to invert the same 3x3 in float at every hit, so this is the cheapest place to give it a closer answer.
  instance.normalTransform = glm::mat3(glm::inverse(glm::dmat3(transform)));
}

void ImportGltfData(GltfSceneResource& sceneResource, const tinygltf::Model& model, rtpt::ResourceAllocator& resources, rtpt::UploadContext& uploads, bool importInstance, uint32_t materialOffset, uint32_t fallbackMaterialIndex)
{
  // Accessor helpers
  // glTF describes each stream as an accessor into a buffer view. These helpers turn one into a shaderio::BufferView: a byte offset, an element count, and a stride.
  // Only the component types this importer accepts are sized; anything else reports 0 bytes.

  auto GetElementByteSize = [](int type) -> uint32_t {
    return type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT ? 2U :
           type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT   ? 4U :
           type == TINYGLTF_COMPONENT_TYPE_FLOAT          ? 4U :
                                                            0U;
  };

  // Number of scalar components in one vector or matrix element.
  auto GetTypeSize = [](int type) -> uint32_t {
    return type == TINYGLTF_TYPE_VEC2 ? 2U :
           type == TINYGLTF_TYPE_VEC3 ? 3U :
           type == TINYGLTF_TYPE_VEC4 ? 4U :
           type == TINYGLTF_TYPE_MAT2 ? 4U * 2U :
           type == TINYGLTF_TYPE_MAT3 ? 4U * 3U :
           type == TINYGLTF_TYPE_MAT4 ? 4U * 4U :
                                        0U;
  };

  // Extracts a named vertex attribute. A missing attribute gets offset 0xFFFFFFFF, which CPU readers treat as absent; shaders check for a zero count instead.
  auto ExtractAttribute = [&](const std::string& name, shaderio::BufferView& attr, const tinygltf::Primitive& primitive) {
    if(!primitive.attributes.contains(name))
    {
      attr.offset = static_cast<uint32_t>(-1);
      return;
    }

    const tinygltf::Accessor&   acc = model.accessors[primitive.attributes.at(name)];
    const tinygltf::BufferView& bv  = model.bufferViews[acc.bufferView];

    assert((acc.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) && "Should be floats");

    // A zero byteStride in glTF means tightly packed, so the stride is derived from the element size.
    attr = {
        .offset     = static_cast<uint32_t>(bv.byteOffset + acc.byteOffset),
        .count      = static_cast<uint32_t>(acc.count),
        .byteStride = static_cast<uint32_t>(bv.byteStride ? static_cast<uint32_t>(bv.byteStride) : GetTypeSize(acc.type) * GetElementByteSize(acc.componentType)),
    };
  };

  // Geometry blob
  // The model's first binary buffer is uploaded whole and every mesh stream is addressed inside it by offset.
  // One buffer serves the raster pipeline (index buffer), shaders (storage through its device address), and BLAS builds (build input), so it carries all of those usages.

  rtpt::Buffer bGltfData;
  uint32_t     bufferIndex = 0U;
  {
    const std::span<const unsigned char> source(model.buffers[0].data);

    rtpt::CheckVk(resources.CreateBuffer(bGltfData, source.size_bytes(), VK_BUFFER_USAGE_2_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_2_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE), "ResourceAllocator::CreateBuffer(glTF data)");

    uploads.UploadBuffer(bGltfData.buffer, 0, std::as_bytes(source), { .stages = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, .access = VK_ACCESS_2_MEMORY_READ_BIT });

    bufferIndex = static_cast<uint32_t>(sceneResource.bGltfDatas.size());
    sceneResource.bGltfDatas.push_back(std::move(bGltfData));
  }

  // Meshes
  // A glTF mesh can hold several primitives, and each supported triangle primitive becomes its own scene mesh with its own material.
  // Primitives that are not indexed triangle lists with 16- or 32-bit indices are skipped.

  // Maps source mesh index to the imported scene mesh indices, one per supported primitive, so node instancing can find them.
  std::vector<std::vector<uint32_t>> meshToSceneMeshIndices(model.meshes.size());

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

      if(accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT && accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
      {
        continue;
      }

      shaderio::GltfMesh         mesh {};
      const tinygltf::BufferView bufferView = model.bufferViews[accessor.bufferView];

      assert((accessor.count % 3 == 0) && "Triangle indices should be a multiple of 3");

      // The index stride doubles as the index width: CPU readers treat a 2-byte stride as 16-bit indices.
      mesh.triMesh.indices = {
          .offset     = static_cast<uint32_t>(bufferView.byteOffset + accessor.byteOffset),
          .count      = static_cast<uint32_t>(accessor.count),
          .byteStride = static_cast<uint32_t>(bufferView.byteStride ? bufferView.byteStride : GetElementByteSize(accessor.componentType)),
      };

      mesh.indexType = accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

      // Device address of the uploaded blob, stored in a pointer-typed field shared with shaders. The CPU never dereferences it.
      mesh.gltfBuffer = reinterpret_cast<uint8_t*>(sceneResource.bGltfDatas[bufferIndex].address);

      ExtractAttribute("POSITION", mesh.triMesh.positions, primitive);
      ExtractAttribute("NORMAL", mesh.triMesh.normals, primitive);
      ExtractAttribute("COLOR_0", mesh.triMesh.colorVert, primitive);
      ExtractAttribute("TEXCOORD_0", mesh.triMesh.texCoords, primitive);
      ExtractAttribute("TANGENT", mesh.triMesh.tangents, primitive);

      const uint32_t sceneMeshIndex = static_cast<uint32_t>(sceneResource.meshes.size());
      uint32_t       materialIndex  = fallbackMaterialIndex;

      // Model material indices are shifted into the scene material array; a missing or invalid one uses the fallback.
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

  // Without node instancing the caller creates instances itself.
  if(!importInstance)
  {
    return;
  }

  // Node instances
  // The node hierarchy is flattened: each node's transform is accumulated from its parents, and every scene mesh of a node's mesh becomes one world-space instance.
  // A node uses either its matrix or its TRS components, composed as translate * rotate * scale as glTF specifies.

  std::function<void(const tinygltf::Node&, const glm::mat4&)> ProcessNode = [&](const tinygltf::Node& node, const glm::mat4& parentTransform) {
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
      // Out-of-range mesh references are ignored rather than trusted.
      if(node.mesh >= 0 && node.mesh < static_cast<int>(meshToSceneMeshIndices.size()))
      {
        for(const uint32_t sceneMeshIndex : meshToSceneMeshIndices[node.mesh])
        {
          shaderio::GltfInstance instance {};

          instance.meshIndex = sceneMeshIndex;

          SetInstanceTransform(instance, nodeTransform);

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

  // Roots
  // A root is any node that no other node lists as a child. Every root across the file is processed, not only the nodes of the default glTF scene.

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

namespace
{

void CreateGltfSceneDataBuffersImpl(GltfSceneResource& sceneResource, rtpt::ResourceAllocator& resources, rtpt::UploadContext& uploads)
{
  // Scene arrays
  // Every array is a storage buffer that shaders reach through a device address stored in GltfSceneInfo.

  constexpr VkBufferUsageFlags2 dataUsage = VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT | VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT;

  constexpr rtpt::AccessScope shaderRead {
      .stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .access = VK_ACCESS_2_SHADER_READ_BIT,
  };

  // An empty array creates no buffer, which leaves its device address at zero.
  const auto createAndUpload = [&](rtpt::Buffer& destination, auto source, VkBufferUsageFlags2 usage, rtpt::AccessScope destinationAccess, const char* operation) {
    const std::span values(source);

    if(values.empty())
    {
      return;
    }

    rtpt::CheckVk(resources.CreateBuffer(destination, values.size_bytes(), usage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE), operation);

    uploads.UploadBuffer(destination.buffer, 0, std::as_bytes(values), destinationAccess);
  };

  createAndUpload(sceneResource.bMeshes, std::span<const shaderio::GltfMesh>(sceneResource.meshes), dataUsage, shaderRead, "ResourceAllocator::CreateBuffer(scene meshes)");
  createAndUpload(sceneResource.bInstances, std::span<const shaderio::GltfInstance>(sceneResource.instances), dataUsage, shaderRead, "ResourceAllocator::CreateBuffer(scene instances)");
  createAndUpload(sceneResource.bMaterials, std::span<const shaderio::GltfMetallicRoughness>(sceneResource.materials), dataUsage, shaderRead, "ResourceAllocator::CreateBuffer(scene materials)");
  createAndUpload(sceneResource.bEmissiveTriangles, std::span<const shaderio::EmissiveTriangleLight>(sceneResource.emissiveTriangles), dataUsage, shaderRead, "ResourceAllocator::CreateBuffer(emissive triangles)");
  createAndUpload(sceneResource.bEmissiveTriangleCdf, std::span<const float>(sceneResource.emissiveTriangleCdf), dataUsage, shaderRead, "ResourceAllocator::CreateBuffer(emissive triangle CDF)");
  createAndUpload(sceneResource.bEnvironmentCdf, std::span<const float>(sceneResource.environmentCdf), dataUsage, shaderRead, "ResourceAllocator::CreateBuffer(environment CDF)");
  createAndUpload(sceneResource.bEnvironmentPdf, std::span<const float>(sceneResource.environmentPdf), dataUsage, shaderRead, "ResourceAllocator::CreateBuffer(environment PDF)");
}

}  // namespace

void CreateGltfSceneDataBuffers(GltfSceneResource& sceneResource, rtpt::ResourceAllocator& resources, rtpt::UploadContext& uploads)
{
  CreateGltfSceneDataBuffersImpl(sceneResource, resources, uploads);
}

void CreateGltfSceneInfoBuffer(GltfSceneResource& sceneResource, rtpt::ResourceAllocator& resources, rtpt::UploadContext& uploads)
{
  // Scene uniform
  // Shaders reach the uniform through its device address in their push constants. TRANSFER_DST also lets SceneRuntime rewrite it every frame with vkCmdUpdateBuffer.

  constexpr VkPipelineStageFlags2 shaderReadStages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

  rtpt::CheckVk(resources.CreateBuffer(sceneResource.bSceneInfo, sizeof(shaderio::GltfSceneInfo), VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE), "ResourceAllocator::CreateBuffer(scene info)");

  uploads.UploadBuffer(sceneResource.bSceneInfo.buffer, 0, std::as_bytes(std::span<const shaderio::GltfSceneInfo>(&sceneResource.sceneInfo, 1)), { .stages = shaderReadStages, .access = VK_ACCESS_2_UNIFORM_READ_BIT });
}

}  // namespace rtpt
