#include "LightModule.hpp"

#include <imgui.h>

#include <etna/PipelineManager.hpp>
#include <span>


LightModule::LightModule()
  : params(
      {.lightsAmount = 0,
       .directionalLightsAmount = 0,
       .shadowCastingDirLightsAmount = 0,
       .constant = 1.0f,
       .linear = 0.14f,
       .quadratic = 0.07f})
{
}

void LightModule::allocateResources()
{
  paramsBuffer = etna::get_context().createBuffer(
    etna::Buffer::CreateInfo{
      .size = sizeof(params),
      .bufferUsage = vk::BufferUsageFlagBits::eUniformBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO,
      .allocationCreate =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
      .name = "meshesParams"});

  oneShotCommands = etna::get_context().createOneShotCmdMgr();

  transferHelper = std::make_unique<etna::BlockingTransferHelper>(
    etna::BlockingTransferHelper::CreateInfo{.stagingSize = 4096 * 4096 * 6});
}

void LightModule::loadShaders()
{
  etna::create_program(
    "lights_displacement", {LIGHTS_MODULE_SHADERS_ROOT "displace_lights.comp.spv"});
}

void LightModule::setupPipelines()
{
  auto& pipelineManager = etna::get_context().getPipelineManager();

  lightDisplacementPipeline = pipelineManager.createComputePipeline("lights_displacement", {});
}

void LightModule::loadLights(
  const std::vector<Light>& new_lights,
  const std::vector<DirectionalLight>& new_directional_lights,
  ShadowCastingDirectionalLight::CreateInfo shadow_casting_dir_light_create_info)
{
  auto& ctx = etna::get_context();

  lights = new_lights;

  for (auto& light : lights)
  {
    float lightMax = std::max({light.color.r, light.color.g, light.color.b});
    light.radius = (-params.linear +
                    static_cast<float>(glm::sqrt(
                      params.linear * params.linear -
                      4 * params.quadratic * (params.constant - (256.0 / 5.0) * lightMax)))) /
      (2 * params.quadratic);
    // spdlog::info("radius - {}", light.radius);
  }

  directionalLights = new_directional_lights;

  shadowCastingDirLights.emplace(shadow_casting_dir_light_create_info);

  params.directionalLightsAmount = static_cast<uint32_t>(directionalLights.size());
  params.lightsAmount = static_cast<uint32_t>(lights.size());
  params.shadowCastingDirLightsAmount = 1;

  if (lights.empty())
  {
    lights.clear();
    lights.emplace_back(
      Light{.pos = {0, 0, 0}, .radius = 0.0f, .color = {0, 0, 0}, .intensity = 0.0f});
  }
  if (directionalLights.empty())
  {
    directionalLights.clear();
    directionalLights.emplace_back(
      DirectionalLight{.direction = {0, 0, 0}, .intensity = 0.0f, .color = {0, 0, 0}});
  }

  vk::DeviceSize directionalLightsSize = sizeof(DirectionalLight) * directionalLights.size();
  vk::DeviceSize lightsSize = sizeof(Light) * lights.size();

  lightsBuffer = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = lightsSize,
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = fmt::format("Lights")});
  directionalLightsBuffer = ctx.createBuffer(
    etna::Buffer::CreateInfo{
      .size = directionalLightsSize,
      .bufferUsage =
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
      .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      .name = fmt::format("DirectionalLights")});

  transferHelper->uploadBuffer(
    *oneShotCommands, directionalLightsBuffer, 0, std::as_bytes(std::span(directionalLights)));
  transferHelper->uploadBuffer(*oneShotCommands, lightsBuffer, 0, std::as_bytes(std::span(lights)));

  paramsBuffer.map();
  std::memcpy(paramsBuffer.data(), &params, sizeof(LightParams));
  paramsBuffer.unmap();
}

void LightModule::displaceLights()
{
  auto commandBuffer = oneShotCommands->start();

  ETNA_CHECK_VK_RESULT(commandBuffer.begin(vk::CommandBufferBeginInfo{}));
  {
    terrainSet->processBarriers(commandBuffer);
    etna::flush_barriers(commandBuffer);

    {
      std::array bufferBarriers = {vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .buffer = lightsBuffer.get(),
        .size = vk::WholeSize}};

      vk::DependencyInfo dependencyInfo = {
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()),
        .pBufferMemoryBarriers = bufferBarriers.data()};

      commandBuffer.pipelineBarrier2(dependencyInfo);
    }
    {
      auto shaderInfo = etna::get_shader_program("lights_displacement");

      auto set = etna::create_descriptor_set(
        shaderInfo.getDescriptorLayoutId(1),
        commandBuffer,
        {etna::Binding{0, paramsBuffer.genBinding()}, etna::Binding{1, lightsBuffer.genBinding()}});

      auto vkSet = set.getVkSet();

      commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        lightDisplacementPipeline.getVkPipelineLayout(),
        0,
        {terrainSet->getVkSet(), vkSet},
        {});

      commandBuffer.bindPipeline(
        vk::PipelineBindPoint::eCompute, lightDisplacementPipeline.getVkPipeline());

      commandBuffer.dispatch((static_cast<uint32_t>(lights.size()) + 31) / 32, 1, 1);
    }

    {
      std::array bufferBarriers = {vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .buffer = lightsBuffer.get(),
        .size = vk::WholeSize}};

      vk::DependencyInfo dependencyInfo = {
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()),
        .pBufferMemoryBarriers = bufferBarriers.data()};

      commandBuffer.pipelineBarrier2(dependencyInfo);
    }
  }
  ETNA_CHECK_VK_RESULT(commandBuffer.end());

  oneShotCommands->submitAndWait(commandBuffer);
}

void LightModule::update(const Camera& main_camera, float aspect_ratio)
{
  shadowCastingDirLights->update(main_camera, aspect_ratio);
}

void LightModule::prepareForDraw()
{
  shadowCastingDirLights->prepareForDraw();
}


void LightModule::drawGui()
{
  ImGui::Begin("Application Settings");

  if (ImGui::CollapsingHeader("Lights"))
  {
    static bool directionalLightsChanged = false;
    static bool lightsChanged = false;
    ImGuiColorEditFlags colorFlags =
      ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoAlpha;
    ImGui::SeparatorText("Directional Lights");
    for (uint32_t i = 0; i < directionalLights.size(); i++)
    {
      auto& currentLight = directionalLights[i];
      if (ImGui::TreeNode(&currentLight, "Light %d", i))
      {
        float direction[] = {
          currentLight.direction.x, currentLight.direction.y, currentLight.direction.z};
        float color[] = {
          currentLight.color.r,
          currentLight.color.g,
          currentLight.color.b,
        };
        float intensity = currentLight.intensity;
        directionalLightsChanged =
          directionalLightsChanged || ImGui::DragFloat3("Direction angles", direction);
        currentLight.direction = shader_vec3(direction[0], direction[1], direction[2]);
        directionalLightsChanged =
          directionalLightsChanged || ImGui::ColorEdit3("Color", color, colorFlags);
        currentLight.color = shader_vec3(color[0], color[1], color[2]);
        directionalLightsChanged =
          directionalLightsChanged || ImGui::DragFloat("Intensity", &intensity);
        currentLight.intensity = intensity;

        ImGui::TreePop();
      }
    }
    ImGui::SeparatorText("Point Lights");
    for (uint32_t i = 0; i < lights.size(); i++)
    {
      auto& currentLight = lights[i];
      if (ImGui::TreeNode(&currentLight, "Light %d", i))
      {

        float position[] = {currentLight.pos.x, currentLight.pos.y, currentLight.pos.z};
        float color[] = {
          currentLight.color.r,
          currentLight.color.g,
          currentLight.color.b,
        };
        float radius = currentLight.radius;
        float intensity = currentLight.intensity;
        lightsChanged = lightsChanged || ImGui::DragFloat3("Position", position);
        currentLight.pos = shader_vec3(position[0], position[1], position[2]);
        lightsChanged = lightsChanged || ImGui::ColorEdit3("Color", color, colorFlags);
        currentLight.color = shader_vec3(color[0], color[1], color[2]);
        lightsChanged = lightsChanged || ImGui::DragFloat("Radius", &radius);
        currentLight.radius = radius;
        lightsChanged = lightsChanged || ImGui::DragFloat("Intensity", &intensity);
        currentLight.intensity = intensity;

        ImGui::TreePop();
      }
    }

    shadowCastingDirLights->drawGui();

    if (directionalLightsChanged)
    {
      ETNA_CHECK_VK_RESULT(etna::get_context().getDevice().waitIdle());
      transferHelper->uploadBuffer(
        *oneShotCommands, directionalLightsBuffer, 0, std::as_bytes(std::span(directionalLights)));
      directionalLightsChanged = false;
    }
    if (lightsChanged)
    {
      ETNA_CHECK_VK_RESULT(etna::get_context().getDevice().waitIdle());
      transferHelper->uploadBuffer(
        *oneShotCommands, lightsBuffer, 0, std::as_bytes(std::span(lights)));
      displaceLights();
      lightsChanged = false;
    }
  }

  ImGui::End();
}

void LightModule::loadMaps(const std::vector<etna::Binding>& terrain_bindings)
{
  auto shaderInfo = etna::get_shader_program("lights_displacement");
  terrainSet =
    std::make_unique<etna::PersistentDescriptorSet>(etna::create_persistent_descriptor_set(
      shaderInfo.getDescriptorLayoutId(0), terrain_bindings, true));
}
