#pragma once

#include <etna/Buffer.hpp>
#include <etna/GpuSharedResource.hpp>
#include <etna/ComputePipeline.hpp>
#include <etna/DescriptorSet.hpp>

#include <glm/glm.hpp>

class TonemappingModule
{
public:
  TonemappingModule();
  explicit TonemappingModule(uint32_t bins_amount);

  void allocateResources();
  void loadShaders();
  void setupPipelines();
  void execute(vk::CommandBuffer cmd_buf, const etna::Image& render_target, glm::vec2 resolution);

private:
  void tonemappingShaderStart(
    vk::CommandBuffer cmd_buf,
    const etna::ComputePipeline& current_pipeline,
    std::string shader_program,
    std::vector<etna::Binding> bindings,
    std::optional<uint32_t> push_constant,
    glm::uvec2 group_count);

  std::optional<etna::GpuSharedResource<etna::Buffer>> histogramBuffer;
  std::optional<etna::GpuSharedResource<etna::Buffer>> histogramInfoBuffer;
  std::optional<etna::GpuSharedResource<etna::Buffer>> distributionBuffer;

  std::uint32_t binsAmount;

  etna::ComputePipeline calculateMinMaxPipeline;
  etna::ComputePipeline histogramPipeline;
  etna::ComputePipeline processHistogramPipeline;
  etna::ComputePipeline distributionPipeline;
  etna::ComputePipeline postprocessComputePipeline;
};

// static_assert(Module<TonemappingModule>);
