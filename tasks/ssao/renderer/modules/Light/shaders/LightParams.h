#ifndef LIGHTPARAMS_H_INCLUDED
#define LIGHTPARAMS_H_INCLUDED

#include "cpp_glsl_compat.h"


struct LightParams
{
  shader_uint lightsAmount;
  shader_uint directionalLightsAmount;
  shader_uint shadowCastingDirLightsAmount;
  // attenuation constants
  shader_float constant;
  shader_float linear;
  shader_float quadratic;
};


#endif // LIGHTPARAMS_H_INCLUDED
