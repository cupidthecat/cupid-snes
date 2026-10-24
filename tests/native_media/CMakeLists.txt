cmake_minimum_required(VERSION 3.21)
project(snes_native_media_checks LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(SNES_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_executable(snes_native_media_tests
  ${SNES_SOURCE_ROOT}/tests/test_msu1_state.cpp
  ${SNES_SOURCE_ROOT}/core/src/cartridge/Msu1.cpp
  ${SNES_SOURCE_ROOT}/core/src/cartridge/msu1/Resource.cpp
  ${SNES_SOURCE_ROOT}/core/src/cartridge/msu1/Stream.cpp
  ${SNES_SOURCE_ROOT}/core/src/state/Msu1State.cpp
  ${SNES_SOURCE_ROOT}/core/src/state/StateArchive.cpp
  ${SNES_SOURCE_ROOT}/core/src/state/Sha256.cpp
)
target_include_directories(snes_native_media_tests PRIVATE ${SNES_SOURCE_ROOT}/core/include)
enable_testing()
add_test(NAME snes_native_media_tests COMMAND snes_native_media_tests)
