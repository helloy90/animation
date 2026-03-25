#ifndef GENERALSPECTRUMPARAMS_H_INCLUDED
#define GENERALSPECTRUMPARAMS_H_INCLUDED

#include "cpp_glsl_compat.h"


struct GeneralSpectrumParams
{
  shader_float gravity;
  shader_float depth;
  shader_float lowCutoff;
  shader_float highCutoff;
  shader_uint seed;
};

#endif // GENERALSPECTRUMPARAMS_H_INCLUDED
