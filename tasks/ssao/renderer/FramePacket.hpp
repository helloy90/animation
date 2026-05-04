#pragma once

#include <scene/ArcballCamera.hpp>
#include <scene/SceneState.hpp>

struct FramePacket
{
  ArcballCamera mainCam;
  glm::mat4x4 sceneTransform;
  SceneState currentState;
  float currentTime = 0;
  float deltaTime = 0;
};
