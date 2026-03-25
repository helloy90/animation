#pragma once

#include <glm/glm.hpp>

#include <etna/Buffer.hpp>
#include <etna/Image.hpp>
#include <etna/ComputePipeline.hpp>
#include <etna/Sampler.hpp>
#include <etna/OneShotCmdMgr.hpp>
#include <etna/BlockingTransferHelper.hpp>
#include <etna/GpuSharedResource.hpp>


class SSAOModule
{
public:
  struct AllocationInfo
  {
    glm::uvec2 noiseTextureSize;
    uint32_t kernelSize;
    uint32_t seed;
  };

public:
  SSAOModule();

  void allocateResources(const AllocationInfo& info);
  void loadShaders();
  void setupPipelines();

  void prepareForExecute(
    const glm::mat4& proj_matrix,
    const glm::mat4& inv_proj_matrix,
    const glm::mat4& inv_view_matrix);

  void execute(
    vk::CommandBuffer cmd_buf,
    const etna::Image& normal_texture,
    const etna::Image& depth_texture,
    const etna::Sampler& depth_sampler,
    const etna::Image& occlusion_texture);

  void drawGui();

private:
  struct PushConstants
  {
    uint32_t kernelSize;
    float radius;
    float occlusionPower;
  };

private:
  void occlude(
    vk::CommandBuffer cmd_buf,
    const etna::Buffer& current_matrices,
    const etna::Image& normal_texture,
    const etna::Image& depth_texture,
    const etna::Sampler& depth_sampler,
    const etna::Image& occlusion_texture);

  void blur(vk::CommandBuffer cmd_buf, const etna::Image& occlusion_texture);

private:
  etna::Image noiseTexture;
  etna::Buffer ssaoKernel;
  PushConstants pushConstants;

  // half measure
  std::optional<etna::GpuSharedResource<etna::Buffer>> matrices;

  etna::Sampler noiseSampler;

  etna::ComputePipeline ssaoPipeline;
  etna::ComputePipeline blurPipeline;

  std::unique_ptr<etna::OneShotCmdMgr> oneShotCommands;
  std::unique_ptr<etna::BlockingTransferHelper> transferHelper;
};
