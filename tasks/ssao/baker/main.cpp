#include "Baker.hpp"

#include <spdlog/spdlog.h>


int main(int argc, char* argv[])
{
  if (argc != 2)
  {
    spdlog::error("Arguments amount is not correct! Expected 1 argument - path to .gltf file.");
    return EXIT_FAILURE;
  }

  auto path = std::filesystem::path(argv[1]);
  if (!std::filesystem::exists(path))
  {
    spdlog::error("Such file does not exist!");
    return EXIT_FAILURE;
  }

  auto baker = Baker(path);

  baker.run();

  return EXIT_SUCCESS;
}
