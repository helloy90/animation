#pragma once

#include <cstdint>

#include "etna/Image.hpp"


struct Texture2D
{
  enum class Id : uint32_t
  {
    Invalid = ~uint32_t(0)
  };

  etna::Image texture;
};
