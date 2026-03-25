#ifndef MATERIAL_RENDER_PARAMS_H_INCLUDED
#define MATERIAL_RENDER_PARAMS_H_INCLUDED

#include "cpp_glsl_compat.h"


struct MaterialRenderParams
{
  shader_vec4 baseColorFactor;
  float roughnessFactor;
  float metallicFactor;
};


#endif // MATERIAL_RENDER_PARAMS_H_INCLUDED
