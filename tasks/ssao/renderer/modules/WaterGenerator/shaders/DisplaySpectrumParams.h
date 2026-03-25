#ifndef DISPLAYSPECTRUMPARAMS_H_INCLUDED
#define DISPLAYSPECTRUMPARAMS_H_INCLUDED

#include "cpp_glsl_compat.h"


struct DisplaySpectrumParams
{
  shader_float scale;
  shader_float windSpeed;
  shader_float windDirection;
  shader_float windActionLength;
  shader_float spreadBlend;
  shader_float swell;
  shader_float peakEnhancement;
  shader_float shortWavesFade;
};


#endif // DISPLAYSPECTRUMPARAMS_H_INCLUDED
