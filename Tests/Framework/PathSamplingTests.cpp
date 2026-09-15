#include "Framework/Vulkan/Barriers.h"
#include "Framework/Vulkan/Descriptors.h"
#include "Framework/Vulkan/GpuExecution.h"
#include "Framework/Vulkan/GpuResources.h"
#include "Framework/Vulkan/Pipelines.h"
#include "Framework/Vulkan/UploadContext.h"
#include "Framework/Vulkan/VulkanDevice.h"
#include "Framework/Vulkan/VulkanInstance.h"
#include "Sampling/SpatiotemporalBlueNoise.h"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "Generated/Shaders/PathSamplingProbe.hlsl.main.h"

// Checks the noise actually emitted by the shader. A seed-only integration
// would turn later dimensions white and fail this low-frequency energy bound.
bool CheckSampleStatistics(const std::byte* bytes)
{
  bool passed = true;

  for(uint32_t dimension = 0; dimension < 3; ++dimension)
  {
    double lowPower = 0.0;
    double mean = 0.0;

    for(uint32_t frame = 0; frame < 64; ++frame)
    {
      for(uint32_t axis = 0; axis < 2; ++axis)
      {
        for(uint32_t frequency = 1; frequency <= 2; ++frequency)
        {
          std::array<double, 64> cosine {};
          std::array<double, 64> sine {};

          for(uint32_t coordinate = 0; coordinate < 64; ++coordinate)
          {
            const double angle = 6.283185307179586 * frequency * coordinate / 64.0;

            cosine[coordinate] = std::cos(angle);
            sine[coordinate] = std::sin(angle);
          }

          double real = 0.0;
          double imaginary = 0.0;

          for(uint32_t y = 0; y < 64; ++y)
          {
            for(uint32_t x = 0; x < 64; ++x)
            {
              float sample = 0.0F;
              const uint32_t index = ((frame * 64u + y) * 64u + x) * 4u + dimension;

              std::memcpy(&sample, bytes + index * sizeof(float), sizeof(sample));

              const uint32_t coordinate = axis == 0 ? x : y;

              real += (sample - 0.5) * cosine[coordinate];
              imaginary += (sample - 0.5) * sine[coordinate];

              if(axis == 0 && frequency == 1) mean += sample;
            }
          }

          lowPower += real * real + imaginary * imaginary;
        }
      }
    }

    // Independent uniform samples have expected power N / 12 per Fourier mode.
    const double relativePower = lowPower / (64.0 * 4.0 * 4096.0 / 12.0);

    mean /= 64.0 * 4096.0;
    passed &= relativePower < 0.25 && std::abs(mean - 0.5) < 0.005;

    std::cout << "dimension " << dimension << ": mean=" << mean << ", low-frequency power / white expectation=" << relativePower << '\n';
  }

  return passed;
}

int main(int argc, char** argv)
{
  try
  {
    // Production upload and shader sampling
    // The probe uses the embedded asset, production sampling functions, and reservoir pack/unpack helpers on the actual device.

    rtpt::VulkanInstance instance;

    instance.Initialize({ .applicationName = "RtptPathSamplingTests", .validation = true, .synchronizationValidation = true });

    rtpt::VulkanDevice device;

    device.Initialize(instance.Handle());

    rtpt::GpuExecution execution;

    execution.Initialize(device.Handle(), device.RenderQueue(), device.Queues().renderFamily, 1);

    rtpt::ResourceAllocator resources;

    resources.Initialize(instance.Handle(), device.PhysicalDevice(), device.Handle(), instance.ApiVersion(), execution);

    rtpt::UploadContext uploads;

    uploads.Initialize(resources, execution);

    rtpt::SpatiotemporalBlueNoise noise({ .resources = &resources, .uploads = &uploads, .diagnostics = &instance.Debug() });

    noise.Initialize();

    constexpr uint32_t sampleCount = 64u * 64u * 64u;
    constexpr VkDeviceSize byteCount = sampleCount * 4u * sizeof(uint32_t);
    rtpt::Buffer output;

    rtpt::CheckVk(resources.CreateBuffer(output, byteCount, VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT), "CreateBuffer(samples)");

    rtpt::Buffer reservoirs;

    rtpt::CheckVk(resources.CreateBuffer(reservoirs, sampleCount * 64u, VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE), "CreateBuffer(reservoir storage)");

    rtpt::DescriptorBindings bindings;

    bindings.Add(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    bindings.Add(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    bindings.Add(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);

    rtpt::DescriptorPack descriptors;

    rtpt::CheckVk(descriptors.Initialize(device.Handle(), bindings, 0, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR), "Initialize(sample descriptors)");

    const VkPushConstantRange phaseRange { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) };

    const VkPipelineLayoutCreateInfo layoutInfo {
      .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts    = descriptors.LayoutPtr(),
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &phaseRange,
    };

    VkPipelineLayout layout = VK_NULL_HANDLE;

    rtpt::CheckVk(vkCreatePipelineLayout(device.Handle(), &layoutInfo, nullptr, &layout), "CreatePipelineLayout(samples)");

    VkPipeline pipeline = VK_NULL_HANDLE;

    rtpt::CheckVk(rtpt::CreateComputePipeline(device.Handle(), layout, std::span(PathSamplingProbe_hlsl), pipeline), "CreateComputePipeline(samples)");

    const VkDescriptorImageInfo imageInfo = noise.Descriptor();
    const VkDescriptorBufferInfo bufferInfo { output.buffer, 0, byteCount };
    const VkDescriptorBufferInfo reservoirInfo { reservoirs.buffer, 0, VK_WHOLE_SIZE };
    std::array<VkWriteDescriptorSet, 3> writes { descriptors.MakeWrite(0), descriptors.MakeWrite(1), descriptors.MakeWrite(2) };

    writes[0].pImageInfo = &imageInfo;
    writes[1].pBufferInfo = &bufferInfo;
    writes[2].pBufferInfo = &reservoirInfo;

    (void)execution.ExecuteAndWait([&](VkCommandBuffer commandBuffer) {
      vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdPushDescriptorSetKHR(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, uint32_t(writes.size()), writes.data());

      uint32_t phase = 0;

      vkCmdPushConstants(commandBuffer, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(phase), &phase);
      vkCmdDispatch(commandBuffer, 8, 8, 64);

      rtpt::CmdBufferBarrier(commandBuffer, reservoirs.buffer, 0, VK_WHOLE_SIZE, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT }, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT });

      phase = 1;

      vkCmdPushConstants(commandBuffer, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(phase), &phase);
      vkCmdDispatch(commandBuffer, 8, 8, 64);

      rtpt::CmdBufferBarrier(commandBuffer, output.buffer, 0, byteCount, { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT }, { VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT });
    });

    rtpt::CheckVk(resources.InvalidateBuffer(output, 0, byteCount), "InvalidateBuffer(samples)");

    uint32_t failures = 0;

    for(uint32_t sample = 0; sample < sampleCount; ++sample)
    {
      uint32_t flags = 0;

      std::memcpy(&flags, output.mapping + (sample * 4u + 3u) * sizeof(uint32_t), sizeof(flags));

      failures |= flags;
    }

    if(!CheckSampleStatistics(output.mapping)) failures |= 16u;

    // Optional raw dump lets the statistical checks measure the GPU results.
    if(argc == 2)
    {
      std::ofstream dump(argv[1], std::ios::binary);

      dump.write(reinterpret_cast<const char*>(output.mapping), byteCount);

      if(!dump) throw std::runtime_error("could not write sample dump");
    }

    vkDestroyPipeline(device.Handle(), pipeline, nullptr);
    vkDestroyPipelineLayout(device.Handle(), layout, nullptr);
    descriptors.Destroy();
    output.Reset();
    reservoirs.Reset();
    noise.Destroy();
    execution.Drain();
    resources.Destroy();
    execution.Destroy();
    device.Destroy();

    if(failures != 0 || instance.Debug().ErrorCount() != 0 || instance.Debug().WarningCount() != 0)
    {
      std::cerr << "sampling failures=" << failures << ", validation errors=" << instance.Debug().ErrorCount() << ", warnings=" << instance.Debug().WarningCount() << '\n';
      return 1;
    }

    std::cout << "PASS: 262144 GPU samples; range, full-width origin storage, endpoint preservation, replay, and independent estimator streams\n";
    return 0;
  }
  catch(const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
