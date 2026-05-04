#pragma once

#include <glm/ext.hpp>
#include <glm/glm.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

struct ArcballCamera
{
  glm::vec3 position;
  glm::vec3 lookAtPos;
  glm::vec2 curRotation;
  glm::vec2 targetRotation;
  float distance;
  glm::quat rotation;
  float lerpStrength{100.0f};
  float fov{60.0f};
  float zNear{0.01f};
  float zFar{2000.0f};

  void lookAt(glm::vec3 from, glm::vec3 to, glm::vec3 up)
  {
    position = from;
    lookAtPos = to;
    distance = glm::distance(position, lookAtPos);
    rotation = glm::quatLookAtLH(normalize(lookAtPos - position), normalize(up));
    curRotation = glm::eulerAngles(rotation);
    targetRotation = curRotation;
  }

  void rotate(float up_angle_deg, float right_angle_deg)
  {
    targetRotation += glm::vec2(glm::radians(right_angle_deg), glm::radians(up_angle_deg));
  }

  void update(float dt)
  {
    curRotation = glm::mix(curRotation, targetRotation, lerpStrength * dt);

    glm::vec3 dir = glm::vec3(
      glm::cos(curRotation.x) * glm::cos(curRotation.y),
      glm::sin(curRotation.y),
      glm::sin(curRotation.x) * glm::cos(curRotation.y));
    position = lookAtPos - distance * dir;
    rotation = glm::quatLookAtLH(normalize(lookAtPos - position), normalize(glm::vec3(0, 1, 0)));
  }

  const glm::vec3 right() const { return rotation * glm::vec3{-1, 0, 0}; }

  const glm::vec3 up() const { return rotation * glm::vec3{0, 1, 0}; }

  const glm::vec3 forward() const { return rotation * glm::vec3{0, 0, 1}; }

  glm::mat4x4 viewItm() const
  {
    return translate(glm::identity<glm::mat4>(), position) * mat4_cast(rotation);
  }

  glm::mat4x4 viewTm() const { return inverse(viewItm()); }

  glm::mat4x4 projTm(float aspect) const
  {
    return glm::perspectiveLH_ZO(-glm::radians(fov), aspect, zNear, zFar);
  }

  glm::mat4x4 projTmZRev(float aspect) const
  {
    return glm::perspectiveLH_ZO(-glm::radians(fov), aspect, zFar, zNear);
  }

  glm::mat4x4 projTmZRevFarInf(float aspect) const
  {
    assert(glm::abs(aspect - std::numeric_limits<float>::epsilon()) > 0.0f);

    const float tanHalfFovy = glm::tan(-glm::radians(fov) / 2.0f);

    glm::mat4x4 result(0.0f);
    result[0][0] = 1.0f / (aspect * tanHalfFovy);
    result[1][1] = 1.0f / (tanHalfFovy);
    result[2][2] = 0;
    result[2][3] = 1.0f;
    result[3][2] = zNear;
    return result;
  }

  glm::mat4x4 projItmZRevFarInf(float aspect) const
  {
    assert(glm::abs(aspect - std::numeric_limits<float>::epsilon()) > 0.0f);

    const float tanHalfFovy = glm::tan(-glm::radians(fov) / 2.0f);

    glm::mat4x4 result(0.0f);
    result[0][0] = (aspect * tanHalfFovy);
    result[1][1] = (tanHalfFovy);
    result[2][2] = 0;
    result[2][3] = 1.0f / zNear;
    result[3][2] = -1.0f;
    result[3][3] = 0.0f;
    return result;
  }
};
