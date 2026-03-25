#pragma once

#include <glm/vec4.hpp>
#include "Texture2D.hpp"


struct Material
{
  enum class Id : uint32_t
  {
    Invalid = ~uint32_t(0)
  };

  glm::vec4 baseColorFactor;
  float roughnessFactor;
  float metallicFactor;
  // almost identical to reference,
  // TextureManager must be present
  Texture2D::Id baseColorTexture;
  Texture2D::Id metallicRoughnessTexture;
  Texture2D::Id normalTexture;
};
