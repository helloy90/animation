#include "WaterGeneratorModule.hpp"

#include <glm/common.hpp>
#include <glm/ext/scalar_constants.hpp>
#include <imgui.h>

#include <glm/gtc/integer.hpp>
#include <glm/exponential.hpp>

#include <etna/Etna.hpp>
#include <etna/Profiling.hpp>
#include <etna/PipelineManager.hpp>


static float jonswapAlpha(float gravity, float wind_action_length, float wind_speed)
{
  return 0.076f * glm::pow(gravity * wind_action_length / wind_speed / wind_speed, -0.22f);
}

static float jonswapPeakFrequency(float gravity, float wind_action_length, float wind_speed)
{
  return 22.0f * glm::pow(wind_speed * wind_action_length / gravity / gravity, -0.33f);
}

WaterGeneratorModule::WaterGeneratorModule()
  : patchSizes({256})
  , generalParams({
      .gravity = shader_float(9.81f),
      .depth = shader_float(20),
      .lowCutoff = shader_float(0.0001f),
      .highCutoff = shader_float(9000.0f),
      .seed = shader_uint(38),
    })
  , displayParamsVector({
      {.scale = shader_float(0.87),
       .windSpeed = shader_float(1.2),
       .windDirection = shader_float(22),
       .windActionLength = shader_float(100000),
       .spreadBlend = shader_float(0.642),
       .swell = shader_float(1),
       .peakEnhancement = shader_float(1),
       .shortWavesFade = shader_float(0.3)},
      {.scale = shader_float(0.07),
       .windSpeed = shader_float(2),
       .windDirection = shader_float(59),
       .windActionLength = shader_float(1000),
       .spreadBlend = shader_float(0),
       .swell = shader_float(1),
       .peakEnhancement = shader_float(1),
       .shortWavesFade = shader_float(0.01)},
    })
  , updateParams(
      {.foamDecayRate = shader_float(0.5f),
       .foamBias = shader_float(0.85f),
       .foamThreshold = shader_float(0.0f),
       .foamMultiplier = shader_float(0.1f),
       .wavePeriod = shader_float(200)})
{
}

void WaterGeneratorModule::allocateResources(const AllocationInfo& info)
{
  auto& ctx = etna::get_context();

  vk::Extent3D textureExtent = {info.texturesExtent, info.texturesExtent, 1};

  uint32_t logExtent = static_cast<uint32_t>(glm::log2(info.texturesExtent));

  inverseFFTInfo = {.size = info.texturesExtent, .logSize = logExtent, .texturesAmount = 2};

  paramsVector.clear();
  paramsVector.reserve(displayParamsVector.size());

  for (uint32_t i = 0; i < displayParamsVector.size(); i++)
  {
    paramsVector.emplace_back(recalculateParams(displayParamsVector[i]));
  }

  initialSpectrumTexture = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = textureExtent,
      .name = "initial_spectrum_tex",
      .format = info.texturesFormat,
      .imageUsage = vk::ImageUsageFlagBits::eStorage});

  updatedSpectrumSlopeTexture = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = textureExtent,
      .name = "updated_spectrum_slope_tex",
      .format = info.texturesFormat,
      .imageUsage = vk::ImageUsageFlagBits::eStorage});
  updatedSpectrumDisplacementTexture = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = textureExtent,
      .name = "updated_spectrum_displacement_tex",
      .format = info.texturesFormat,
      .imageUsage = vk::ImageUsageFlagBits::eStorage});

  heightMap = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = textureExtent,
      .name = "water_height_map",
      .format = info.texturesFormat,
      .imageUsage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage});
  normalMap = ctx.createImage(
    etna::Image::CreateInfo{
      .extent = textureExtent,
      .name = "water_normal_map",
      .format = info.texturesFormat,
      .imageUsage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage});

  paramsBuffer = etna::get_context().createBuffer(
    etna::Buffer::CreateInfo{
      .size = sizeof(SpectrumGenerationParams) * paramsVector.size(),
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "spectrumGenerationParams"});
  patchSizesBuffer = etna::get_context().createBuffer(
    etna::Buffer::CreateInfo{
      .size = sizeof(uint32_t) * patchSizes.size(),
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = "WaterPatchSizes"});
  generalParamsBuffer = etna::get_context().createBuffer(
    etna::Buffer::CreateInfo{
      .size = sizeof(GeneralSpectrumParams),
      .bufferUsage = vk::BufferUsageFlagBits::eUniformBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO,
      .allocationCreate =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
      .name = "GeneralSpectrumParams"});
  updateParamsBuffer = etna::get_context().createBuffer(
    etna::Buffer::CreateInfo{
      .size = sizeof(SpectrumUpdateParams),
      .bufferUsage = vk::BufferUsageFlagBits::eUniformBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO,
      .allocationCreate =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
      .name = "spectrumUpdateParams"});

  infoBuffer = etna::get_context().createBuffer(
    etna::Buffer::CreateInfo{
      .size = sizeof(InverseFFTInfo),
      .bufferUsage = vk::BufferUsageFlagBits::eUniformBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO,
      .allocationCreate =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
      .name = "inverseFFTInfo"});

  oneShotCommands = ctx.createOneShotCmdMgr();
  transferHelper =
    std::make_unique<etna::BlockingTransferHelper>(etna::BlockingTransferHelper::CreateInfo{
      .stagingSize = sizeof(SpectrumGenerationParams) * paramsVector.size()});

  textureSampler = etna::Sampler(
    etna::Sampler::CreateInfo{
      .filter = vk::Filter::eLinear,
      .addressMode = vk::SamplerAddressMode::eRepeat,
      .name = "spectrum_sampler"});

  ETNA_VERIFYF(patchSizes.size() * 2 == paramsVector.size(), "Incorrect amount of patches");

  transferHelper->uploadBuffer(
    *oneShotCommands, paramsBuffer, 0, std::as_bytes(std::span(paramsVector)));
  transferHelper->uploadBuffer(
    *oneShotCommands, patchSizesBuffer, 0, std::as_bytes(std::span(patchSizes)));
}

void WaterGeneratorModule::loadShaders()
{
  etna::create_program(
    "water_spectrum_generation",
    {WATER_GENERATOR_MODULE_SHADERS_ROOT "generate_initial_spectrum.comp.spv"});

  etna::create_program(
    "water_spectrum_progression",
    {WATER_GENERATOR_MODULE_SHADERS_ROOT "update_spectrum_for_fft.comp.spv"});
  etna::create_program(
    "water_horizontal_inverse_fft",
    {WATER_GENERATOR_MODULE_SHADERS_ROOT "horizontal_inverse_fft.comp.spv"});
  etna::create_program(
    "water_vertical_inverse_fft",
    {WATER_GENERATOR_MODULE_SHADERS_ROOT "vertical_inverse_fft.comp.spv"});

  etna::create_program(
    "water_assembler", {WATER_GENERATOR_MODULE_SHADERS_ROOT "assemble.comp.spv"});
}

void WaterGeneratorModule::setupPipelines()
{
  initialSpectrumGenerationPipeline =
    etna::get_context().getPipelineManager().createComputePipeline("water_spectrum_generation", {});

  spectrumProgressionPipeline = etna::get_context().getPipelineManager().createComputePipeline(
    "water_spectrum_progression", {});

  horizontalInverseFFTPipeline = etna::get_context().getPipelineManager().createComputePipeline(
    "water_horizontal_inverse_fft", {});
  verticalInverseFFTPipeline = etna::get_context().getPipelineManager().createComputePipeline(
    "water_vertical_inverse_fft", {});

  assemblerPipeline =
    etna::get_context().getPipelineManager().createComputePipeline("water_assembler", {});
}

void WaterGeneratorModule::executeStart()
{
  auto commandBuffer = oneShotCommands->start();

  ETNA_CHECK_VK_RESULT(commandBuffer.begin(vk::CommandBufferBeginInfo{}));
  {
    generalParamsBuffer.map();
    std::memcpy(generalParamsBuffer.data(), &generalParams, sizeof(GeneralSpectrumParams));
    generalParamsBuffer.unmap();

    updateParamsBuffer.map();
    std::memcpy(updateParamsBuffer.data(), &updateParams, sizeof(SpectrumUpdateParams));
    updateParamsBuffer.unmap();

    infoBuffer.map();
    std::memcpy(infoBuffer.data(), &inverseFFTInfo, sizeof(InverseFFTInfo));
    infoBuffer.unmap();

    etna::set_state(
      commandBuffer,
      initialSpectrumTexture.get(),
      vk::PipelineStageFlagBits2::eComputeShader,
      vk::AccessFlagBits2::eShaderStorageWrite,
      vk::ImageLayout::eGeneral,
      vk::ImageAspectFlagBits::eColor);

    etna::flush_barriers(commandBuffer);

    {
      ETNA_PROFILE_GPU(commandBuffer, generateInitialSpectrum)
      commandBuffer.bindPipeline(
        vk::PipelineBindPoint::eCompute, initialSpectrumGenerationPipeline.getVkPipeline());
      generateInitialSpectrum(
        commandBuffer, initialSpectrumGenerationPipeline.getVkPipelineLayout());
    }

    std::vector<etna::Binding> bindings = {
      etna::Binding{
        0,
        updatedSpectrumSlopeTexture.genBinding(textureSampler.get(), vk::ImageLayout::eGeneral),
        0},
      etna::Binding{
        0,
        updatedSpectrumDisplacementTexture.genBinding(
          textureSampler.get(), vk::ImageLayout::eGeneral),
        1},
    };

    auto shaderInfoHorizontal = etna::get_shader_program("water_horizontal_inverse_fft");
    horizontalInverseFFTDescriptorSet = etna::create_persistent_descriptor_set(
      shaderInfoHorizontal.getDescriptorLayoutId(0), bindings, true);
    horizontalInverseFFTDescriptorSet->processBarriers(commandBuffer);

    auto shaderInfoVertical = etna::get_shader_program("water_vertical_inverse_fft");
    verticalIInverseFFTDescriptorSet = etna::create_persistent_descriptor_set(
      shaderInfoVertical.getDescriptorLayoutId(0), bindings, true);
    verticalIInverseFFTDescriptorSet->processBarriers(commandBuffer);
  }
  ETNA_CHECK_VK_RESULT(commandBuffer.end());

  oneShotCommands->submitAndWait(commandBuffer);
}

void WaterGeneratorModule::executeProgress(vk::CommandBuffer cmd_buf, float time)
{
  ETNA_PROFILE_GPU(cmd_buf, waterProgress);

  etna::set_state(
    cmd_buf,
    updatedSpectrumSlopeTexture.get(),
    vk::PipelineStageFlagBits2::eComputeShader,
    vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead,
    vk::ImageLayout::eGeneral,
    vk::ImageAspectFlagBits::eColor);
  etna::set_state(
    cmd_buf,
    updatedSpectrumDisplacementTexture.get(),
    vk::PipelineStageFlagBits2::eComputeShader,
    vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead,
    vk::ImageLayout::eGeneral,
    vk::ImageAspectFlagBits::eColor);

  etna::flush_barriers(cmd_buf);

  {
    ETNA_PROFILE_GPU(cmd_buf, updateSpectrumForFFT);
    cmd_buf.bindPipeline(
      vk::PipelineBindPoint::eCompute, spectrumProgressionPipeline.getVkPipeline());
    updateSpectrumForFFT(cmd_buf, spectrumProgressionPipeline.getVkPipelineLayout(), time);
  }

  {
    ETNA_PROFILE_GPU(cmd_buf, inverseFFT);
    inverseFFT(cmd_buf);
  }

  etna::set_state(
    cmd_buf,
    heightMap.get(),
    vk::PipelineStageFlagBits2::eComputeShader,
    vk::AccessFlagBits2::eShaderStorageWrite,
    vk::ImageLayout::eGeneral,
    vk::ImageAspectFlagBits::eColor);
  etna::set_state(
    cmd_buf,
    normalMap.get(),
    vk::PipelineStageFlagBits2::eComputeShader,
    vk::AccessFlagBits2::eShaderStorageWrite,
    vk::ImageLayout::eGeneral,
    vk::ImageAspectFlagBits::eColor);

  etna::flush_barriers(cmd_buf);

  {
    ETNA_PROFILE_GPU(cmd_buf, assembleMaps);
    cmd_buf.bindPipeline(vk::PipelineBindPoint::eCompute, assemblerPipeline.getVkPipeline());
    assembleMaps(cmd_buf, assemblerPipeline.getVkPipelineLayout());
  }
}

void WaterGeneratorModule::drawGui()
{
  ImGui::Begin("Application Settings");

  static bool paramsChanged = false;
  static bool generalParamsChanged = false;
  static bool updateParamsChanged = false;

  if (ImGui::CollapsingHeader("Water Generator"))
  {
    ImGui::Text("Water spectrum parameters (regeneration needed for these to take effect)");
    for (uint32_t i = 0; i < displayParamsVector.size(); i++)
    {
      auto& displayParams = displayParamsVector[i];
      if (ImGui::TreeNode(&displayParams, "Settings %d", i))
      {
        float scale = displayParams.scale;
        float windSpeed = displayParams.windSpeed;
        float windDirection = displayParams.windDirection;
        float windActionLength = displayParams.windActionLength;
        float spreadBlend = displayParams.spreadBlend;
        float swell = displayParams.swell;
        float peakEnhancement = displayParams.peakEnhancement;
        float shortWavesFade = displayParams.shortWavesFade;
        int32_t patchSize = patchSizes[i / 2];

        paramsChanged =
          paramsChanged || ImGui::DragFloat("Water scale", &scale, 0.01f, 0.0f, 5000.0f);
        displayParams.scale = scale;
        paramsChanged =
          paramsChanged || ImGui::DragFloat("Wind speed", &windSpeed, 0.1f, 0.0f, 5000.0f);
        displayParams.windSpeed = windSpeed;
        paramsChanged =
          paramsChanged || ImGui::DragFloat("Wind direction", &windDirection, 0.01f, 0.0f, 360.0f);
        displayParams.windDirection = windDirection;
        paramsChanged = paramsChanged ||
          ImGui::DragFloat("Wind action length", &windActionLength, 1.0f, 0.0f, 10000000.0f);
        displayParams.windActionLength = windActionLength;
        paramsChanged =
          paramsChanged || ImGui::DragFloat("Spread blend", &spreadBlend, 0.01f, 0.0f, 1.0f);
        displayParams.spreadBlend = spreadBlend;
        paramsChanged = paramsChanged || ImGui::DragFloat("Water swell", &swell, 0.01f, 0.0f, 1.0f);
        displayParams.swell = swell;
        paramsChanged = paramsChanged ||
          ImGui::DragFloat("Water peak enchancement", &peakEnhancement, 0.1f, 0.0f, 5000.0f);
        displayParams.peakEnhancement = peakEnhancement;
        paramsChanged = paramsChanged ||
          ImGui::DragFloat("Water short waves fade", &shortWavesFade, 0.1f, 0.0f, 5000.0f);
        displayParams.shortWavesFade = shortWavesFade;
        paramsChanged = paramsChanged || ImGui::DragInt("Patch Size", &patchSize, 1, 0, 4096);
        patchSizes[i / 2] = patchSize;

        ImGui::TreePop();
      }
    }

    float foamDecayRate = updateParams.foamDecayRate;
    float foamBias = updateParams.foamBias;
    float foamThreshold = updateParams.foamThreshold;
    float foamMultiplier = updateParams.foamMultiplier;
    float wavePeriod = updateParams.wavePeriod;

    float gravity = generalParams.gravity;
    float depth = generalParams.depth;
    float lowCutoff = generalParams.lowCutoff;
    float highCutoff = generalParams.highCutoff;
    int32_t seed = generalParams.seed;

    ImGui::SeparatorText("Water update parameters");

    updateParamsChanged = updateParamsChanged ||
      ImGui::DragFloat("Foam decay rate", &foamDecayRate, 0.01f, 0.0f, 100.0f);
    updateParams.foamDecayRate = foamDecayRate;
    updateParamsChanged =
      updateParamsChanged || ImGui::DragFloat("Foam bias", &foamBias, 0.01f, -1.0f, 1.0f);
    updateParams.foamBias = foamBias;
    updateParamsChanged =
      updateParamsChanged || ImGui::DragFloat("Foam threshold", &foamThreshold, 0.01f, -5.0f, 5.0f);
    updateParams.foamThreshold = foamThreshold;
    updateParamsChanged = updateParamsChanged ||
      ImGui::DragFloat("Foam multiplier", &foamMultiplier, 0.01f, 0.0f, 100.0f);
    updateParams.foamMultiplier = foamMultiplier;
    updateParamsChanged =
      updateParamsChanged || ImGui::DragFloat("Wave period", &wavePeriod, 1.0f, 0.00001f, 5000.0f);
    updateParams.wavePeriod = wavePeriod;

    ImGui::SeparatorText(
      "General spectrum parameters (regeneration needed for these to take effect)");

    generalParamsChanged =
      generalParamsChanged || ImGui::DragFloat("Gravity", &gravity, 0.1f, 0.0f, 5000.0f);
    generalParams.gravity = gravity;
    generalParamsChanged =
      generalParamsChanged || ImGui::DragFloat("Depth", &depth, 0.01f, 0.0f, 200.0f);
    generalParams.depth = depth;
    generalParamsChanged =
      generalParamsChanged || ImGui::DragFloat("Low cutoff", &lowCutoff, 0.01f, 0.0f, 200.0f);
    generalParams.lowCutoff = lowCutoff;
    generalParamsChanged =
      generalParamsChanged || ImGui::DragFloat("High cutoff", &highCutoff, 0.1f, 200.0f, 10000.0f);
    generalParams.highCutoff = highCutoff;
    generalParamsChanged = generalParamsChanged || ImGui::DragInt("Seed", &seed, 1.0f, 0, 5000000);
    generalParams.seed = seed;


    if (ImGui::Button("Regenerate water"))
    {
      executeStart();
    }
  }

  if (paramsChanged)
  {
    ETNA_CHECK_VK_RESULT(etna::get_context().getDevice().waitIdle());
    for (uint32_t i = 0; i < displayParamsVector.size(); i++)
    {
      paramsVector[i] = recalculateParams(displayParamsVector[i]);
    }
    transferHelper->uploadBuffer(
      *oneShotCommands, paramsBuffer, 0, std::as_bytes(std::span(paramsVector)));
    transferHelper->uploadBuffer(
      *oneShotCommands, patchSizesBuffer, 0, std::as_bytes(std::span(patchSizes)));
    paramsChanged = false;
  }

  if (updateParamsChanged)
  {
    updateParamsBuffer.map();
    std::memcpy(updateParamsBuffer.data(), &updateParams, sizeof(SpectrumUpdateParams));
    updateParamsBuffer.unmap();
    updateParamsChanged = false;
  }

  if (generalParamsChanged)
  {
    generalParamsBuffer.map();
    std::memcpy(generalParamsBuffer.data(), &generalParams, sizeof(GeneralSpectrumParams));
    generalParamsBuffer.unmap();
    generalParamsChanged = false;
  }

  ImGui::End();
}

SpectrumGenerationParams WaterGeneratorModule::recalculateParams(
  const DisplaySpectrumParams& display_params)
{
  return SpectrumGenerationParams{
    .scale = display_params.scale,
    .angle = display_params.windDirection / 180.0f * glm::pi<float>(),
    .spreadBlend = display_params.spreadBlend,
    .swell = glm::clamp(display_params.swell, 0.01f, 1.0f),
    .jonswapAlpha = jonswapAlpha(
      generalParams.gravity, display_params.windActionLength, display_params.windSpeed),
    .peakFrequency = jonswapPeakFrequency(
      generalParams.gravity, display_params.windActionLength, display_params.windSpeed),
    .peakEnhancement = display_params.peakEnhancement,
    .shortWavesFade = display_params.shortWavesFade,
  };
}

void WaterGeneratorModule::generateInitialSpectrum(
  vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout)
{
  auto extent = initialSpectrumTexture.getExtent();
  auto shaderInfo = etna::get_shader_program("water_spectrum_generation");

  auto set = etna::create_descriptor_set(
    shaderInfo.getDescriptorLayoutId(0),
    cmd_buf,
    {
      etna::Binding{
        0, initialSpectrumTexture.genBinding(textureSampler.get(), vk::ImageLayout::eGeneral)},
      etna::Binding{1, paramsBuffer.genBinding()},
      etna::Binding{2, generalParamsBuffer.genBinding()},
      etna::Binding{3, infoBuffer.genBinding()},
      etna::Binding{4, patchSizesBuffer.genBinding()},
    });

  auto vkSet = set.getVkSet();

  cmd_buf.bindDescriptorSets(
    vk::PipelineBindPoint::eCompute, pipeline_layout, 0, 1, &vkSet, 0, nullptr);

  cmd_buf.dispatch((extent.width + 31) / 32, (extent.height + 31) / 32, 1);
}

void WaterGeneratorModule::updateSpectrumForFFT(
  vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout, float time)
{
  auto extent = initialSpectrumTexture.getExtent();
  auto shaderInfo = etna::get_shader_program("water_spectrum_progression");

  auto set = etna::create_descriptor_set(
    shaderInfo.getDescriptorLayoutId(0),
    cmd_buf,
    {
      etna::Binding{
        0, initialSpectrumTexture.genBinding(textureSampler.get(), vk::ImageLayout::eGeneral)},
      etna::Binding{
        1, updatedSpectrumSlopeTexture.genBinding(textureSampler.get(), vk::ImageLayout::eGeneral)},
      etna::Binding{
        2,
        updatedSpectrumDisplacementTexture.genBinding(
          textureSampler.get(), vk::ImageLayout::eGeneral)},
      etna::Binding{3, generalParamsBuffer.genBinding()},
      etna::Binding{4, paramsBuffer.genBinding()},
      etna::Binding{5, updateParamsBuffer.genBinding()},
      etna::Binding{6, infoBuffer.genBinding()},
      etna::Binding{7, patchSizesBuffer.genBinding()},
    });

  auto vkSet = set.getVkSet();

  cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_layout, 0, {vkSet}, {});

  cmd_buf.pushConstants<float>(pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, {time});

  cmd_buf.dispatch((extent.width + 31) / 32, (extent.height + 31) / 32, 1);
}

void WaterGeneratorModule::inverseFFT(vk::CommandBuffer cmd_buf)
{
  {
    ETNA_PROFILE_GPU(cmd_buf, inverseFFTHorizontalStep);
    cmd_buf.bindPipeline(
      vk::PipelineBindPoint::eCompute, horizontalInverseFFTPipeline.getVkPipeline());
    executeInverseFFT(
      cmd_buf,
      horizontalInverseFFTPipeline.getVkPipelineLayout(),
      "water_horizontal_inverse_fft",
      *horizontalInverseFFTDescriptorSet,
      {1, inverseFFTInfo.size, 1});
  }

  {
    ETNA_PROFILE_GPU(cmd_buf, inverseFFTVerticalStep);
    cmd_buf.bindPipeline(
      vk::PipelineBindPoint::eCompute, verticalInverseFFTPipeline.getVkPipeline());
    executeInverseFFT(
      cmd_buf,
      verticalInverseFFTPipeline.getVkPipelineLayout(),
      "water_vertical_inverse_fft",
      *verticalIInverseFFTDescriptorSet,
      {1, inverseFFTInfo.size, 1});
  }
}

void WaterGeneratorModule::executeInverseFFT(
  vk::CommandBuffer cmd_buf,
  vk::PipelineLayout pipeline_layout,
  const char* shader_program,
  etna::PersistentDescriptorSet persistent_set,
  vk::Extent3D extent)
{
  auto shaderInfo = etna::get_shader_program(shader_program);

  auto set = etna::create_descriptor_set(
    shaderInfo.getDescriptorLayoutId(1), cmd_buf, {etna::Binding{0, infoBuffer.genBinding()}});

  auto vkSet = set.getVkSet();


  cmd_buf.bindDescriptorSets(
    vk::PipelineBindPoint::eCompute, pipeline_layout, 0, {persistent_set.getVkSet(), vkSet}, {});

  cmd_buf.dispatch(extent.width, extent.height, 1);
}

void WaterGeneratorModule::assembleMaps(
  vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout)
{
  auto extent = initialSpectrumTexture.getExtent();
  auto shaderInfo = etna::get_shader_program("water_assembler");

  auto set = etna::create_descriptor_set(
    shaderInfo.getDescriptorLayoutId(0),
    cmd_buf,
    {
      etna::Binding{
        0, updatedSpectrumSlopeTexture.genBinding(textureSampler.get(), vk::ImageLayout::eGeneral)},
      etna::Binding{
        1,
        updatedSpectrumDisplacementTexture.genBinding(
          textureSampler.get(), vk::ImageLayout::eGeneral)},
      etna::Binding{2, heightMap.genBinding(textureSampler.get(), vk::ImageLayout::eGeneral)},
      etna::Binding{3, normalMap.genBinding(textureSampler.get(), vk::ImageLayout::eGeneral)},
      etna::Binding{4, updateParamsBuffer.genBinding()},
    });

  auto vkSet = set.getVkSet();

  cmd_buf.bindDescriptorSets(
    vk::PipelineBindPoint::eCompute, pipeline_layout, 0, 1, &vkSet, 0, nullptr);

  cmd_buf.dispatch((extent.width + 31) / 32, (extent.height + 31) / 32, 1);
}
