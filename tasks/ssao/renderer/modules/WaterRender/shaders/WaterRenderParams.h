#ifndef WATERRENDERPARAMS_H_INCLUDED
#define WATERRENDERPARAMS_H_INCLUDED

#include "cpp_glsl_compat.h"


struct WaterRenderParams
{
  shader_vec4 scatterColor;
  shader_vec4 bubbleColor;
  shader_vec4 foamColor;
  shader_float roughness;
  shader_float reflectionStrength;
  shader_float wavePeakScatterStrength;
  shader_float scatterStrength;
  shader_float scatterShadowStrength;
  shader_float bubbleDensity;
  shader_float transparencyStrength;
};

#endif // WATERRENDERPARAMS_H_INCLUDED
