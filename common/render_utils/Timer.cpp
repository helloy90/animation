#include "Timer.hpp"


void Timer::start()
{
  start_ = std::chrono::steady_clock::now();
}

void Timer::stop()
{
  end_ = std::chrono::steady_clock::now();
  std::chrono::duration<double> time = end_ - start_;
  spdlog::info("Elapsed time - {}s, timer stopped", time.count());
}
