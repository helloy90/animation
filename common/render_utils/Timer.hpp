#pragma once

#include <chrono>

#include <spdlog/spdlog.h>


// Check time of a function execution that is on startup (for other functions use Tracy)
#define TIMER_START(timer, function)                                                               \
  timer.start();                                                                                   \
  spdlog::info("Started timer for function {}", #function);

#define TIMER_END(timer) timer.stop();

class Timer
{
public:
  void start();
  void stop();

private:
  std::chrono::time_point<std::chrono::steady_clock> start_;
  std::chrono::time_point<std::chrono::steady_clock> end_;
};

// template <class Function, class... Args>
// void runtime(Function function, Args&&... args)
// {
//   auto start = std::chrono::steady_clock::now();

//   function(std::forward<Args>(args)...);

//   auto end = std::chrono::steady_clock::now();
//   std::chrono::duration<double> time = end - start;
//   spdlog::info("Benchmarking complete for function {}, elapsed time - {}s", time.count());
// }
