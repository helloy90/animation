set_source_files_properties(mikktspace.c PROPERTIES LANGUAGE CXX)

# make mikkspace compile in CI
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU") # gcc and clang
  add_compile_options(-Wno-pedantic -Wno-unused)
elseif(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  add_compile_options(/wd4201 /wd4456 /wd4505) 
endif()

add_executable(baker
  main.cpp
  Baker.cpp
  mikktspace.c
)

target_link_libraries(baker
  PRIVATE tinygltf glm::glm etna)
