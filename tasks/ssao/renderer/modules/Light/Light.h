#ifndef LIGHT_H_INCLUDED
#define LIGHT_H_INCLUDED

#include "cpp_glsl_compat.h"


struct Light
{
  shader_vec3 pos;
  float radius;
  shader_vec3 color;
  float intensity;
};


#endif // LIGHT_H_INCLUDED
