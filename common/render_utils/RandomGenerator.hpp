#include <random>


// not very generic for now
// template <class float>
class RandomGenerator
{
public:
  RandomGenerator(uint32_t seed, float left, float right)
    : generator(seed)
    , distribution(left, right) {};

  float get() { return distribution(generator); }

private:
  std::mt19937 generator;
  std::uniform_real_distribution<float> distribution;
};
