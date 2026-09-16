#include "Framework/Vulkan/AccelerationStructures.h"
#include "Framework/Vulkan/Diagnostics.h"
#include "Framework/Vulkan/Descriptors.h"
#include "Framework/Vulkan/GpuExecution.h"
#include "Framework/Vulkan/GpuProfiler.h"
#include "Framework/Vulkan/GpuResources.h"
#include "Framework/Vulkan/Pipelines.h"
#include "Framework/Vulkan/ReadbackContext.h"
#include "Framework/Vulkan/ShaderBindingTable.h"
#include "Framework/Vulkan/UploadContext.h"
#include "Framework/Vulkan/VulkanDevice.h"
#include "Framework/Vulkan/VulkanInstance.h"
#include "Rendering/ViewportTargets.h"

#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <iostream>
#include <utility>

#include "Generated/Shaders/RayTracingProbe.hlsl.library.h"

int main()
{
  try
  {
    // Instance and device
    // Headless: no window extensions are requested. Validation and synchronization validation are enabled, and any reported error fails the test at teardown.

    rtpt::VulkanInstance instance;

    instance.Initialize({ .applicationName = "RtptVulkanBootstrapTests", .validation = true, .synchronizationValidation = true });

    rtpt::VulkanDevice device;

    device.Initialize(instance.Handle());

    // Push descriptors
    // A pack created with zero sets and the push descriptor layout flag has no allocated set, so its writes must leave dstSet null.

    rtpt::DescriptorBindings pushBindings;

    pushBindings.Add(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);

    rtpt::DescriptorPack pushDescriptors;

    rtpt::CheckVk(pushDescriptors.Initialize(device.Handle(), pushBindings, 0, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR), "DescriptorPack::Initialize(push descriptor test)");

    const VkWriteDescriptorSet pushWrite = pushDescriptors.MakeWrite(0);

    if(pushWrite.dstSet != VK_NULL_HANDLE)
    {
      std::cerr << "push descriptor write unexpectedly targets an allocated set\n";
      return 1;
    }

    // Execution, profiling, and allocation
    // Two frame slots, with room for four timestamp scopes per frame.

    rtpt::GpuExecution execution;

    execution.Initialize(device.Handle(), device.RenderQueue(), device.Queues().renderFamily, 2);

    rtpt::GpuProfiler profiler;

    rtpt::CheckVk(profiler.Initialize(device.PhysicalDevice(), device.Handle(), device.Queues().renderFamily, 2, 4), "GpuProfiler::Initialize");

    rtpt::ResourceAllocator resources;

    resources.Initialize(instance.Handle(), device.PhysicalDevice(), device.Handle(), instance.ApiVersion(), execution);

    // Sampler ownership
    // Moving a Sampler must leave the source empty and the destination owning the handle.

    const VkSamplerCreateInfo samplerInfo {
      .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter    = VK_FILTER_LINEAR,
      .minFilter    = VK_FILTER_LINEAR,
      .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod       = VK_LOD_CLAMP_NONE,
    };

    rtpt::Sampler sampler;

    rtpt::CheckVk(resources.CreateSampler(sampler, samplerInfo), "ResourceAllocator::CreateSampler(probe)");

    rtpt::Sampler movedSampler = std::move(sampler);

    if(sampler || !movedSampler)
    {
      std::cerr << "sampler ownership did not transfer during move construction\n";
      return 1;
    }

    // Viewport targets

    rtpt::ViewportTargets viewportTargets;

    viewportTargets.Initialize(resources, device.PhysicalDevice());

    if(!viewportTargets.Resize({ 64, 64 }) || !viewportTargets.Hdr() || !viewportTargets.Depth())
    {
      std::cerr << "viewport target creation did not produce usable HDR and depth views\n";
      return 1;
    }

    // Triangle geometry
    // One triangle spanning (0,0,0), (1,0,0), and (0,1,0). RayTracingProbe.hlsl aims one ray inside it and one outside.

    constexpr std::array<float, 9> triangleVertices { 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F };
    constexpr std::array<uint32_t, 3> triangleIndices { 0, 1, 2 };

    rtpt::Buffer vertexBuffer;
    rtpt::Buffer indexBuffer;

    rtpt::CheckVk(resources.CreateBuffer(vertexBuffer, sizeof(triangleVertices), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE), "ResourceAllocator::CreateBuffer(probe vertices)");
    rtpt::CheckVk(resources.CreateBuffer(indexBuffer, sizeof(triangleIndices), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE), "ResourceAllocator::CreateBuffer(probe indices)");

    rtpt::UploadContext uploads;

    uploads.Initialize(resources, execution);

    // Uploaded buffers are left in the access state an acceleration structure build reads them with.
    constexpr rtpt::AccessScope accelerationBuildInput {
      .stages = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      .access = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
    };

    uploads.UploadBuffer(vertexBuffer.buffer, 0, std::as_bytes(std::span(triangleVertices)), accelerationBuildInput);
    uploads.UploadBuffer(indexBuffer.buffer, 0, std::as_bytes(std::span(triangleIndices)), accelerationBuildInput);

    // Bottom-level acceleration structure

    const VkAccelerationStructureGeometryTrianglesDataKHR triangles {
      .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
      .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
      .vertexData   = { .deviceAddress = vertexBuffer.address },
      .vertexStride = sizeof(float) * 3,
      .maxVertex    = 2,
      .indexType    = VK_INDEX_TYPE_UINT32,
      .indexData    = { .deviceAddress = indexBuffer.address },
    };

    const VkAccelerationStructureGeometryKHR bottomGeometry {
      .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
      .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
      .geometry     = { .triangles = triangles },
      .flags        = VK_GEOMETRY_OPAQUE_BIT_KHR,
    };

    rtpt::AccelerationStructureBuild bottomBuild(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);

    bottomBuild.AddGeometry(bottomGeometry, { .primitiveCount = 1 });

    rtpt::CheckVk(bottomBuild.Finalize(device.Handle()), "AccelerationStructureBuild::Finalize(BLAS probe)");

    rtpt::AccelerationStructureBuilder accelerationBuilder;

    accelerationBuilder.Initialize(resources, execution, device.Support().accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment);

    rtpt::AccelerationStructure bottomLevel;

    rtpt::CheckVk(accelerationBuilder.Build(bottomBuild, bottomLevel), "AccelerationStructureBuilder::Build(BLAS probe)");

    // Top-level acceleration structure
    // A single identity-transformed instance of the triangle. Culling is disabled so the hit does not depend on winding order.

    const VkAccelerationStructureInstanceKHR instanceDescription {
      .transform                              = { .matrix = { { 1.0F, 0.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F, 0.0F } } },
      .instanceCustomIndex                    = 0,
      .mask                                   = 0xff,
      .instanceShaderBindingTableRecordOffset = 0,
      .flags                                  = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
      .accelerationStructureReference         = bottomLevel.address,
    };

    rtpt::Buffer instanceBuffer;

    rtpt::CheckVk(resources.CreateBuffer(instanceBuffer, sizeof(instanceDescription), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE), "ResourceAllocator::CreateBuffer(probe instance)");

    uploads.UploadBuffer(instanceBuffer.buffer, 0, std::as_bytes(std::span(&instanceDescription, 1)), accelerationBuildInput);

    const VkAccelerationStructureGeometryInstancesDataKHR instances {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
      .data  = { .deviceAddress = instanceBuffer.address },
    };

    const VkAccelerationStructureGeometryKHR topGeometry {
      .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
      .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
      .geometry     = { .instances = instances },
    };

    rtpt::AccelerationStructureBuild topBuild(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);

    topBuild.AddGeometry(topGeometry, { .primitiveCount = 1 });

    rtpt::CheckVk(topBuild.Finalize(device.Handle()), "AccelerationStructureBuild::Finalize(TLAS probe)");

    rtpt::AccelerationStructure topLevel;

    rtpt::CheckVk(accelerationBuilder.Build(topBuild, topLevel), "AccelerationStructureBuilder::Build(TLAS probe)");

    // Ray tracing descriptors
    // Binding 0 is the TLAS and binding 1 receives one marker per ray, matching RayTracingProbe.hlsl.

    rtpt::Buffer rayResults;

    rtpt::CheckVk(resources.CreateBuffer(rayResults, sizeof(uint32_t) * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE), "ResourceAllocator::CreateBuffer(ray results)");

    rtpt::DescriptorBindings rayBindings;

    rayBindings.Add(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
    rayBindings.Add(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);

    rtpt::DescriptorPack rayDescriptors;

    rtpt::CheckVk(rayDescriptors.Initialize(device.Handle(), rayBindings), "DescriptorPack::Initialize(ray tracing probe)");

    const VkWriteDescriptorSetAccelerationStructureKHR accelerationWrite {
      .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
      .accelerationStructureCount = 1,
      .pAccelerationStructures    = &topLevel.accel,
    };

    const VkDescriptorBufferInfo resultBufferInfo { .buffer = rayResults.buffer, .range = rayResults.bufferSize };

    // Acceleration structure writes carry their payload in a pNext extension struct rather than an image or buffer info.
    std::array rayDescriptorWrites { rayDescriptors.MakeWrite(0), rayDescriptors.MakeWrite(1) };

    rayDescriptorWrites[0].pNext       = &accelerationWrite;
    rayDescriptorWrites[1].pBufferInfo = &resultBufferInfo;

    vkUpdateDescriptorSets(device.Handle(), static_cast<uint32_t>(rayDescriptorWrites.size()), rayDescriptorWrites.data(), 0, nullptr);

    // Ray tracing pipeline
    // Group 0 is raygen, group 1 the miss shader, and group 2 a triangle hit group with only a closest hit shader.

    VkPipelineLayout rayLayout = VK_NULL_HANDLE;

    const VkPipelineLayoutCreateInfo rayLayoutInfo {
      .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts    = rayDescriptors.LayoutPtr(),
    };

    rtpt::CheckVk(vkCreatePipelineLayout(device.Handle(), &rayLayoutInfo, nullptr, &rayLayout), "vkCreatePipelineLayout(ray tracing probe)");

    constexpr std::array rayStages {
      rtpt::RayTracingShaderStage { VK_SHADER_STAGE_RAYGEN_BIT_KHR, "raygenMain" },
      rtpt::RayTracingShaderStage { VK_SHADER_STAGE_MISS_BIT_KHR, "missMain" },
      rtpt::RayTracingShaderStage { VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, "closestHitMain" },
    };

    constexpr std::array rayGroups {
      VkRayTracingShaderGroupCreateInfoKHR { .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, .generalShader = 0, .closestHitShader = VK_SHADER_UNUSED_KHR, .anyHitShader = VK_SHADER_UNUSED_KHR, .intersectionShader = VK_SHADER_UNUSED_KHR },
      VkRayTracingShaderGroupCreateInfoKHR { .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, .generalShader = 1, .closestHitShader = VK_SHADER_UNUSED_KHR, .anyHitShader = VK_SHADER_UNUSED_KHR, .intersectionShader = VK_SHADER_UNUSED_KHR },
      VkRayTracingShaderGroupCreateInfoKHR { .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR, .generalShader = VK_SHADER_UNUSED_KHR, .closestHitShader = 2, .anyHitShader = VK_SHADER_UNUSED_KHR, .intersectionShader = VK_SHADER_UNUSED_KHR },
    };

    VkPipeline rayPipeline = VK_NULL_HANDLE;

    rtpt::CheckVk(rtpt::CreateRayTracingPipeline(device.Handle(), rayLayout, std::span(RayTracingProbe_hlsl), rayStages, rayGroups, 1, rayPipeline), "CreateRayTracingPipeline(probe)");

    // Shader binding table
    // Every region must start on a multiple of shaderGroupBaseAlignment, and the raygen region must hold exactly one record.

    constexpr std::array<uint32_t, 1> raygenGroups { 0 };
    constexpr std::array<uint32_t, 1> missGroups { 1 };
    constexpr std::array<uint32_t, 1> hitGroups { 2 };

    rtpt::ShaderBindingTable shaderBindingTable;

    rtpt::CheckVk(shaderBindingTable.Initialize(resources, rayPipeline, static_cast<uint32_t>(rayGroups.size()), device.Support().rayTracingProperties, { .raygen = raygenGroups, .miss = missGroups, .hit = hitGroups }), "ShaderBindingTable::Initialize(probe)");

    const auto& rayRegions = shaderBindingTable.Regions();
    const VkDeviceSize baseAlignment = device.Support().rayTracingProperties.shaderGroupBaseAlignment;

    if(rayRegions.raygen.deviceAddress % baseAlignment != 0 || rayRegions.miss.deviceAddress % baseAlignment != 0 || rayRegions.hit.deviceAddress % baseAlignment != 0 || rayRegions.raygen.size != rayRegions.raygen.stride)
    {
      std::cerr << "shader binding table does not satisfy device alignment requirements\n";
      return 1;
    }

    // Trace
    // A 2x1 dispatch launches ray 0, which hits, and ray 1, which misses. The submission must publish a completion point.

    const rtpt::CompletionPoint rayTraceCompletion = execution.ExecuteAndWait([&](VkCommandBuffer commandBuffer) {
      vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rayPipeline);

      const VkDescriptorSet rayDescriptorSet = rayDescriptors.Set();

      vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rayLayout, 0, 1, &rayDescriptorSet, 0, nullptr);
      vkCmdTraceRaysKHR(commandBuffer, &rayRegions.raygen, &rayRegions.miss, &rayRegions.hit, &rayRegions.callable, 2, 1, 1);
    });

    if(!rayTraceCompletion)
    {
      std::cerr << "ray tracing probe submission did not publish a completion point\n";
      return 1;
    }

    // Upload, readback, and timestamp profiling
    // Known values are uploaded, then copied back inside a profiled frame. The bytes must survive the round trip unchanged.
    // The frame's single profiler scope must read back with its name and a finite, non-negative duration.

    rtpt::Buffer buffer;

    rtpt::CheckVk(resources.CreateBuffer(buffer, 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE), "ResourceAllocator::CreateBuffer(test destination)");

    const std::array<uint32_t, 4> values { 1, 2, 3, 4 };

    uploads.UploadBuffer(buffer.buffer, 0, std::as_bytes(std::span(values)), { .stages = VK_PIPELINE_STAGE_2_COPY_BIT, .access = VK_ACCESS_2_TRANSFER_READ_BIT });

    const rtpt::FrameContext frame = execution.BeginFrame();

    profiler.BeginFrame(frame.commands, frame.slot);

    const rtpt::GpuProfileScope copyScope = profiler.BeginScope(frame.commands, frame.slot, "buffer readback");

    rtpt::ReadbackContext readbacks;

    readbacks.Initialize(resources, execution);

    rtpt::ReadbackTicket readback = readbacks.RecordBufferCopy(frame, buffer.buffer, 0, sizeof(values), { .stages = VK_PIPELINE_STAGE_2_COPY_BIT, .access = VK_ACCESS_2_TRANSFER_READ_BIT });

    profiler.EndScope(frame.commands, copyScope);

    const rtpt::CompletionPoint completion = execution.SubmitFrame();

    readbacks.Commit(readback, completion);

    const std::span<const std::byte> bytes = readbacks.Bytes(readback);

    if(bytes.size() != sizeof(values) || std::memcmp(bytes.data(), values.data(), sizeof(values)) != 0)
    {
      std::cerr << "GPU upload/readback did not preserve bytes\n";
      return 1;
    }

    readbacks.Release(readback);

    const std::vector<rtpt::GpuProfileResult> profileResults = profiler.Read(frame.slot);

    if(profileResults.size() != 1 || profileResults.front().name != "buffer readback" || !std::isfinite(profileResults.front().milliseconds) || profileResults.front().milliseconds < 0.0)
    {
      std::cerr << "GPU timestamp profiling did not return the recorded scope\n";
      return 1;
    }

    // Ray results
    // RayTracingProbe.hlsl writes 0x1234 for the ray that hits and 0x5678 for the one that misses.
    // Reading both markers back proves the raygen, closest hit, and miss stages were bound and the payload routed through the right shaders.

    const rtpt::FrameContext rayReadbackFrame = execution.BeginFrame();

    rtpt::ReadbackTicket rayReadback = readbacks.RecordBufferCopy(rayReadbackFrame, rayResults.buffer, 0, sizeof(uint32_t) * 2, { .stages = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, .access = VK_ACCESS_2_SHADER_WRITE_BIT });

    const rtpt::CompletionPoint rayReadbackCompletion = execution.SubmitFrame();

    readbacks.Commit(rayReadback, rayReadbackCompletion);

    const std::span<const std::byte> rayBytes = readbacks.Bytes(rayReadback);

    constexpr std::array<uint32_t, 2> expectedRayResults { 0x1234u, 0x5678u };

    if(rayBytes.size() != sizeof(expectedRayResults) || std::memcmp(rayBytes.data(), expectedRayResults.data(), sizeof(expectedRayResults)) != 0)
    {
      std::cerr << "ray tracing probe did not preserve hit/miss payload routing\n";
      return 1;
    }

    readbacks.Release(rayReadback);

    // Deferred resize retirement
    // Resizing during an active frame must keep the old targets alive, so the live count rises by one full set of targets.
    // Only after that frame completes and retired resources are collected does the count return to where it was.

    const uint32_t resourcesBeforeResize = resources.LiveResourceCount();

    (void)execution.BeginFrame();

    if(!viewportTargets.Resize({ 32, 16 }) || viewportTargets.Extent().width != 32 || viewportTargets.Extent().height != 16 || resources.LiveResourceCount() != resourcesBeforeResize + rtpt::ViewportTargets::kTargetCount)
    {
      std::cerr << "viewport resize did not retain old targets through the active frame\n";
      return 1;
    }

    const rtpt::CompletionPoint resizeCompletion = execution.SubmitFrame();

    execution.Wait(resizeCompletion);
    execution.CollectRetiredResources();

    if(resources.LiveResourceCount() != resourcesBeforeResize)
    {
      std::cerr << "viewport resize did not retire superseded targets after frame completion\n";
      return 1;
    }

    // Retirement in a submitted frame
    // A release retired during a frame must not run before submission, and must run once the frame completes and retirements are collected.

    bool submittedRetirementRan = false;

    (void)execution.BeginFrame();

    execution.Retire([&submittedRetirementRan] { submittedRetirementRan = true; });

    if(submittedRetirementRan)
    {
      std::cerr << "resource retirement ran before its frame was submitted\n";
      return 1;
    }

    const rtpt::CompletionPoint retirementCompletion = execution.SubmitFrame();

    execution.Wait(retirementCompletion);
    execution.CollectRetiredResources();

    if(!submittedRetirementRan)
    {
      std::cerr << "submitted-frame resource retirement did not run after completion\n";
      return 1;
    }

    // Retirement in a cancelled frame
    // A cancelled frame never gets a completion point of its own, so its retirements resolve against the previous completion, which has already finished, and run during CancelFrame.

    bool cancelledRetirementRan = false;

    (void)execution.BeginFrame();

    execution.Retire([&cancelledRetirementRan] { cancelledRetirementRan = true; });
    execution.CancelFrame();

    if(!cancelledRetirementRan)
    {
      std::cerr << "cancelled-frame resource retirement did not resolve against prior completion\n";
      return 1;
    }

    // Teardown
    // Everything is destroyed explicitly, then Drain releases retired resources so any remaining live resource is a genuine leak.
    // Validation errors are read before the instance that owns the debug messenger is destroyed.

    resources.DestroyBuffer(buffer);
    shaderBindingTable.Destroy();
    vkDestroyPipeline(device.Handle(), rayPipeline, nullptr);
    vkDestroyPipelineLayout(device.Handle(), rayLayout, nullptr);
    rayDescriptors.Destroy();
    resources.DestroyBuffer(rayResults);
    resources.DestroyAccelerationStructure(topLevel);
    resources.DestroyBuffer(instanceBuffer);
    resources.DestroyAccelerationStructure(bottomLevel);
    resources.DestroyBuffer(indexBuffer);
    resources.DestroyBuffer(vertexBuffer);
    viewportTargets.Destroy();
    resources.DestroySampler(movedSampler);
    execution.Drain();

    if(resources.LiveResourceCount() != 0)
    {
      std::cerr << "resource allocator retained live objects after explicit teardown\n";
      return 1;
    }

    resources.Destroy();
    profiler.Destroy();
    execution.Destroy();
    pushDescriptors.Destroy();
    device.Destroy();

    if(instance.Debug().ErrorCount() != 0)
    {
      std::cerr << "validation reported " << instance.Debug().ErrorCount() << " error(s)\n";
      return 1;
    }

    instance.Destroy();

    return 0;
  }
  catch(const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
