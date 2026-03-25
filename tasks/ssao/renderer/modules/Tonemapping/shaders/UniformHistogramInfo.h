#ifndef UNIFORM_HISTOGRAM_INFO_H_INCLUDED
#define UNIFORM_HISTOGRAM_INFO_H_INCLUDED

#include "cpp_glsl_compat.h"


struct UniformHistogramInfo
{
  shader_float binStepSize;
  shader_float minWorldLuminance; // relative
  shader_float maxWorldLuminance; // relative
  shader_float minWorldBrightness;
  shader_float maxWorldBrightness;
};


#endif // UNIFORM_HISTOGRAM_INFO_H_INCLUDED
