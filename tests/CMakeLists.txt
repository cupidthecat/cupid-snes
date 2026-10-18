add_executable(snes_core_tests
  test_main.cpp
)

target_link_libraries(snes_core_tests
  PRIVATE
    snes_core
)

# Keep the assert-based checks active in Release builds too.
if(MSVC)
  target_compile_options(snes_core_tests PRIVATE /UNDEBUG)
else()
  target_compile_options(snes_core_tests PRIVATE -UNDEBUG)
endif()

# Debug and sanitizer builds can keep the legacy main() fixtures on the stack
# simultaneously. Reserve space for them with both MSVC and Clang on Windows.
if(MINGW)
  target_link_options(snes_core_tests PRIVATE "LINKER:--stack,8388608")
elseif(WIN32)
  target_link_options(snes_core_tests PRIVATE "LINKER:/STACK:8388608")
endif()

add_test(NAME snes_core_tests COMMAND snes_core_tests)

add_executable(snes_gameboy_cpu_tests test_gameboy_cpu.cpp)
target_link_libraries(snes_gameboy_cpu_tests PRIVATE snes_core)
add_test(NAME snes_gameboy_cpu_tests COMMAND snes_gameboy_cpu_tests)

add_executable(snes_gameboy_host_tests test_gameboy_host.cpp)
target_link_libraries(snes_gameboy_host_tests PRIVATE snes_core)
add_test(NAME snes_gameboy_host_tests COMMAND snes_gameboy_host_tests)

add_executable(snes_gameboy_clocked_io_tests test_gameboy_clocked_io.cpp)
target_link_libraries(snes_gameboy_clocked_io_tests PRIVATE snes_core)
add_test(NAME snes_gameboy_clocked_io_tests COMMAND snes_gameboy_clocked_io_tests)

add_executable(snes_gameboy_cartridge_tests test_gameboy_cartridge.cpp)
target_link_libraries(snes_gameboy_cartridge_tests PRIVATE snes_core)
add_test(NAME snes_gameboy_cartridge_tests COMMAND snes_gameboy_cartridge_tests)

add_executable(snes_gameboy_machine_tests test_gameboy_machine.cpp)
target_link_libraries(snes_gameboy_machine_tests PRIVATE snes_core)
add_test(NAME snes_gameboy_machine_tests COMMAND snes_gameboy_machine_tests)

add_executable(snes_gameboy_sound_channels_tests test_gameboy_sound_channels.cpp)
target_link_libraries(snes_gameboy_sound_channels_tests PRIVATE snes_core)
add_test(NAME snes_gameboy_sound_channels_tests COMMAND snes_gameboy_sound_channels_tests)

add_executable(snes_gameboy_video_tests test_gameboy_video.cpp)
target_link_libraries(snes_gameboy_video_tests PRIVATE snes_core)
add_test(NAME snes_gameboy_video_tests COMMAND snes_gameboy_video_tests)

add_executable(snes_frame_execution_tests test_frame_execution.cpp)
target_link_libraries(snes_frame_execution_tests PRIVATE snes_core)
add_test(NAME snes_frame_execution_tests COMMAND snes_frame_execution_tests)

add_executable(snes_console_reset_tests test_console_reset.cpp)
target_link_libraries(snes_console_reset_tests PRIVATE snes_core)
add_test(NAME snes_console_reset_tests COMMAND snes_console_reset_tests)

add_executable(snes_upd_dsp_tests test_upd_dsp.cpp)
target_link_libraries(snes_upd_dsp_tests PRIVATE snes_core)
add_test(NAME snes_upd_dsp_tests COMMAND snes_upd_dsp_tests)

add_executable(snes_nec_dsp_tests test_nec_dsp.cpp)
target_link_libraries(snes_nec_dsp_tests PRIVATE snes_core)
add_test(NAME snes_nec_dsp_tests COMMAND snes_nec_dsp_tests)

add_executable(snes_firmware_asset_tests test_firmware_assets.cpp)
target_link_libraries(snes_firmware_asset_tests PRIVATE snes_core)
add_test(NAME snes_firmware_asset_tests COMMAND snes_firmware_asset_tests)

add_executable(snes_execution_context_tests test_execution_context.cpp)
target_link_libraries(snes_execution_context_tests PRIVATE snes_core)
add_test(NAME snes_execution_context_tests COMMAND snes_execution_context_tests)

add_executable(snes_ppu_regression_tests test_ppu_regressions.cpp)
target_link_libraries(snes_ppu_regression_tests PRIVATE snes_core)
add_test(NAME snes_ppu_regression_tests COMMAND snes_ppu_regression_tests)

add_executable(snes_hardware_regression_tests test_hardware_regressions.cpp)
target_link_libraries(snes_hardware_regression_tests PRIVATE snes_core)
add_test(NAME snes_hardware_regression_tests COMMAND snes_hardware_regression_tests)

add_executable(snes_cartridge_device_tests test_cartridge_devices.cpp)
target_link_libraries(snes_cartridge_device_tests PRIVATE snes_core)
add_test(NAME snes_cartridge_device_tests COMMAND snes_cartridge_device_tests)

add_executable(snes_msu1_tests test_msu1.cpp)
target_link_libraries(snes_msu1_tests PRIVATE snes_core)
add_test(NAME snes_msu1_tests COMMAND snes_msu1_tests)

add_executable(snes_msu1_integration_tests test_msu1_integration.cpp)
target_link_libraries(snes_msu1_integration_tests PRIVATE snes_core)
add_test(NAME snes_msu1_integration_tests COMMAND snes_msu1_integration_tests)

add_executable(snes_sdd1_tests test_sdd1.cpp)
target_link_libraries(snes_sdd1_tests PRIVATE snes_core)
add_test(NAME snes_sdd1_tests COMMAND snes_sdd1_tests)

add_executable(snes_st010_tests test_st010.cpp)
target_link_libraries(snes_st010_tests PRIVATE snes_core)
add_test(NAME snes_st010_tests COMMAND snes_st010_tests)

add_executable(snes_spc7110_tests test_spc7110.cpp)
target_link_libraries(snes_spc7110_tests PRIVATE snes_core)
add_test(NAME snes_spc7110_tests COMMAND snes_spc7110_tests)

add_executable(snes_spc7110_timing_tests test_spc7110_timing.cpp)
target_link_libraries(snes_spc7110_timing_tests PRIVATE snes_core)
add_test(NAME snes_spc7110_timing_tests COMMAND snes_spc7110_timing_tests)

add_executable(snes_spc7110_parts_tests test_spc7110_parts.cpp)
target_link_libraries(snes_spc7110_parts_tests PRIVATE snes_core)
add_test(NAME snes_spc7110_parts_tests COMMAND snes_spc7110_parts_tests)

add_executable(snes_dma_timing_tests test_dma_timing.cpp)
target_link_libraries(snes_dma_timing_tests PRIVATE snes_core)
add_test(NAME snes_dma_timing_tests COMMAND snes_dma_timing_tests)

add_executable(snes_automatic_hdma_tests test_automatic_hdma.cpp)
target_link_libraries(snes_automatic_hdma_tests PRIVATE snes_core)
add_test(NAME snes_automatic_hdma_tests COMMAND snes_automatic_hdma_tests)

add_executable(snes_controller_tests test_controller_ports.cpp)
target_link_libraries(snes_controller_tests PRIVATE snes_core)
add_test(NAME snes_controller_tests COMMAND snes_controller_tests)

add_executable(snes_light_gun_tests test_light_guns.cpp)
target_link_libraries(snes_light_gun_tests PRIVATE snes_core)
add_test(NAME snes_light_gun_tests COMMAND snes_light_gun_tests)

add_executable(snes_sdl_input_tests test_sdl_input.cpp ../frontend/src/SdlInputProvider.cpp)
target_include_directories(snes_sdl_input_tests PRIVATE ../frontend/src)
target_link_libraries(snes_sdl_input_tests PRIVATE snes_core SDL3::SDL3)
add_test(NAME snes_sdl_input_tests COMMAND snes_sdl_input_tests)

add_executable(snes_cpu_regression_tests test_cpu_regressions.cpp)
target_link_libraries(snes_cpu_regression_tests PRIVATE snes_core)
add_test(NAME snes_cpu_regression_tests COMMAND snes_cpu_regression_tests)
add_executable(snes_bus_scheduling_tests test_bus_scheduling.cpp)
target_link_libraries(snes_bus_scheduling_tests PRIVATE snes_core)
add_test(NAME snes_bus_scheduling_tests COMMAND snes_bus_scheduling_tests)
add_executable(snes_console_timing_tests test_console_timing.cpp)
target_link_libraries(snes_console_timing_tests PRIVATE snes_core)
add_test(NAME snes_console_timing_tests COMMAND snes_console_timing_tests)
add_executable(snes_dma_arbitration_tests test_dma_arbitration.cpp)
target_link_libraries(snes_dma_arbitration_tests PRIVATE snes_core)
add_test(NAME snes_dma_arbitration_tests COMMAND snes_dma_arbitration_tests)
add_executable(snes_processor_startup_tests test_processor_startup.cpp)
target_link_libraries(snes_processor_startup_tests PRIVATE snes_core)
add_test(NAME snes_processor_startup_tests COMMAND snes_processor_startup_tests)
add_executable(snes_cpu_math_tests test_cpu_math.cpp)
target_link_libraries(snes_cpu_math_tests PRIVATE snes_core)
add_test(NAME snes_cpu_math_tests COMMAND snes_cpu_math_tests)
add_executable(snes_smp_wait_tests test_smp_wait.cpp)
target_link_libraries(snes_smp_wait_tests PRIVATE snes_core)
add_test(NAME snes_smp_wait_tests COMMAND snes_smp_wait_tests)
add_executable(snes_sound_clocking_tests test_sound_clocking.cpp)
target_link_libraries(snes_sound_clocking_tests PRIVATE snes_core)
add_test(NAME snes_sound_clocking_tests COMMAND snes_sound_clocking_tests)
add_executable(snes_spc_bus_tests test_spc_bus.cpp)
target_link_libraries(snes_spc_bus_tests PRIVATE snes_core)
add_test(NAME snes_spc_bus_tests COMMAND snes_spc_bus_tests)
add_executable(snes_sa1_tests test_sa1.cpp)
target_link_libraries(snes_sa1_tests PRIVATE snes_core)
add_test(NAME snes_sa1_tests COMMAND snes_sa1_tests)
add_executable(snes_superfx_tests test_superfx.cpp)
target_link_libraries(snes_superfx_tests PRIVATE snes_core)
add_test(NAME snes_superfx_tests COMMAND snes_superfx_tests)
add_executable(snes_cartridge_processor_tests test_cartridge_processors.cpp)
target_link_libraries(snes_cartridge_processor_tests PRIVATE snes_core)
add_test(NAME snes_cartridge_processor_tests COMMAND snes_cartridge_processor_tests)
add_executable(snes_cx4_tests test_cx4.cpp)
target_link_libraries(snes_cx4_tests PRIVATE snes_core)
add_test(NAME snes_cx4_tests COMMAND snes_cx4_tests)
add_executable(snes_seta_protocol_tests test_seta_protocol.cpp)
target_link_libraries(snes_seta_protocol_tests PRIVATE snes_core)
add_test(NAME snes_seta_protocol_tests COMMAND snes_seta_protocol_tests)
add_executable(snes_bsx_tests test_bsx.cpp)
target_link_libraries(snes_bsx_tests PRIVATE snes_core)
add_test(NAME snes_bsx_tests COMMAND snes_bsx_tests)
add_executable(snes_cartridge_command_tests test_cartridge_commands.cpp)
target_link_libraries(snes_cartridge_command_tests PRIVATE snes_core)
add_test(NAME snes_cartridge_command_tests COMMAND snes_cartridge_command_tests)
add_executable(snes_dsp3_tests test_dsp3.cpp)
target_link_libraries(snes_dsp3_tests PRIVATE snes_core)
add_test(NAME snes_dsp3_tests COMMAND snes_dsp3_tests)
add_executable(snes_dsp4_tests test_dsp4.cpp)
target_link_libraries(snes_dsp4_tests PRIVATE snes_core)
add_test(NAME snes_dsp4_tests COMMAND snes_dsp4_tests)
add_test(NAME snes_cpu_stop_tests COMMAND snes_cpu_regression_tests stop)
set_tests_properties(snes_cpu_stop_tests PROPERTIES TIMEOUT 5)

add_executable(snes_audio_regression_tests test_audio_regressions.cpp)
find_package(Threads REQUIRED)
target_link_libraries(snes_audio_regression_tests PRIVATE snes_core Threads::Threads)
add_test(NAME snes_audio_regression_tests COMMAND snes_audio_regression_tests)
add_executable(snes_dsp_phase_tests test_dsp_phases.cpp)
target_link_libraries(snes_dsp_phase_tests PRIVATE snes_core)
add_test(NAME snes_dsp_phase_tests COMMAND snes_dsp_phase_tests)

add_executable(snes_sdl_audio_tests test_sdl_audio.cpp ../frontend/src/SdlAudioOutput.cpp)

add_executable(snes_frame_pacing_tests test_frame_pacing.cpp)
target_include_directories(snes_frame_pacing_tests PRIVATE ../frontend/src)
target_link_libraries(snes_frame_pacing_tests PRIVATE snes_core)
add_test(NAME snes_frame_pacing_tests COMMAND snes_frame_pacing_tests)

add_executable(snes_sdl_video_tests test_sdl_video.cpp ../frontend/src/SdlVideoOutput.cpp)
target_include_directories(snes_sdl_video_tests PRIVATE ../frontend/src)
target_link_libraries(snes_sdl_video_tests PRIVATE snes_core SDL3::SDL3)
add_test(NAME snes_sdl_video_tests COMMAND snes_sdl_video_tests)

add_executable(snes_save_ram_tests test_save_ram.cpp ../frontend/src/SaveRamFile.cpp)
target_include_directories(snes_save_ram_tests PRIVATE ../frontend/src)
target_link_libraries(snes_save_ram_tests PRIVATE snes_core SDL3::SDL3)
add_test(NAME snes_save_ram_tests COMMAND snes_save_ram_tests)
target_include_directories(snes_sdl_audio_tests PRIVATE ../frontend/src)
target_link_libraries(snes_sdl_audio_tests PRIVATE snes_core SDL3::SDL3)
foreach(rate 32000 32040 44100 48000)
  add_test(NAME snes_sdl_audio_${rate} COMMAND snes_sdl_audio_tests ${rate})
endforeach()

# Opt in with an existing diagnostic checkout; ROMs are never fetched by a build.
set(SNES_DIAGNOSTIC_ROM_DIR "" CACHE PATH "Root of an external SnesTests checkout")
if(SNES_DIAGNOSTIC_ROM_DIR)
  set(dma_irq_rom "${SNES_DIAGNOSTIC_ROM_DIR}/dma_irq_test/dma_irq_test.sfc")
  if(NOT EXISTS "${dma_irq_rom}")
    message(FATAL_ERROR "SNES_DIAGNOSTIC_ROM_DIR does not contain dma_irq_test/dma_irq_test.sfc")
  endif()
  add_test(NAME snes_external_dma_irq
    COMMAND snes_rom_diagnostic "${dma_irq_rom}" 300
      --expect "${CMAKE_CURRENT_SOURCE_DIR}/expected/dma_irq.txt")
  set_tests_properties(snes_external_dma_irq PROPERTIES TIMEOUT 120 LABELS "external")
endif()

set(SNES_PROCESSOR_DIAGNOSTIC_DIR "" CACHE PATH "Root of the external CPU/SPC diagnostic ROM release")
if(SNES_PROCESSOR_DIAGNOSTIC_DIR)
  foreach(processor cpu spc)
    if(processor STREQUAL "cpu")
      set(processor_rom "${SNES_PROCESSOR_DIAGNOSTIC_DIR}/cputest/cputest-full.sfc")
    else()
      set(processor_rom "${SNES_PROCESSOR_DIAGNOSTIC_DIR}/spctest/spctest.sfc")
    endif()
    if(NOT EXISTS "${processor_rom}")
      message(FATAL_ERROR "Missing processor diagnostic ROM: ${processor_rom}")
    endif()
    add_test(NAME snes_external_${processor}
      COMMAND snes_rom_diagnostic "${processor_rom}" 220 0
        --expect "${CMAKE_CURRENT_SOURCE_DIR}/expected/processor.txt")
    set_tests_properties(snes_external_${processor} PROPERTIES TIMEOUT 120 LABELS "external")
  endforeach()
endif()

add_executable(snes_ppu_background_tests test_ppu_background.cpp)
target_link_libraries(snes_ppu_background_tests PRIVATE snes_core)
add_test(NAME snes_ppu_background_tests COMMAND snes_ppu_background_tests)

add_executable(snes_ppu_object_tests test_ppu_objects.cpp)
target_link_libraries(snes_ppu_object_tests PRIVATE snes_core)
add_test(NAME snes_ppu_object_tests COMMAND snes_ppu_object_tests)

add_executable(snes_ppu_port_tests test_ppu_ports.cpp)
target_link_libraries(snes_ppu_port_tests PRIVATE snes_core)
add_test(NAME snes_ppu_port_tests COMMAND snes_ppu_port_tests)

add_executable(snes_st018_tests test_st018.cpp)
target_link_libraries(snes_st018_tests PRIVATE snes_core)
add_test(NAME snes_st018_tests COMMAND snes_st018_tests)

add_executable(snes_arm6_tests test_arm6.cpp)
target_link_libraries(snes_arm6_tests PRIVATE snes_core)
add_test(NAME snes_arm6_tests COMMAND snes_arm6_tests)

add_executable(snes_hg51b_tests test_hg51b.cpp)
target_link_libraries(snes_hg51b_tests PRIVATE snes_core)
add_test(NAME snes_hg51b_tests COMMAND snes_hg51b_tests)

add_executable(snes_cx4_processor_tests test_cx4_processor.cpp)
target_link_libraries(snes_cx4_processor_tests PRIVATE snes_core)
add_test(NAME snes_cx4_processor_tests COMMAND snes_cx4_processor_tests)

add_executable(snes_srtc_tests test_srtc.cpp)
target_link_libraries(snes_srtc_tests PRIVATE snes_core)
add_test(NAME snes_srtc_tests COMMAND snes_srtc_tests)

add_executable(snes_srtc_timing_tests test_srtc_timing.cpp)
target_link_libraries(snes_srtc_timing_tests PRIVATE snes_core)
add_test(NAME snes_srtc_timing_tests COMMAND snes_srtc_timing_tests)

add_executable(snes_cartridge_storage_tests test_cartridge_storage.cpp)
target_link_libraries(snes_cartridge_storage_tests PRIVATE snes_core)
add_test(NAME snes_cartridge_storage_tests COMMAND snes_cartridge_storage_tests)

add_executable(snes_competition_cartridge_tests test_competition_cartridges.cpp)
target_link_libraries(snes_competition_cartridge_tests PRIVATE snes_core)
add_test(NAME snes_competition_cartridge_tests COMMAND snes_competition_cartridge_tests)
