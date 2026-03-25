#pragma once

#include <glm/glm.hpp>

#include <etna/DescriptorSet.hpp>
#include <etna/RenderTargetStates.hpp>
#include <etna/Sampler.hpp>
#include <etna/Image.hpp>


class GBuffer
{
public:
  struct CreateInfo
  {
    glm::uvec2 resolution;
    glm::uvec2 shadowMapsResolution;
    vk::Format renderTargetFormat;
    vk::Format normalsFormat;
    vk::Format velocityBufferFormat;
    vk::Format occlusionFormat;
    vk::Format depthFormat;
    vk::Format shadowsFormat;
    uint32_t shadowCascadesAmount;
  };

public:
  explicit GBuffer(const CreateInfo& info);

  // no flush
  void prepareForRender(vk::CommandBuffer cmd_buf);

  // used for transparent depth read/write
  // no flush
  void prepareForDepthReadWrite(vk::CommandBuffer cmd_buf);

  // no flush
  void prepareForDepthRead(vk::CommandBuffer cmd_buf, vk::PipelineStageFlagBits2 pipeline_stage);

  // NOTE - maybe specific function should be (re-)moved
  void prepareForOcclusionExecute(vk::CommandBuffer cmd_buf);

  // no flush
  void prepareForRead(vk::CommandBuffer cmd_buf);

  // used to set barrier for depth image for taa copy
  // no flush
  void prepareForDepthCopy(vk::CommandBuffer cmd_buf);

  // no flush
  void prepareForTaaExecute(vk::CommandBuffer cmd_buf);

  // no flush
  void prepareForVelocityReset(vk::CommandBuffer cmd_buf);

  void resetVelocityTexture(vk::CommandBuffer cmd_buf);

  std::vector<etna::RenderTargetState::AttachmentParams> genColorAttachmentParams(
    vk::AttachmentLoadOp load_op = vk::AttachmentLoadOp::eClear);

  etna::RenderTargetState::AttachmentParams genDepthAttachmentParams(
    vk::AttachmentLoadOp load_op = vk::AttachmentLoadOp::eClear,
    vk::AttachmentStoreOp store_op = vk::AttachmentStoreOp::eStore);

  etna::RenderTargetState::AttachmentParams genShadowMappingAttachmentParams(
    uint32_t index,
    vk::AttachmentLoadOp load_op = vk::AttachmentLoadOp::eClear,
    vk::AttachmentStoreOp store_op = vk::AttachmentStoreOp::eStore);

  vk::Extent2D getShadowTextureExtent() const
  {
    const auto& extent = shadows[0].getExtent();
    return {extent.width, extent.height};
  }

  vk::Format getShadowTextureFormat() const { return shadows[0].getFormat(); }

  const etna::Image& getAlbedoTexture() const { return albedo; }
  const etna::Image& getNormalTexture() const { return normal; }
  const etna::Image& getMaterialTexture() const { return material; }
  const etna::Image& getVelocityTexture() const { return velocity; }
  const etna::Image& getOcclusionTexture() const { return occlusion; }
  const etna::Image& getDepthTexture() const { return depth; }
  const std::vector<etna::Image>& getShadowTextures() const { return shadows; }
  const etna::Sampler& getDepthSampler() const { return depthSampler; }

private:
  etna::Image albedo;
  etna::Image normal;
  etna::Image material;
  etna::Image velocity;
  etna::Image occlusion;
  etna::Image depth;
  std::vector<etna::Image> shadows;

  etna::Sampler depthSampler;
};
