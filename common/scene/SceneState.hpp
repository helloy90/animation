#pragma once

#include <cstdint>
enum class SceneState : uint8_t
{
  None = 0,
  Idle = 1,
  Walking = 2,
  Running = 3,
};
