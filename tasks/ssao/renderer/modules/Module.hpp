#pragma once

#include <concepts>
#include <etna/Vulkan.hpp>
#include <type_traits>
enum class ModuleType : uint32_t
{
  Invalid = ~uint32_t(0),
  Render = 0,
  PostFX = 1,
  Generator = 2,
};

template <class T>
concept Module = requires(T t) {
  { t.type };
  requires std::same_as<std::remove_cv_t<decltype(t.type)>, ModuleType>;
};
