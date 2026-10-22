include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

set(imgui_release 1.92.5)
FetchContent_Declare(imgui
  SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/imgui-${imgui_release}-src
  URL https://codeload.github.com/ocornut/imgui/tar.gz/refs/tags/v${imgui_release}
  URL_HASH SHA256=0eb50fe9aeba1a51f96b5843c7f630a32ed2e9362d693c61b87e4fa870cf826d
)
FetchContent_MakeAvailable(imgui)

add_library(snes_desktop_ui_library STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(snes_desktop_ui_library PUBLIC
  ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_compile_definitions(snes_desktop_ui_library PUBLIC IMGUI_USE_WCHAR32)
target_link_libraries(snes_desktop_ui_library PUBLIC SDL3::SDL3)
