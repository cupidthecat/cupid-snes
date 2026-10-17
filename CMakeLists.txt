cmake_minimum_required(VERSION 3.21)

project(snes_emulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SNES_ENABLE_TESTS "Build tests" ON)
option(SNES_ENABLE_LTO "Optimize release builds across translation units" ON)

add_compile_definitions(__PRFCHWINTRIN_H)

include(cmake/SDL3.cmake)

# Check the compiler and linker together before enabling release optimization.
# Apply this after SDL configuration so its cached build keeps its own settings.
if(SNES_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT SNES_IPO_SUPPORTED LANGUAGES CXX)
  if(SNES_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
  else()
    message(STATUS "Link-time optimization is unavailable; using normal compilation")
  endif()
endif()

add_subdirectory(core)
add_subdirectory(frontend)
add_subdirectory(tools)

if(SNES_ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
