#ifndef DIRECTIONAL_LIGHT_H_INCLUDED
#define DIRECTIONAL_LIGHT_H_INCLUDED

#include "cpp_glsl_compat.h"


struct DirectionalLight
{
  shader_vec3 direction;
  float intensity;
  shader_vec3 color;
};


#endif // DIRECTIONAL_LIGHT_H_INCLUDED
