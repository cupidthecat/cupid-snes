include(FetchContent)

# Prefer an installed SDK; fetch the pinned release when one is unavailable.
find_package(SDL3 QUIET)
if(NOT SDL3_FOUND)
  message(STATUS "SDL3 not found locally - fetching via FetchContent")
  FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.8
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
  )
  # Build SDL3 as a static library so the frontend is self-contained.
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL3)
endif()
