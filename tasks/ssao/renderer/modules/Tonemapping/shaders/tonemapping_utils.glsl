const float log10 = 2.302585092994; // precompute

// absolute in cd/m^2, hardcode from monitor spec
const float minDisplayLuminance = 0.35;
const float maxDisplayLuminance = 350;

float getAbsoluteLuminance(vec3 color, float minWorldLuminance, float maxWorldLuminance)
{
  float luminance = 0.299 * color.r + 0.587 * color.g + 0.114 * color.b;
  // y = kx + b
  float k = (maxDisplayLuminance - minDisplayLuminance) / (maxWorldLuminance - minWorldLuminance);
  return k * luminance + minDisplayLuminance;
}

float getAbsoluteLuminance(float luminance, float minWorldLuminance, float maxWorldLuminance)
{
  float k = (maxDisplayLuminance - minDisplayLuminance) / (maxWorldLuminance - minWorldLuminance);
  return k * luminance + minDisplayLuminance;
}

uint getBin(float luminance, float binStepSize, uint len)
{
  return clamp(uint(floor((luminance - minDisplayLuminance) / binStepSize)), 0, len - 1);
}
