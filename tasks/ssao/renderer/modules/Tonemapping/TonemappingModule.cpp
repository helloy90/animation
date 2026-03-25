#include "TonemappingModule.hpp"

#include <tracy/Tracy.hpp>

#include <etna/GlobalContext.hpp>
#include <etna/Etna.hpp>
#include <etna/PipelineManager.hpp>
#include <etna/Profiling.hpp>

#include "shaders/UniformHistogramInfo.h"


TonemappingModule::TonemappingModule()
  : binsAmount(128)
{
}

TonemappingModule::TonemappingModule(uint32_t bins_amount)
  : binsAmount(bins_amount)
{
}

void TonemappingModule::allocateResources()
{
  auto& ctx = etna::get_context();

  histogramBuffer.emplace(
    ctx.getMainWorkCount(), [&ctx, binsAmount = this->binsAmount](std::size_t i) {
      return ctx.createBuffer(
        etna::Buffer::CreateInfo{
          .size = binsAmount * sizeof(int32_t),
          .bufferUsage =
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
          .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
          .name = fmt::format("histogram{}", i)});
    });

  histogramInfoBuffer.emplace(ctx.getMainWorkCount(), [&ctx](std::size_t i) {
    return ctx.createBuffer(
      etna::Buffer::CreateInfo{
        .size = sizeof(UniformHistogramInfo),
        .bufferUsage =
          vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
        .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .name = fmt::format("histogram_info{}", i)});
  });

  distributionBuffer.emplace(
    ctx.getMainWorkCount(), [&ctx, binsAmount = this->binsAmount](std::size_t i) {
      return ctx.createBuffer(
        etna::Buffer::CreateInfo{
          .size = binsAmount * sizeof(float),
          .bufferUsage =
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
          .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
          .name = fmt::format("distribution{}", i)});
    });
}

void TonemappingModule::loadShaders()
{
  etna::create_program(
    "min_max_calculation", {TONEMAPPING_MODULE_SHADERS_ROOT "calculate_min_max.comp.spv"});
  etna::create_program(
    "histogram_calculation", {TONEMAPPING_MODULE_SHADERS_ROOT "histogram.comp.spv"});
  etna::create_program(
    "histogram_processing", {TONEMAPPING_MODULE_SHADERS_ROOT "process_histogram.comp.spv"});
  etna::create_program(
    "postprocess_compute", {TONEMAPPING_MODULE_SHADERS_ROOT "postprocess.comp.spv"});
}

void TonemappingModule::setupPipelines()
{
  auto& pipelineManager = etna::get_context().getPipelineManager();

  calculateMinMaxPipeline = pipelineManager.createComputePipeline("min_max_calculation", {});
  histogramPipeline = pipelineManager.createComputePipeline("histogram_calculation", {});
  processHistogramPipeline = pipelineManager.createComputePipeline("histogram_processing", {});
  postprocessComputePipeline = pipelineManager.createComputePipeline("postprocess_compute", {});
}

void TonemappingModule::execute(
  vk::CommandBuffer cmd_buf, const etna::Image& render_target, glm::vec2 resolution)
{
  auto& currentHistogramBuffer = histogramBuffer->get();
  auto& currentDistributionBuffer = distributionBuffer->get();
  auto& currentHistogramInfo = histogramInfoBuffer->get();

  cmd_buf.fillBuffer(currentHistogramBuffer.get(), 0, vk::WholeSize, 0);
  cmd_buf.fillBuffer(currentDistributionBuffer.get(), 0, vk::WholeSize, 0);
  cmd_buf.fillBuffer(currentHistogramInfo.get(), 0, vk::WholeSize, 0);

  {
    std::array bufferBarriers = {
      vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead,
        .buffer = currentHistogramBuffer.get(),
        .size = vk::WholeSize},
      vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead,
        .buffer = currentDistributionBuffer.get(),
        .size = vk::WholeSize},
      vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead,
        .buffer = currentHistogramInfo.get(),
        .size = vk::WholeSize}};

    vk::DependencyInfo dependencyInfo = {
      .dependencyFlags = vk::DependencyFlagBits::eByRegion,
      .bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()),
      .pBufferMemoryBarriers = bufferBarriers.data()};

    cmd_buf.pipelineBarrier2(dependencyInfo);
  }

  {
    ETNA_PROFILE_GPU(cmd_buf, tonemapping);
    tonemappingShaderStart(
      cmd_buf,
      calculateMinMaxPipeline,
      "min_max_calculation",
      {etna::Binding{0, render_target.genBinding({}, vk::ImageLayout::eGeneral)},
       etna::Binding{1, currentHistogramInfo.genBinding()}},
      binsAmount,
      {(resolution.x + 31) / 32, (resolution.y + 31) / 32});

    {
      std::array bufferBarriers = {vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .buffer = currentHistogramInfo.get(),
        .size = vk::WholeSize}};

      vk::DependencyInfo dependencyInfo = {
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()),
        .pBufferMemoryBarriers = bufferBarriers.data()};

      cmd_buf.pipelineBarrier2(dependencyInfo);
    }

    tonemappingShaderStart(
      cmd_buf,
      histogramPipeline,
      "histogram_calculation",
      {etna::Binding{0, render_target.genBinding({}, vk::ImageLayout::eGeneral)},
       etna::Binding{1, currentHistogramBuffer.genBinding()},
       etna::Binding{2, currentHistogramInfo.genBinding()}},
      binsAmount,
      {(resolution.x + 31) / 32, (resolution.y + 31) / 32});

    {
      std::array bufferBarriers = {
        vk::BufferMemoryBarrier2{
          .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
          .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
          .buffer = currentHistogramBuffer.get(),
          .size = vk::WholeSize},
        vk::BufferMemoryBarrier2{
          .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
          .srcAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead,
          .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
          .dstAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead,
          .buffer = currentHistogramInfo.get(),
          .size = vk::WholeSize}};

      vk::DependencyInfo dependencyInfo = {
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()),
        .pBufferMemoryBarriers = bufferBarriers.data()};

      cmd_buf.pipelineBarrier2(dependencyInfo);
    }

    tonemappingShaderStart(
      cmd_buf,
      processHistogramPipeline,
      "histogram_processing",
      {etna::Binding{0, currentHistogramBuffer.genBinding()},
       etna::Binding{1, currentDistributionBuffer.genBinding()},
       etna::Binding{2, currentHistogramInfo.genBinding()}},
      binsAmount,
      {1, 1});

    {
      std::array bufferBarriers = {
        vk::BufferMemoryBarrier2{
          .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
          .srcAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead,
          .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
          .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
          .buffer = currentDistributionBuffer.get(),
          .size = vk::WholeSize},
        vk::BufferMemoryBarrier2{
          .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
          .srcAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead,
          .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
          .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
          .buffer = currentHistogramInfo.get(),
          .size = vk::WholeSize}};

      vk::DependencyInfo dependencyInfo = {
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()),
        .pBufferMemoryBarriers = bufferBarriers.data()};

      cmd_buf.pipelineBarrier2(dependencyInfo);
    }

    etna::set_state(
      cmd_buf,
      render_target.get(),
      vk::PipelineStageFlagBits2::eComputeShader,
      vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
      vk::ImageLayout::eGeneral,
      vk::ImageAspectFlagBits::eColor);

    etna::flush_barriers(cmd_buf);

    tonemappingShaderStart(
      cmd_buf,
      postprocessComputePipeline,
      "postprocess_compute",
      {etna::Binding{0, render_target.genBinding({}, vk::ImageLayout::eGeneral)},
       etna::Binding{1, currentDistributionBuffer.genBinding()},
       etna::Binding{2, currentHistogramInfo.genBinding()}},
      binsAmount,
      {(resolution.x + 31) / 32, (resolution.y + 31) / 32});
  }
}

void TonemappingModule::tonemappingShaderStart(
  vk::CommandBuffer cmd_buf,
  const etna::ComputePipeline& current_pipeline,
  std::string shader_program,
  std::vector<etna::Binding> bindings,
  std::optional<uint32_t> push_constant,
  glm::uvec2 group_count)
{
  ZoneScoped;
  auto vkPipelineLayout = current_pipeline.getVkPipelineLayout();
  cmd_buf.bindPipeline(vk::PipelineBindPoint::eCompute, current_pipeline.getVkPipeline());

  auto shaderProgramInfo = etna::get_shader_program(shader_program.c_str());

  auto set =
    etna::create_descriptor_set(shaderProgramInfo.getDescriptorLayoutId(0), cmd_buf, bindings);

  auto vkSet = set.getVkSet();

  cmd_buf.bindDescriptorSets(
    vk::PipelineBindPoint::eCompute, vkPipelineLayout, 0, 1, &vkSet, 0, nullptr);

  if (push_constant.has_value())
  {
    auto pushConst = push_constant.value();
    cmd_buf.pushConstants(
      vkPipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pushConst), &pushConst);
  }

  etna::flush_barriers(cmd_buf);

  cmd_buf.dispatch(group_count.x, group_count.y, 1);
}
