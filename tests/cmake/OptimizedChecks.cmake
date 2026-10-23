option(SNES_BUNDLE_OPTIMIZED_TESTS "Link optimized checks into a shared test executable" OFF)

set(SNES_OPTIMIZED_TEST_TARGETS
  snes_disassembly_tests
  snes_cpu_debugger_tests
  snes_cpu_debugger_application_tests
  snes_memory_inspection_tests
  snes_ram_search_tests
  snes_memory_tools_tests
  snes_memory_application_tests
  snes_cheat_tests
  snes_cheat_storage_tests
  snes_cheat_application_tests
  snes_sha256_tests
  snes_msu1_state_tests
  snes_state_archive_tests
  snes_processor_state_tests
  snes_audio_state_tests
  snes_smp_state_boundary_tests
  snes_emulator_state_tests
  snes_ppu_state_tests
  snes_system_state_tests
  snes_cartridge_memory_state_tests
  snes_spc7110_state_tests
  snes_command_dsp_state_tests
  snes_arm_state_tests
  snes_cx4_state_tests
  snes_superfx_state_tests
  snes_gameboy_state_tests
  snes_cartridge_state_tests
  snes_gameboy_cpu_tests
  snes_gameboy_host_tests
  snes_gameboy_clocked_io_tests
  snes_gameboy_cartridge_tests
  snes_gameboy_camera_tests
  snes_gameboy_tpp1_tests
  snes_gameboy_oam_tests
  snes_gameboy_machine_tests
  snes_gameboy_video_tests
  snes_gameboy_sound_channels_tests
  snes_gameboy_audio_tests
  snes_gameboy_peripherals_tests
  snes_super_game_boy_tests
  snes_msu1_tests
  snes_msu1_integration_tests
  snes_cartridge_compatibility_tests
  snes_cartridge_storage_tests
  snes_bus_scheduling_tests
  snes_sound_clocking_tests
  snes_dsp_batch_tests
  snes_power_on_state_tests
  snes_ppu_background_tests
  snes_ppu_object_tests
  snes_ppu_port_tests
  snes_ppu_scanline_tests
  snes_ppu_regression_tests
  snes_dma_timing_tests
  snes_controller_tests
  snes_light_gun_tests
  snes_frame_execution_tests
  snes_frame_pacing_tests
  snes_console_reset_tests
  snes_competition_cartridge_tests
  snes_sdl_audio_tests
  snes_sdl_video_tests
  snes_sdl_input_tests
  snes_bsx_tests
  snes_cartridge_command_tests
  snes_sdd1_tests
  snes_processor_startup_tests
  snes_srtc_tests
  snes_srtc_timing_tests
  snes_cartridge_device_tests
  snes_superfx_tests
  snes_cartridge_processor_tests
  snes_st010_tests
  snes_spc7110_timing_tests
  snes_save_ram_tests
  snes_session_tests
  snes_state_storage_tests
  snes_input_frame_progress_tests
  snes_state_application_tests
  snes_battery_directory_tests
  snes_desktop_tests
  snes_desktop_application_tests
  snes_preferences_tests
  snes_cx4_processor_tests
  snes_sa1_tests
  snes_st018_tests
)

function(snes_add_check name target)
  if(SNES_BUNDLE_OPTIMIZED_TESTS AND target IN_LIST SNES_OPTIMIZED_TEST_TARGETS)
    add_test(NAME ${name} COMMAND snes_optimized_test_runner ${target} ${ARGN})
  else()
    add_test(NAME ${name} COMMAND ${target} ${ARGN})
  endif()
  if(target IN_LIST SNES_OPTIMIZED_TEST_TARGETS)
    set_tests_properties(${name} PROPERTIES LABELS optimized)
  endif()
endfunction()

function(snes_configure_optimized_checks)
  set(check_dependencies ${SNES_OPTIMIZED_TEST_TARGETS})
  if(SNES_BUNDLE_OPTIMIZED_TESTS)
    set(SNES_CHECK_RUNNER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/runner")
    set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/optimized")
    file(MAKE_DIRECTORY "${generated_dir}")
    set(check_objects)
    set(check_libraries)
    set(SNES_CHECK_CASES "")
    foreach(check IN LISTS SNES_OPTIMIZED_TEST_TARGETS)
      get_target_property(check_sources ${check} SOURCES)
      set(entry_source "")
      set(extra_sources)
      foreach(source IN LISTS check_sources)
        get_filename_component(source "${source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        file(READ "${source}" contents)
        if(contents MATCHES "(^|[\r\n])int main\\(")
          if(entry_source)
            message(FATAL_ERROR "${check} has more than one test entry point")
          endif()
          set(entry_source "${source}")
        else()
          list(APPEND extra_sources "${source}")
        endif()
      endforeach()
      if(NOT entry_source)
        message(FATAL_ERROR "${check} has no test entry point")
      endif()
      set(SNES_CHECK_SOURCE "${entry_source}")
      configure_file("${SNES_CHECK_RUNNER_DIR}/Entry.cpp.in" "${generated_dir}/${check}.cpp" @ONLY)
      set(object_target ${check}_entry)
      add_library(${object_target} OBJECT "${generated_dir}/${check}.cpp" ${extra_sources})
      get_target_property(libraries ${check} LINK_LIBRARIES)
      target_link_libraries(${object_target} PRIVATE ${libraries})
      list(APPEND check_libraries ${libraries})
      foreach(property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS)
        get_target_property(value ${check} ${property})
        if(value)
          set_property(TARGET ${object_target} PROPERTY ${property} "${value}")
        endif()
      endforeach()
      target_compile_definitions(${object_target} PRIVATE
        SNES_FIXTURE_ENTRY=snes_fixture_${check}
        SNES_FIXTURE_WRAPPER=Run_${check}
      )
      # A renamed main follows ordinary function return rules.
      if(MSVC)
        target_compile_options(${object_target} PRIVATE /we4715 /we4716)
      else()
        target_compile_options(${object_target} PRIVATE -Werror=return-type)
      endif()
      list(APPEND check_objects $<TARGET_OBJECTS:${object_target}>)
      string(APPEND SNES_CHECK_CASES "SNES_OPTIMIZED_CHECK(${check})\n")
      set_property(TARGET ${check} PROPERTY EXCLUDE_FROM_ALL TRUE)
    endforeach()
    configure_file("${SNES_CHECK_RUNNER_DIR}/Cases.inc.in" "${generated_dir}/Cases.inc" @ONLY)
    add_executable(snes_optimized_test_runner "${SNES_CHECK_RUNNER_DIR}/Main.cpp" ${check_objects})
    target_include_directories(snes_optimized_test_runner PRIVATE "${generated_dir}")
    list(REMOVE_DUPLICATES check_libraries)
    target_link_libraries(snes_optimized_test_runner PRIVATE ${check_libraries})
    set(check_dependencies snes_optimized_test_runner)
  endif()
  add_custom_target(snes_optimized_checks
    DEPENDS snes_frontend snes_rom_diagnostic snes_rom_smoke snes_trace_dump ${check_dependencies}
  )
endfunction()
