#include "RasterRenderer.h"

#include <array>

#include <glm/gtc/matrix_inverse.hpp>

#include "Framework/Vulkan/Diagnostics.h"
#include "Framework/Vulkan/Pipelines.h"

#include "Generated/Shaders/Rasterizer.hlsl.fragmentMain.h"
#include "Generated/Shaders/Rasterizer.hlsl.vertexMain.h"
#include "Generated/Shaders/Sky.hlsl.main.h"

namespace rtpt
{

RasterRenderer::RasterRenderer(const CreateInfo& createInfo)
    : m_Device(createInfo.device)
    , m_MaxTextureDescriptors(createInfo.maxTextureDescriptors)
    , m_ColorFormat(createInfo.colorFormat)
    , m_DepthFormat(createInfo.depthFormat)
{
}

RasterRenderer::~RasterRenderer()
{
  Destroy();
}

void RasterRenderer::Initialize()
{
  // A missing dependency leaves the renderer unready rather than half-initialized; IsReady reports it.
  if(m_Device == nullptr || m_MaxTextureDescriptors == 0 || m_ColorFormat == VK_FORMAT_UNDEFINED || m_DepthFormat == VK_FORMAT_UNDEFINED)
  {
    return;
  }

  // Permanent Vulkan objects
  // Created in dependency order: the descriptor layout feeds the pipeline layout, and the pipelines are built against it. The sky pass is self-contained and comes last.
  // The shaders come from the SPIR-V embedded in the generated shader headers.

  CreateDescriptorSetLayout();
  CreatePipelineLayout();
  CreateGraphicsPipelines();

  rtpt::CheckVk(m_Sky.Initialize(m_Device->Handle(), std::span(Sky_hlsl)), "SkyRenderer::Initialize");
}

void RasterRenderer::Destroy()
{
  // Without a device nothing below was ever created.
  if(m_Device == nullptr)
  {
    return;
  }

  VkDevice device = m_Device->Handle();

  // Pipeline state
  // The pipelines were built from the layout, and the layout from the descriptor layout, so they go in that order. Destroying a null handle is a no-op.

  m_Sky.Destroy();

  vkDestroyPipeline(device, m_BackgroundPipeline, nullptr);
  vkDestroyPipeline(device, m_MeshPipeline, nullptr);
  vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);

  m_BackgroundPipeline = VK_NULL_HANDLE;
  m_MeshPipeline       = VK_NULL_HANDLE;
  m_PipelineLayout     = VK_NULL_HANDLE;

  m_DescPack.Destroy();
}

bool RasterRenderer::IsReady() const
{
  return m_MeshPipeline != VK_NULL_HANDLE && m_BackgroundPipeline != VK_NULL_HANDLE && m_PipelineLayout != VK_NULL_HANDLE;
}

rtpt::DescriptorPack& RasterRenderer::GetDescriptorPack()
{
  return m_DescPack;
}

const rtpt::DescriptorPack& RasterRenderer::GetDescriptorPack() const
{
  return m_DescPack;
}

void RasterRenderer::CreateDescriptorSetLayout()
{
  // Texture bindings
  // Scene textures are exposed as descriptor-indexed arrays of maxTextureDescriptors entries. Partially bound lets a scene fill only some of them,
  // and update-after-bind lets Application rewrite them on scene reloads.
  // eTextures is the legacy combined-image-sampler array; eHlslTextures and eHlslTextureSamplers are the separate arrays DXC-compiled shaders declare.

  rtpt::DescriptorBindings bindings;

  constexpr VkDescriptorBindingFlags textureFlags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;

  bindings.Add(shaderio::BindingPoints::eTextures, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_MaxTextureDescriptors, VK_SHADER_STAGE_ALL, textureFlags);
  bindings.Add(shaderio::BindingPoints::eHlslTextures, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, m_MaxTextureDescriptors, VK_SHADER_STAGE_ALL_GRAPHICS, textureFlags);
  bindings.Add(shaderio::BindingPoints::eHlslTextureSamplers, VK_DESCRIPTOR_TYPE_SAMPLER, m_MaxTextureDescriptors, VK_SHADER_STAGE_ALL_GRAPHICS, textureFlags);

  // Descriptor pack
  // The raster pass has no per-frame descriptors, so a single set serves every frame slot. The pool flags must match the update-after-bind texture bindings above.

  rtpt::CheckVk(m_DescPack.Initialize(m_Device->Handle(), bindings, 1, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT), "DescriptorPack::Initialize(raster)");
}

void RasterRenderer::CreatePipelineLayout()
{
  // One layout serves both graphics pipelines, with a push constant visible to every graphics stage.
  const VkPushConstantRange pushRange {
      .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
      .offset     = 0,
      .size       = sizeof(shaderio::RasterPushConstant),
  };

  rtpt::CheckVk(rtpt::CreatePipelineLayout(m_Device->Handle(), m_PipelineLayout, std::span(m_DescPack.LayoutPtr(), 1), std::span(&pushRange, 1)), "CreatePipelineLayout(raster)");
}

void RasterRenderer::CreateGraphicsPipelines()
{
  // Shader modules
  // Only needed while the pipelines are created, so they are locals released when this function returns.

  ShaderModule vertex;
  ShaderModule fragment;

  rtpt::CheckVk(vertex.Initialize(m_Device->Handle(), std::span(Rasterizer_vertex_hlsl)), "ShaderModule::Initialize(raster vertex)");
  rtpt::CheckVk(fragment.Initialize(m_Device->Handle(), std::span(Rasterizer_fragment_hlsl)), "ShaderModule::Initialize(raster fragment)");

  // Variants
  // Mesh tests and writes depth for scene geometry. Background has depth disabled and draws the HDRI fullscreen triangle before the meshes.

  rtpt::CheckVk(CreatePipelineVariant(vertex.Get(), fragment.Get(), true, m_MeshPipeline), "vkCreateGraphicsPipelines(raster mesh)");
  rtpt::CheckVk(CreatePipelineVariant(vertex.Get(), fragment.Get(), false, m_BackgroundPipeline), "vkCreateGraphicsPipelines(raster background)");
}

VkResult RasterRenderer::CreatePipelineVariant(VkShaderModule vertex, VkShaderModule fragment, bool depthEnabled, VkPipeline& pipeline) const
{
  // Stages
  // Entry point names must match the vertexMain and fragmentMain entry points in Rasterizer.hlsl.

  const std::array stages {
      VkPipelineShaderStageCreateInfo {
          .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage  = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vertex,
          .pName  = "vertexMain",
      },
      VkPipelineShaderStageCreateInfo {
          .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fragment,
          .pName  = "fragmentMain",
      },
  };

  // Fixed-function state
  // The vertex shader fetches its vertices from scene buffers by SV_VertexID, so there is no vertex input state.
  // Culling is disabled. Viewport and scissor are dynamic, so viewport resizes never require rebuilding the pipelines.

  const VkPipelineVertexInputStateCreateInfo vertexInput {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };

  const VkPipelineInputAssemblyStateCreateInfo inputAssembly {
      .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };

  const VkPipelineViewportStateCreateInfo viewport {
      .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount  = 1,
  };

  const VkPipelineRasterizationStateCreateInfo rasterization {
      .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode    = VK_CULL_MODE_NONE,
      .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth   = 1.0F,
  };

  const VkPipelineMultisampleStateCreateInfo multisample {
      .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };

  const VkPipelineDepthStencilStateCreateInfo depthStencil {
      .sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable  = depthEnabled,
      .depthWriteEnable = depthEnabled,
      .depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL,
      .minDepthBounds   = 0.0F,
      .maxDepthBounds   = 1.0F,
  };

  const VkPipelineColorBlendAttachmentState blendAttachment {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };

  const VkPipelineColorBlendStateCreateInfo blend {
      .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments    = &blendAttachment,
  };

  constexpr std::array dynamicStates { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

  const VkPipelineDynamicStateCreateInfo dynamic {
      .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
      .pDynamicStates    = dynamicStates.data(),
  };

  // Pipeline
  // Dynamic rendering declares the attachment formats here instead of through a render pass.

  const VkPipelineRenderingCreateInfo rendering {
      .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount    = 1,
      .pColorAttachmentFormats = &m_ColorFormat,
      .depthAttachmentFormat   = m_DepthFormat,
  };

  const VkGraphicsPipelineCreateInfo createInfo {
      .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext               = &rendering,
      .stageCount          = static_cast<uint32_t>(stages.size()),
      .pStages             = stages.data(),
      .pVertexInputState   = &vertexInput,
      .pInputAssemblyState = &inputAssembly,
      .pViewportState      = &viewport,
      .pRasterizationState = &rasterization,
      .pMultisampleState   = &multisample,
      .pDepthStencilState  = &depthStencil,
      .pColorBlendState    = &blend,
      .pDynamicState       = &dynamic,
      .layout              = m_PipelineLayout,
  };

  return vkCreateGraphicsPipelines(m_Device->Handle(), VK_NULL_HANDLE, 1, &createInfo, nullptr, &pipeline);
}

void RasterRenderer::Render(const RenderInput& input)
{
  // Nothing can be recorded without the pipelines or the scene.
  if(!IsReady() || input.cmd == VK_NULL_HANDLE || input.sceneResource == nullptr || input.sceneInfo == nullptr)
  {
    return;
  }

  const rtpt::GltfSceneResource& sceneResource = *input.sceneResource;
  const shaderio::GltfSceneInfo& sceneInfo     = *input.sceneInfo;
  const VkExtent2D               viewportSize  = input.colorTarget.extent;

  shaderio::RasterPushConstant pushValues = {
      .sceneInfoAddress          = (shaderio::GltfSceneInfo*)sceneResource.bSceneInfo.address,
      .metallicRoughnessOverride = input.metallicRoughnessOverride,
  };

  // Background choice
  // An HDRI takes precedence over the procedural sky, matching the path tracers' SampleEnvironment; with neither, the color target is cleared to the background color.

  const bool useHdriBackground = (sceneInfo.useHdrEnv != 0) && (sceneInfo.environmentTextureIndex >= 0);
  const bool useProceduralSky  = (sceneInfo.useSky != 0) && !useHdriBackground;

  // Procedural sky
  // The sky is a compute pass that writes the whole color target before rasterization. The raster pass then loads that image instead of clearing it.

  if(useProceduralSky)
  {
    const VkImageMemoryBarrier2 prepareSky {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .dstStageMask     = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask    = VK_ACCESS_2_SHADER_WRITE_BIT,
        .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout        = VK_IMAGE_LAYOUT_GENERAL,
        .image            = input.colorTarget.image,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
    };

    const VkDependencyInfo prepareSkyDependency {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &prepareSky,
    };

    vkCmdPipelineBarrier2(input.cmd, &prepareSkyDependency);

    m_Sky.Run(input.cmd, viewportSize, input.viewMatrix, input.projectionMatrix, sceneInfo.skySimpleParam, input.colorTarget.Descriptor(VK_IMAGE_LAYOUT_GENERAL));
  }

  // Attachments
  // Depth is cleared every frame. Color keeps the sky when one was drawn and is otherwise cleared to the background color.

  VkRenderingAttachmentInfo colorAttachment {
      .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView   = input.colorTarget.view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp      = useProceduralSky ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
  };

  colorAttachment.clearValue = { .color = { sceneInfo.backgroundColor.x, sceneInfo.backgroundColor.y, sceneInfo.backgroundColor.z, 1.0f } };

  VkRenderingAttachmentInfo depthAttachment {
      .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView   = input.depthTarget.view,
      .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
      .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue  = { .depthStencil = { 1.0F, 0 } },
  };

  const VkRenderingInfo renderingInfo {
      .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea           = { .extent = viewportSize },
      .layerCount           = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments    = &colorAttachment,
      .pDepthAttachment     = &depthAttachment,
  };

  // Attachment layouts
  // The color target comes from the sky pass in GENERAL when a sky was drawn, and its old contents are discarded otherwise. Depth history is never needed.

  const VkImageMemoryBarrier2 transitions[] {
      {
          .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask     = useProceduralSky ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
          .srcAccessMask    = useProceduralSky ? VK_ACCESS_2_SHADER_WRITE_BIT : VK_ACCESS_2_NONE,
          .dstStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
          .oldLayout        = useProceduralSky ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          .image            = input.colorTarget.image,
          .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
      },
      {
          .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .dstStageMask     = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
          .dstAccessMask    = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout        = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
          .image            = input.depthTarget.image,
          .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .levelCount = 1, .layerCount = 1 },
      },
  };

  const VkDependencyInfo transitionDependency {
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 2,
      .pImageMemoryBarriers    = transitions,
  };

  vkCmdPipelineBarrier2(input.cmd, &transitionDependency);

  vkCmdBindDescriptorSets(input.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout, 0, 1, m_DescPack.SetPtr(), 0, nullptr);

  vkCmdBeginRendering(input.cmd, &renderingInfo);

  const VkViewport viewport { .x = 0.0F, .y = 0.0F, .width = static_cast<float>(viewportSize.width), .height = static_cast<float>(viewportSize.height), .minDepth = 0.0F, .maxDepth = 1.0F };
  const VkRect2D   scissor { .offset = { 0, 0 }, .extent = viewportSize };

  vkCmdSetViewport(input.cmd, 0, 1, &viewport);
  vkCmdSetScissor(input.cmd, 0, 1, &scissor);

  // HDRI background
  // A fullscreen triangle drawn before the meshes. An instance index of -1 tells the raster shaders to emit the triangle and sample the environment instead of loading an instance.

  if(useHdriBackground)
  {
    vkCmdBindPipeline(input.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_BackgroundPipeline);

    pushValues.instanceIndex = -1;

    vkCmdPushConstants(input.cmd, m_PipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pushValues), &pushValues);
    vkCmdDraw(input.cmd, 3, 1, 0, 0);
  }

  // Meshes
  // One indexed draw per instance. Vertex attributes are pulled in the shader from the glTF blob, so only the index buffer is bound; the push constants carry the instance index and its normal matrix.

  vkCmdBindPipeline(input.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_MeshPipeline);

  for(size_t i = 0; i < sceneResource.instances.size(); ++i)
  {
    const uint32_t                meshIndex = sceneResource.instances[i].meshIndex;
    const shaderio::GltfMesh&     gltfMesh  = sceneResource.meshes[meshIndex];
    const shaderio::TriangleMesh& triMesh   = gltfMesh.triMesh;

    // Inverse transpose keeps normals perpendicular under non-uniform scale.
    pushValues.normalMatrix  = glm::transpose(glm::inverse(glm::mat3(sceneResource.instances[i].transform)));
    pushValues.instanceIndex = static_cast<int>(i);

    vkCmdPushConstants(input.cmd, m_PipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pushValues), &pushValues);

    const uint32_t      bufferIndex = sceneResource.meshToBufferIndex[meshIndex];
    const rtpt::Buffer& v           = sceneResource.bGltfDatas[bufferIndex];

    vkCmdBindIndexBuffer(input.cmd, v.buffer, triMesh.indices.offset, VkIndexType(gltfMesh.indexType));
    vkCmdDrawIndexed(input.cmd, triMesh.indices.count, 1, 0, 0, 0);
  }

  vkCmdEndRendering(input.cmd);

  // Hand-off
  // Later passes read, write, or copy the color target, so it leaves the raster pass in GENERAL.

  const VkImageMemoryBarrier2 makeGeneral {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      .srcAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      .dstStageMask     = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .dstAccessMask    = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
      .oldLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .newLayout        = VK_IMAGE_LAYOUT_GENERAL,
      .image            = input.colorTarget.image,
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
  };

  const VkDependencyInfo finishDependency {
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers    = &makeGeneral,
  };

  vkCmdPipelineBarrier2(input.cmd, &finishDependency);
}

}  // namespace rtpt
