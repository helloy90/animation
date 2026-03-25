#pragma once

#include <glm/glm.hpp>

// Contains per frame updated information.
// Used in push constants or constant buffers.
// (If changed dont forget to change all shaders that use it)
struct RenderPacket
{
  // If there are 3 or more HeavyInfo members used in push constant, use constant buffer instead
  // includes info for velocity buffer constuction
  struct HeavyInfo
  {
    glm::mat4x4 projView;
    glm::mat4x4 previousProjView;
    glm::vec2 currentJitter;
    glm::vec2 previousJitter;
    glm::vec3 cameraWorldPosition;
  };

  HeavyInfo heavyInfo;
  float time;
  glm::uvec2 resolution;
  float nearPlane;
  float farPlane;
};
