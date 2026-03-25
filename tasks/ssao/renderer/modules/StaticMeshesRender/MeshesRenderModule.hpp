#pragma once

#include <etna/Buffer.hpp>
#include <etna/GpuSharedResource.hpp>
#include <etna/ComputePipeline.hpp>
#include <etna/GraphicsPipeline.hpp>
#include <etna/DescriptorSet.hpp>
#include <etna/RenderTargetStates.hpp>

#include <glm/glm.hpp>

#include "scene/SceneManager.hpp"

#include "../RenderPacket.hpp"
#include "shaders/MeshesParams.h"


class MeshesRenderModule
{
public:
  MeshesRenderModule();

  void allocateResources();
  void loadShaders();
  void loadScene(std::filesystem::path path);
  void setupPipelines(
    bool wireframe_enabled,
    std::vector<vk::Format> color_attachent_formats,
    vk::Format depth_attachment_format,
    vk::Format shadow_attachment_format);

  void loadSet();

  void executeRender(
    vk::CommandBuffer cmd_buf,
    const RenderPacket& packet,
    const etna::Buffer& heavy_packet_info_buffer,
    std::vector<etna::RenderTargetState::AttachmentParams> color_attachment_params,
    etna::RenderTargetState::AttachmentParams depth_attachment_params);

  void executeShadowMapping(
    vk::CommandBuffer cmd_buf,
    vk::Extent2D extent,
    etna::BufferBinding light_info_binding,
    etna::RenderTargetState::AttachmentParams shadow_mapping_attachment_params);

  void drawGui();

  const etna::Sampler& getStaticMeshSampler() const { return staticMeshSampler; }

private:
  // TODO - rename
  struct Info
  {
    glm::mat4 translation;
  };

private:
  void cullMeshes(
    vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout, const glm::mat4x4& proj_view);

  void cullMeshes(
    vk::CommandBuffer cmd_buf,
    vk::PipelineLayout pipeline_layout,
    const etna::BufferBinding& proj_view_binding);

  void renderScene(
    vk::CommandBuffer cmd_buf,
    vk::PipelineLayout pipeline_layout,
    const etna::Buffer& heavy_packet_info_buffer);

private:
  MeshesParams params;
  etna::Buffer paramsBuffer;

  Info info;

  std::unique_ptr<SceneManager> sceneMgr;

  std::optional<etna::PersistentDescriptorSet> meshesDescriptorSet;

  etna::GraphicsPipeline staticMeshPipeline;
  etna::GraphicsPipeline staticMeshShadowPipeline;
  etna::ComputePipeline cullingPipeline;
  etna::ComputePipeline cullingShadowPipeline;

  etna::Sampler staticMeshSampler;

  std::unique_ptr<etna::OneShotCmdMgr> oneShotCommands;
  std::unique_ptr<etna::BlockingTransferHelper> transferHelper;
};

// static_assert(Module<MeshesRenderModule>);
