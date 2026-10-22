include(${PROJECT_SOURCE_DIR}/cmake/ImGui.cmake)

add_library(snes_frontend_support STATIC
  src/SdlVideoOutput.cpp
  src/SdlAudioOutput.cpp
  src/SdlInputProvider.cpp
  src/SaveRamFile.cpp
  src/storage/AtomicFile.cpp
  src/settings/Preferences.cpp
  src/settings/ConsoleDevices.cpp
  src/input/Hotkeys.cpp
  src/input/InputBindings.cpp
  src/session/Session.cpp
  src/desktop/DesktopMenu.cpp
  src/desktop/FileDialogs.cpp
  src/desktop/DesktopApplication.cpp
  src/desktop/PreferencesDialog.cpp
  src/desktop/DesktopUi.cpp
)
target_include_directories(snes_frontend_support PUBLIC src)
target_link_libraries(snes_frontend_support PUBLIC snes_core SDL3::SDL3)
target_link_libraries(snes_frontend_support PRIVATE snes_desktop_ui_library)
if(WIN32)
  target_link_libraries(snes_frontend_support PRIVATE user32)
endif()
add_executable(snes_frontend src/main.cpp)
target_link_libraries(snes_frontend PRIVATE snes_frontend_support)
add_custom_command(TARGET snes_frontend POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:snes_frontend>/licenses
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${imgui_SOURCE_DIR}/LICENSE.txt
    $<TARGET_FILE_DIR:snes_frontend>/licenses/DearImGui.txt
)
