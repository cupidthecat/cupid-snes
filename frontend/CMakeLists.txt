add_library(snes_frontend_support STATIC
  src/SdlVideoOutput.cpp
  src/SdlAudioOutput.cpp
  src/SdlInputProvider.cpp
  src/SaveRamFile.cpp
  src/session/Session.cpp
  src/desktop/DesktopMenu.cpp
  src/desktop/FileDialogs.cpp
  src/desktop/DesktopApplication.cpp
)
target_include_directories(snes_frontend_support PUBLIC src)
target_link_libraries(snes_frontend_support PUBLIC snes_core SDL3::SDL3)
if(WIN32)
  target_link_libraries(snes_frontend_support PRIVATE user32)
endif()
add_executable(snes_frontend src/main.cpp)
target_link_libraries(snes_frontend PRIVATE snes_frontend_support)
