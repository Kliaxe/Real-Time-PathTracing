#include "SceneRenderer.h"

// Role:
// Implements scene raster pass setup, background handling, and mesh draw loop.

#include <glm/gtc/matrix_inverse.hpp>
#include <nvshaders_host/sky.hpp>
#include <nvutils/camera_manipulator.hpp>
#include <nvvk/barriers.hpp>
#include <nvvk/default_structs.hpp>

#include "Shaders/ShaderIo.h"

namespace nvsamples
{

void SceneRenderer::Render(const RenderInput& input) const
{
  NVVK_DBG_SCOPE(input.cmd);

  const nvsamples::GltfSceneResource& sceneResource = *input.sceneResource;
  const shaderio::GltfSceneInfo&      sceneInfo     = *input.sceneInfo;

  shaderio::RasterPushConstant pushValues = {
      .sceneInfoAddress          = (shaderio::GltfSceneInfo*)sceneResource.bSceneInfo.address,
      .metallicRoughnessOverride = *input.metallicRoughnessOverride,
  };
  const VkPushConstantsInfo pushInfo = {
      .sType      = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,
      .layout     = input.graphicsPipelineLayout,
      .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
      .offset     = 0,
      .size       = sizeof(shaderio::RasterPushConstant),
      .pValues    = &pushValues,
  };

  const bool useHdriBackground = (sceneInfo.useHdrEnv != 0) && (sceneInfo.environmentTextureIndex >= 0);
  const bool useProceduralSky  = (sceneInfo.useSky != 0) && !useHdriBackground;

  if(useProceduralSky)
  {
    const glm::mat4& viewMatrix = input.cameraManip->getViewMatrix();
    const glm::mat4& projMatrix = input.cameraManip->getPerspectiveMatrix();
    input.skySimple->runCompute(input.cmd, *input.viewportSize, viewMatrix, projMatrix, sceneInfo.skySimpleParam,
                                input.gBuffers->getDescriptorImageInfo(input.renderedImageIndex));
  }

  VkRenderingAttachmentInfo colorAttachment = DEFAULT_VkRenderingAttachmentInfo;
  colorAttachment.loadOp                    = useProceduralSky ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachment.imageView                 = input.gBuffers->getColorImageView(input.renderedImageIndex);
  colorAttachment.clearValue =
      {.color = {sceneInfo.backgroundColor.x, sceneInfo.backgroundColor.y, sceneInfo.backgroundColor.z, 1.0f}};

  VkRenderingAttachmentInfo depthAttachment = DEFAULT_VkRenderingAttachmentInfo;
  depthAttachment.imageView                 = input.gBuffers->getDepthImageView();
  depthAttachment.clearValue                = {.depthStencil = DEFAULT_VkClearDepthStencilValue};

  VkRenderingInfo renderingInfo      = DEFAULT_VkRenderingInfo;
  renderingInfo.renderArea           = DEFAULT_VkRect2D(input.gBuffers->getSize());
  renderingInfo.colorAttachmentCount = 1;
  renderingInfo.pColorAttachments    = &colorAttachment;
  renderingInfo.pDepthAttachment     = &depthAttachment;

  nvvk::cmdImageMemoryBarrier(input.cmd, {input.gBuffers->getColorImage(input.renderedImageIndex), VK_IMAGE_LAYOUT_GENERAL,
                                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
  nvvk::cmdImageMemoryBarrier(input.cmd,
                              {input.gBuffers->getDepthImage(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                               {VK_IMAGE_ASPECT_DEPTH_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}});

  const VkBindDescriptorSetsInfo bindDescriptorSetsInfo = {
      .sType              = VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO,
      .stageFlags         = VK_SHADER_STAGE_ALL_GRAPHICS,
      .layout             = input.graphicsPipelineLayout,
      .firstSet           = 0,
      .descriptorSetCount = 1,
      .pDescriptorSets    = input.descPack->getSetPtr(),
  };
  vkCmdBindDescriptorSets2(input.cmd, &bindDescriptorSetsInfo);

  vkCmdBeginRendering(input.cmd, &renderingInfo);

  input.dynamicPipeline->rasterizationState.cullMode = VK_CULL_MODE_NONE;
  input.dynamicPipeline->cmdApplyAllStates(input.cmd);
  input.dynamicPipeline->cmdSetViewportAndScissor(input.cmd, *input.viewportSize);
  vkCmdSetDepthTestEnable(input.cmd, VK_TRUE);

  input.dynamicPipeline->cmdBindShaders(input.cmd, {.vertex = input.vertexShader, .fragment = input.fragmentShader});
  vkCmdSetVertexInputEXT(input.cmd, 0, nullptr, 0, nullptr);

  if(useHdriBackground)
  {
    vkCmdSetDepthTestEnable(input.cmd, VK_FALSE);
    vkCmdSetDepthWriteEnable(input.cmd, VK_FALSE);
    pushValues.instanceIndex = -1;
    vkCmdPushConstants2(input.cmd, &pushInfo);
    vkCmdDraw(input.cmd, 3, 1, 0, 0);
    vkCmdSetDepthWriteEnable(input.cmd, VK_TRUE);
    vkCmdSetDepthTestEnable(input.cmd, VK_TRUE);
  }

  for(size_t i = 0; i < sceneResource.instances.size(); ++i)
  {
    const uint32_t                meshIndex = sceneResource.instances[i].meshIndex;
    const shaderio::GltfMesh&     gltfMesh  = sceneResource.meshes[meshIndex];
    const shaderio::TriangleMesh& triMesh   = gltfMesh.triMesh;

    pushValues.normalMatrix  = glm::transpose(glm::inverse(glm::mat3(sceneResource.instances[i].transform)));
    pushValues.instanceIndex = static_cast<int>(i);
    vkCmdPushConstants2(input.cmd, &pushInfo);

    const uint32_t      bufferIndex = sceneResource.meshToBufferIndex[meshIndex];
    const nvvk::Buffer& v           = sceneResource.bGltfDatas[bufferIndex];

    vkCmdBindIndexBuffer(input.cmd, v.buffer, triMesh.indices.offset, VkIndexType(gltfMesh.indexType));
    vkCmdDrawIndexed(input.cmd, triMesh.indices.count, 1, 0, 0, 0);
  }

  vkCmdEndRendering(input.cmd);

  nvvk::cmdImageMemoryBarrier(input.cmd, {input.gBuffers->getColorImage(input.renderedImageIndex),
                                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL});
  nvvk::cmdImageMemoryBarrier(input.cmd,
                              {input.gBuffers->getDepthImage(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                               {VK_IMAGE_ASPECT_DEPTH_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}});
}

}  // namespace nvsamples
