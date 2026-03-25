#include "ShadowCastingDirectionalLight.hpp"

#include <imgui.h>
#include <tracy/Tracy.hpp>

#include <etna/GlobalContext.hpp>


ShadowCastingDirectionalLight::ShadowCastingDirectionalLight(const CreateInfo& info)
  : shaderInfo(
      {.light = info.light,
       .cascadesAmount = static_cast<uint32_t>(info.planes.size() - 1),
       .nearPlanesBackwardOffset = 12.0f})
  , settings(
      {.zExpansion = 100.0f,
       .zNearOffset = 0.0f,
       .zFarOffset = 0.0f,
       .rotationMargin = 0.5f,
       .zFarExpandMul = 1.0f,
       .nearPlanesBackwardOffset = 12.0f})
  , projViewMatrices({})
  , planes(info.planes)
  , shadowMapSize(info.shadowMapSize)
{
  projViewMatrices.resize(shaderInfo.cascadesAmount);

  vk::DeviceSize infoBufferSize = sizeof(ShadowCastingDirectionalLight::ShaderInfo) +
    shaderInfo.cascadesAmount * sizeof(glm::mat4x4) + planes.size() * sizeof(float);

  auto& ctx = etna::get_context();

  infoBuffer.emplace(ctx.getMainWorkCount(), [infoBufferSize, &ctx](std::size_t i) {
    return ctx.createBuffer(
      etna::Buffer::CreateInfo{
        .size = infoBufferSize,
        .bufferUsage = vk::BufferUsageFlagBits::eStorageBuffer,
        .memoryUsage = VMA_MEMORY_USAGE_AUTO,
        .allocationCreate =
          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .name = fmt::format("ShadowCastingDirLightInfo_{}", i)});
  });
}

void ShadowCastingDirectionalLight::update(const Camera& main_camera, float aspect_ratio)
{
  ZoneScoped;

  Camera frustumCamera = main_camera;

  for (std::size_t cascade = 0; cascade < shaderInfo.cascadesAmount; cascade++)
  {
    frustumCamera.zNear = planes[cascade] -
      (cascade == 0
         ? 0.0f
         : glm::min(
             settings.nearPlanesBackwardOffset, (planes[cascade] - planes[cascade - 1]) * 0.75f));
    frustumCamera.zFar = planes[cascade + 1];

    float zFarExpansion = glm::max(
      2.0f * settings.zExpansion,
      (glm::abs(shaderInfo.light.direction.y) < 0.000001f)
        ? 0.0f
        : settings.zExpansion / (-shaderInfo.light.direction.y));

    float zNearExpansion = zFarExpansion + glm::max(settings.zNearOffset, 0.0f);
    zFarExpansion += glm::max(settings.zFarOffset, 0.0f);

    glm::mat4x4 proj = frustumCamera.projTm(aspect_ratio);

    std::array corners = getWorldSpaceFrustumCorners(proj * frustumCamera.viewTm());

    glm::mat4x4 lightView = getLightViewMatrix(frustumCamera.position, false);
    glm::mat3x3 lightView3 = glm::mat3x3(lightView);

    std::array cornersInLS = corners;
    for (auto& corner : cornersInLS)
    {
      corner = lightView3 * corner;
    }

    glm::vec3 bbMin = cornersInLS[0];
    glm::vec3 bbMax = cornersInLS[0];
    for (uint32_t i = 1; i < 8; i++)
    {
      bbMin = glm::min(bbMin, cornersInLS[i]);
      bbMax = glm::max(bbMax, cornersInLS[i]);
    }

    glm::vec3 bbWidth = bbMax - bbMin;

    const int borderPixels = 4;
    float texelWidth = bbWidth.x / shadowMapSize;
    float texelHeight = bbWidth.y / shadowMapSize;

    bbMin.x -= borderPixels * texelWidth;
    bbMin.y -= borderPixels * texelHeight;

    bbMax.x += borderPixels * texelWidth;
    bbMax.y += borderPixels * texelHeight;

    float step = settings.rotationMargin * frustumCamera.zFar;

    bbMin.x = step * glm::floor(bbMin.x / step);
    bbMin.y = step * glm::floor(bbMin.y / step);

    bbMax.x = step * glm::ceil(bbMax.x / step);
    bbMax.y = step * glm::ceil(bbMax.y / step);

    glm::vec3 anchorPoint = lightView3 * getShadowAnchor(frustumCamera, cascade);

    bbWidth = bbMax - bbMin;

    texelWidth = bbWidth.x / shadowMapSize;
    texelHeight = bbWidth.y / shadowMapSize;

    bbMin.x = anchorPoint.x + glm::floor((bbMin.x - anchorPoint.x) / texelWidth) * texelWidth;
    bbMin.y = anchorPoint.y + glm::floor((bbMin.y - anchorPoint.y) / texelHeight) * texelHeight;

    bbMax.x = anchorPoint.x + glm::ceil((bbMax.x - anchorPoint.x) / texelWidth) * texelWidth;
    bbMax.y = anchorPoint.y + glm::ceil((bbMax.y - anchorPoint.y) / texelHeight) * texelHeight;

    bbMin.z -= zNearExpansion;
    bbMax.z += settings.zFarExpandMul * zFarExpansion;

    glm::mat4x4 lightProj =
      getLightProjMatrix(bbMax.x, bbMin.x, bbMax.y, bbMin.y, bbMin.z, bbMax.z);


    projViewMatrices[cascade] = lightProj * lightView;
  }
}

void ShadowCastingDirectionalLight::drawGui()
{
  ImGui::Begin("Application Settings");

  ImGui::SeparatorText("Shadow Casting Directional Light");

  ImGui::DragFloat("Plane expansion", &settings.zExpansion, 0.1f, 0.0f, 5000.0f);
  ImGui::DragFloat("Near Plane Offset", &settings.zNearOffset, 0.1f, 0.0f, 5000.0f);
  ImGui::DragFloat("Far Plane Offset", &settings.zFarOffset, 0.1f, 0.0f, 5000.0f);
  ImGui::DragFloat("Cascade Rotation Margin", &settings.rotationMargin, 0.001f, 0.0f, 1.0f);
  ImGui::DragFloat("Far Plane Expansion Multiplier", &settings.zFarExpandMul, 0.01f, 0.0f, 5.0f);

  if (ImGui::DragFloat(
        "Near Planes Backward Offset", &settings.nearPlanesBackwardOffset, 0.01f, 0.0f, 50.0f))
  {
    shaderInfo.nearPlanesBackwardOffset = settings.nearPlanesBackwardOffset;
  }
  ImGui::End();
}

void ShadowCastingDirectionalLight::prepareForDraw()
{
  auto& currentInfoBuffer = infoBuffer->get();

  currentInfoBuffer.map();

  std::size_t offset = 0;

  std::memcpy(
    currentInfoBuffer.data() + offset,
    &shaderInfo,
    sizeof(ShadowCastingDirectionalLight::ShaderInfo));

  offset += sizeof(ShadowCastingDirectionalLight::ShaderInfo);
  std::memcpy(
    currentInfoBuffer.data() + offset,
    projViewMatrices.data(),
    sizeof(glm::mat4x4) * projViewMatrices.size());

  offset += sizeof(glm::mat4x4) * projViewMatrices.size();

  std::memcpy(currentInfoBuffer.data() + offset, planes.data(), sizeof(float) * planes.size());

  currentInfoBuffer.unmap();
}

std::array<glm::vec3, 8> ShadowCastingDirectionalLight::getWorldSpaceFrustumCorners(
  const glm::mat4x4& proj_view)
{
  glm::mat4x4 invProjView = glm::inverse(proj_view);

  std::array corners = {
    glm::vec3(-1, -1, 0),
    glm::vec3(1, -1, 0),
    glm::vec3(-1, 1, 0),
    glm::vec3(1, 1, 0),
    glm::vec3(-1, -1, 1),
    glm::vec3(1, -1, 1),
    glm::vec3(-1, 1, 1),
    glm::vec3(1, 1, 1)};

  for (auto& corner : corners)
  {
    glm::vec4 invCorner = invProjView * glm::vec4(corner, 1.0f);
    corner = invCorner / invCorner.w;
  }

  return corners;
}

glm::vec3 ShadowCastingDirectionalLight::getFrustumCenter(const std::array<glm::vec3, 8>& corners)
{
  glm::vec3 center = {0, 0, 0};

  for (const auto& corner : corners)
  {
    center += corner;
  }

  center /= 8.0f;

  return center;
}

glm::vec3 ShadowCastingDirectionalLight::getShadowAnchor(
  const Camera& main_camera, [[maybe_unused]] std::size_t cascade_index)
{
  if (cascade_index == 0)
  {
    return {0, 0, 0};
  }
  return -main_camera.position;
}

glm::mat4x4 ShadowCastingDirectionalLight::getLightViewMatrix(
  const glm::vec3& camera_pos, bool world_space)
{
  glm::mat4x4 lightView = glm::lookAtLH({0, 0, 0}, shaderInfo.light.direction, {0, 1, 0});

  if (world_space)
  {
    glm::mat4x4 worldToCam = glm::identity<glm::mat4x4>();
    worldToCam[3] = glm::vec4(-camera_pos, 1.0f);

    lightView = worldToCam * lightView;
  }

  return lightView;
}

glm::mat4x4 ShadowCastingDirectionalLight::getLightProjMatrix(
  float left, float right, float bottom, float top, float z_near, float z_far)
{
  glm::mat4x4 proj = {
    2.0f / (right - left),
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    2.0 / (top - bottom),
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    (glm::abs(z_far - z_near) < 0.00000001f) ? 0.0f : 1.0f / (z_far - z_near),
    0.0f,
    -(right + left) / (right - left),
    -(top + bottom) / (top - bottom),
    (glm::abs(z_far - z_near) < 0.00000001f) ? 0.0f : z_near / (z_near - z_far),
    1.0f};
  return proj;
}
