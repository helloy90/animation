#ifndef SPECTRUMUPDATEPARAMS_H_INCLUDED
#define SPECTRUMUPDATEPARAMS_H_INCLUDED

#include "cpp_glsl_compat.h"


struct SpectrumUpdateParams
{
  shader_float foamDecayRate;
  shader_float foamBias;
  shader_float foamThreshold;
  shader_float foamMultiplier;
  shader_float wavePeriod;
};


#endif // SPECTRUMUPDATEPARAMS_H_INCLUDED
