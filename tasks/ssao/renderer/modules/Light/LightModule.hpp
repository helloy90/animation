#pragma once

#include <etna/ComputePipeline.hpp>
#include <etna/Buffer.hpp>
#include <etna/Sampler.hpp>
#include <etna/BlockingTransferHelper.hpp>

#include "DirectionalLight.h"
#include "Light.h"
#include "ShadowCastingDirectionalLight.hpp"

#include "shaders/LightParams.h"


class LightModule
{
public:
  LightModule();

  void allocateResources();
  void loadShaders();
  void setupPipelines();

  void loadLights(
    const std::vector<Light>& new_lights,
    const std::vector<DirectionalLight>& new_directional_lights,
    ShadowCastingDirectionalLight::CreateInfo shadow_casting_dir_light_create_info);
  void displaceLights();

  void update(const Camera& main_camera, float aspect_ratio);

  void prepareForDraw();

  void drawGui();

  void loadMaps(const std::vector<etna::Binding>& terrain_bindings);

  const etna::Buffer& getLightParamsBuffer() const { return paramsBuffer; }
  const etna::Buffer& getPointLightsBuffer() const { return lightsBuffer; }
  const etna::Buffer& getDirectionalLightsBuffer() const { return directionalLightsBuffer; }
  const etna::Buffer& getShadowCastingDirLightInfoBuffer() const
  {
    return shadowCastingDirLights->getInfoBuffer();
  }

  etna::BufferBinding getShadowCastingDirLightMatrixBinding(uint32_t cascade_index) const
  {
    return getShadowCastingDirLightInfoBuffer().genBinding(
      sizeof(ShadowCastingDirectionalLight::ShaderInfo) + sizeof(glm::mat4x4) * cascade_index,
      sizeof(glm::mat4x4));
  }

private:
  LightParams params;
  etna::Buffer paramsBuffer;

  std::vector<Light> lights;
  std::vector<DirectionalLight> directionalLights;

  // For now only one shadow casting light
  std::optional<ShadowCastingDirectionalLight> shadowCastingDirLights;

  etna::Buffer lightsBuffer;
  etna::Buffer directionalLightsBuffer;

  etna::ComputePipeline lightDisplacementPipeline;

  std::unique_ptr<etna::OneShotCmdMgr> oneShotCommands;
  std::unique_ptr<etna::BlockingTransferHelper> transferHelper;

  std::unique_ptr<etna::PersistentDescriptorSet> terrainSet;
};
