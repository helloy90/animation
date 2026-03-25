#ifndef ANTIALIASING_PARAMS_H_INCLUDED
#define ANTIALIASING_PARAMS_H_INCLUDED

#include "cpp_glsl_compat.h"


struct AntialiasingParams
{
  shader_mat4 currentProjView;
  shader_mat4 currentInvProjView;
  shader_mat4 previousProjView;
  shader_float previousFrameUsage;
  // NOTE - in view space
  shader_float maxDepthDelta;
  shader_float catmullRomBParam;
  shader_float catmullRomCParam;
};


#endif // ANTIALIASING_PARAMS_H_INCLUDED
