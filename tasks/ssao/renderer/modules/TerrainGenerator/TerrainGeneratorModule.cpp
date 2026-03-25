#include "TerrainGeneratorModule.hpp"

#include <imgui.h>

#include <etna/Etna.hpp>
#include <etna/GlobalContext.hpp>
#include <etna/PipelineManager.hpp>
#include <etna/RenderTargetStates.hpp>


TerrainGeneratorModule::TerrainGeneratorModule()
  : params(
      {.numberOfSamples = 6,
       .seed = 1258.0f,
       .gradientRotation = 0.0f,
       .amplitudeDecay = 0.45f,
       .initialAmplitude = 0.5f,
       .lacunarity = 2.0f,
       .noiseRotation = 0.0f,
       .scale = 300.0f,
       .heightAmplifier = 165.0f,
       .heightOffset = -200.0f,
       .angleVariance = shader_vec2(0.0f, 0.0f),
       .frequencyVariance = shader_vec2(0.0f),
       .offset = shader_vec2(0.0f, 0.0f)})
  , maxNumberOfSamples(16)
{
}

TerrainGeneratorModule::TerrainGeneratorModule(const TerrainGeneratorModule::CreateInfo& info)
  : params(info.params)
  , maxNumberOfSamples(info.maxNumberOfSamples)
{
}

void TerrainGeneratorModule::allocateResources(const AllocationInfo& info)
{
  auto& ctx = etna::get_context();

  terrainMap = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = info.extent,
      .name = "terrain_map",
      .format = info.mapFormat,
      .imageUsage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage});
  terrainNormalMap = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = info.extent,
      .name = "terrain_normal_map",
      .format = vk::Format::eR16G16B16A16Snorm,
      .imageUsage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage});

  paramsBuffer = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = sizeof(TerrainGenerationParams),
      .bufferUsage = vk::BufferUsageFlagBits::eUniformBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO,
      .allocationCreate =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
      .name = "terrainGenerationParams"});

  oneShotCommands = ctx.createOneShotCmdMgr();

  terrainSampler = etna::Sampler(
    etna::Sampler::CreateInfo{
      .filter = vk::Filter::eLinear,
      .addressMode = vk::SamplerAddressMode::eRepeat,
      .name = "terrain_sampler"});
}

void TerrainGeneratorModule::loadShaders()
{
  etna::create_program(
    "terrain_generator", {TERRAIN_GENERATOR_MODULE_SHADERS_ROOT "generator.comp.spv"});
}

void TerrainGeneratorModule::setupPipelines()
{
  auto& pipelineManager = etna::get_context().getPipelineManager();

  terrainGenerationPipeline = pipelineManager.createComputePipeline("terrain_generator", {});
}

void TerrainGeneratorModule::execute()
{
  auto commandBuffer = oneShotCommands->start();

  ETNA_CHECK_VK_RESULT(commandBuffer.begin(vk::CommandBufferBeginInfo{}));
  {
    paramsBuffer.map();
    std::memcpy(paramsBuffer.data(), &params, sizeof(TerrainGenerationParams));
    paramsBuffer.unmap();

    etna::set_state(
      commandBuffer,
      terrainMap.get(),
      vk::PipelineStageFlagBits2::eComputeShader,
      vk::AccessFlagBits2::eShaderStorageWrite,
      vk::ImageLayout::eGeneral,
      vk::ImageAspectFlagBits::eColor);
    etna::set_state(
      commandBuffer,
      terrainNormalMap.get(),
      vk::PipelineStageFlagBits2::eComputeShader,
      vk::AccessFlagBits2::eShaderStorageWrite,
      vk::ImageLayout::eGeneral,
      vk::ImageAspectFlagBits::eColor);

    etna::flush_barriers(commandBuffer);

    auto extent = terrainMap.getExtent();
    glm::uvec2 glmExtent = {extent.width, extent.height};

    {
      auto shaderInfo = etna::get_shader_program("terrain_generator");
      auto set = etna::create_descriptor_set(
        shaderInfo.getDescriptorLayoutId(0),
        commandBuffer,
        {etna::Binding{0, terrainMap.genBinding({}, vk::ImageLayout::eGeneral)},
         etna::Binding{1, terrainNormalMap.genBinding({}, vk::ImageLayout::eGeneral)},
         etna::Binding{2, paramsBuffer.genBinding()}});

      auto vkSet = set.getVkSet();

      commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        terrainGenerationPipeline.getVkPipelineLayout(),
        0,
        1,
        &vkSet,
        0,
        nullptr);

      commandBuffer.bindPipeline(
        vk::PipelineBindPoint::eCompute, terrainGenerationPipeline.getVkPipeline());

      commandBuffer.dispatch((glmExtent.x + 31) / 32, (glmExtent.y + 31) / 32, 1);
    }
  }
  ETNA_CHECK_VK_RESULT(commandBuffer.end());

  oneShotCommands->submitAndWait(commandBuffer);
}

void TerrainGeneratorModule::drawGui()
{
  ImGui::Begin("Application Settings");

  static ImU32 numberOfSamplesMin = 1;
  static ImU32 numberOfSamplesMax = maxNumberOfSamples;

  if (ImGui::CollapsingHeader("Terrain Generation"))
  {
    ImGui::SeparatorText("Generation parameters");
    ImGui::SliderScalar(
      "Number of samples",
      ImGuiDataType_U32,
      &params.numberOfSamples,
      &numberOfSamplesMin,
      &numberOfSamplesMax,
      "%u");

    ImGui::InputScalar("Seed", ImGuiDataType_Float, &params.seed);

    ImGui::DragFloat("Gradient Rotation", &params.gradientRotation, 0.01f, 0.0f, 360.0f);
    ImGui::DragFloat("Amplitude Decay", &params.amplitudeDecay, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Initial Amplitude", &params.initialAmplitude, 0.1f, 0.0f, 5000.0f);
    ImGui::DragFloat("Lacunarity", &params.lacunarity, 0.01f, 0.0f, 10.0f);

    ImGui::DragFloatRange2(
      "Angle Variance", &params.angleVariance.x, &params.angleVariance.y, 0.01f, 0.0f, 360.0f);
    ImGui::DragFloat("Noise Rotation", &params.noiseRotation, 0.01f, 0.0f, 360.0f);

    float frequencyVariance[] = {params.frequencyVariance.x, params.frequencyVariance.y};
    ImGui::InputFloat2("Frequency Variance", frequencyVariance);
    params.frequencyVariance = shader_vec2(frequencyVariance[0], frequencyVariance[1]);

    float offset[] = {params.offset.x, params.offset.y};
    ImGui::InputFloat2("Offset", offset);
    params.offset = shader_vec2(offset[0], offset[1]);

    ImGui::DragFloat("Scale", &params.scale, 0.01f, 0.0f, 5000.0f);

    ImGui::SeparatorText("Height Adjustment");
    ImGui::SliderFloat("Height Amplifier", &params.heightAmplifier, 0, 10000, "%.3f");
    ImGui::InputFloat("Height Offset", &params.heightOffset);

    if (ImGui::Button("Regenerate Terrain"))
    {
      execute();
    }
  }

  ImGui::End();
}

std::vector<etna::Binding> TerrainGeneratorModule::getBindings(vk::ImageLayout layout) const
{
  return {
    etna::Binding{0, terrainMap.genBinding(terrainSampler.get(), layout)},
    etna::Binding{1, terrainNormalMap.genBinding(terrainSampler.get(), layout)}};
}

void TerrainGeneratorModule::setMapsState(
  vk::CommandBuffer com_buffer,
  vk::PipelineStageFlags2 pipeline_stage_flags,
  vk::AccessFlags2 access_flags,
  vk::ImageLayout layout,
  vk::ImageAspectFlags aspect_flags)
{
  etna::set_state(
    com_buffer, terrainMap.get(), pipeline_stage_flags, access_flags, layout, aspect_flags);
  etna::set_state(
    com_buffer, terrainNormalMap.get(), pipeline_stage_flags, access_flags, layout, aspect_flags);
}
