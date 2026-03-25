#ifndef TERRAIN_GENERATION_PARAMS_H_INCLUDED
#define TERRAIN_GENERATION_PARAMS_H_INCLUDED

#include "cpp_glsl_compat.h"


struct TerrainGenerationParams
{
  shader_uint numberOfSamples;
  shader_float seed;
  shader_float gradientRotation;
  shader_float amplitudeDecay;
  shader_float initialAmplitude;
  shader_float lacunarity;
  shader_float noiseRotation;
  shader_float scale;
  shader_float heightAmplifier;
  shader_float heightOffset;
  shader_vec2 angleVariance;
  shader_vec2 frequencyVariance;
  shader_vec2 offset;
};


#endif // TERRAIN_GENERATION_PARAMS_H_INCLUDED
