#pragma once

#include <glm/fwd.hpp>
#include <vector>
#include <optional>

#include <etna/Buffer.hpp>
#include <etna/GpuSharedResource.hpp>

#include "scene/Camera.hpp"
#include "DirectionalLight.h"


class ShadowCastingDirectionalLight
{
public:
  struct CreateInfo
  {
    DirectionalLight light;
    std::vector<float> planes; // including camera near and far planes
    float shadowMapSize;
  };

  // repack this struct later
  struct ShaderInfo
  {
    DirectionalLight light;
    uint32_t cascadesAmount;
    float nearPlanesBackwardOffset;
    float _padding[7] = {};
  };

  struct Settings
  {
    float zExpansion;
    float zNearOffset;
    float zFarOffset;
    float rotationMargin;
    float zFarExpandMul;
    float nearPlanesBackwardOffset;
  };

public:
  ShadowCastingDirectionalLight() = default;

  explicit ShadowCastingDirectionalLight(const CreateInfo& info);

  void update(const Camera& main_camera, float aspect_ratio);
  void drawGui();

  void prepareForDraw();

  const ShaderInfo& getInfo() const { return shaderInfo; }
  const etna::Buffer& getInfoBuffer() const { return infoBuffer->get(); }

private:
  std::array<glm::vec3, 8> getWorldSpaceFrustumCorners(const glm::mat4x4& proj_view);
  glm::vec3 getFrustumCenter(const std::array<glm::vec3, 8>& corners);
  glm::vec3 getShadowAnchor(const Camera& main_camera, std::size_t cascade_index);
  glm::mat4x4 getLightViewMatrix(const glm::vec3& camera_pos, bool world_space);
  glm::mat4x4 getLightProjMatrix(
    float left, float right, float bottom, float top, float z_near, float z_far);

private:
  ShaderInfo shaderInfo;
  Settings settings;
  std::vector<glm::mat4x4> projViewMatrices;
  std::vector<float> planes;
  float shadowMapSize;
  std::optional<etna::GpuSharedResource<etna::Buffer>> infoBuffer;

  Camera shadowCamera;
};
