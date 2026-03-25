#ifndef WATERGENERATIONPARAMS_H_INCLUDED
#define WATERGENERATIONPARAMS_H_INCLUDED

#include "cpp_glsl_compat.h"


struct SpectrumGenerationParams
{
  shader_float scale;
  shader_float angle;
  shader_float spreadBlend;
  shader_float swell;
  shader_float jonswapAlpha;
  shader_float peakFrequency;
  shader_float peakEnhancement;
  shader_float shortWavesFade;
};


#endif // WATERGENERATIONPARAMS_H_INCLUDED
