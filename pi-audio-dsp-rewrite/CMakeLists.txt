cmake_minimum_required(VERSION 3.16)
project(pi_audio_dsp_rewrite VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------------------------------------------------------ #
#  Options                                                             #
# ------------------------------------------------------------------ #
option(PI_AUDIO_DIAG_LOG "Enable diagnostic file logging to /run/pi-audio/diag.log" OFF)
if(PI_AUDIO_DIAG_LOG)
    message(STATUS "Diagnostic logging: ENABLED (writes to /run/pi-audio/diag.log)")
else()
    message(STATUS "Diagnostic logging: DISABLED (production mode, no file I/O in audio path)")
endif()

# ------------------------------------------------------------------ #
#  Common include paths                                               #
# ------------------------------------------------------------------
set(REWRITE_CORE_INC   ${CMAKE_SOURCE_DIR}/src/core/include)
set(REWRITE_HAL_INC    ${CMAKE_SOURCE_DIR}/src/hal/include)
set(REWRITE_MSG_INC    ${CMAKE_SOURCE_DIR}/src/msg/include)
set(REWRITE_CTRL_INC   ${CMAKE_SOURCE_DIR}/src/control/include)
set(REWRITE_CORE_SRC   ${CMAKE_SOURCE_DIR}/src/core/src)

# ------------------------------------------------------------------ #
#  Message library (header-only + compile unit)                       #
# ------------------------------------------------------------------
add_library(msg_lib STATIC src/msg/src/queue.cpp src/msg/src/shm_ring.cpp)
target_include_directories(msg_lib PUBLIC ${REWRITE_MSG_INC})
if(UNIX AND NOT APPLE)
    target_link_libraries(msg_lib PRIVATE rt)
endif()

# ------------------------------------------------------------------ #
#  DSP Core — portable, no exceptions, no RTTI, no ALSA              #
# ------------------------------------------------------------------
add_library(dsp_core STATIC
    src/core/src/dsp_core.cpp
    src/core/src/routing.cpp
    src/core/src/stages/gain_stage.cpp
    src/core/src/stages/biquad_stage.cpp
    src/core/src/stages/crossover_stage.cpp
    src/core/src/stages/eq_chain_stage.cpp
    src/core/src/stages/trim_stage.cpp
    src/core/src/stages/delay_stage.cpp
    src/core/src/stages/limiter_stage.cpp
    src/core/src/drift_controller.cpp
    src/core/src/fractional_resampler.cpp
)
target_include_directories(dsp_core PUBLIC ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC})
target_link_libraries(dsp_core PRIVATE msg_lib)

# Portability constraints (A4).
target_compile_options(dsp_core PRIVATE -fno-exceptions -fno-rtti -ffast-math -Wdouble-promotion -Werror=return-type)
set_target_properties(dsp_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Diagnostic logging control.
if(PI_AUDIO_DIAG_LOG)
    target_compile_definitions(dsp_core PRIVATE PI_AUDIO_DIAG_LOG)
endif()

# ------------------------------------------------------------------ #
#  SimHal — no ALSA dependency, compiles on any platform             #
# ------------------------------------------------------------------
add_library(hal_sim STATIC
    src/hal/src/sim_hal.cpp
    src/hal/src/engine.cpp
)
target_include_directories(hal_sim PUBLIC ${REWRITE_HAL_INC} ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC})
target_link_libraries(hal_sim PRIVATE dsp_core msg_lib)
target_compile_options(hal_sim PRIVATE -fno-exceptions -fno-rtti)

# Diagnostic logging control.
if(PI_AUDIO_DIAG_LOG)
    target_compile_definitions(hal_sim PRIVATE PI_AUDIO_DIAG_LOG)
endif()

# ------------------------------------------------------------------ #
#  AlsaHal — Linux-only, links ALSA                                  #
# ------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED alsa)

    add_library(hal_alsa STATIC
        src/hal/src/alsa_hal.cpp
        src/hal/src/engine.cpp
    )
    target_include_directories(hal_alsa PUBLIC ${REWRITE_HAL_INC} ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC})
    target_link_libraries(hal_alsa PRIVATE dsp_core msg_lib ${ALSA_LIBRARIES} rt)
    target_compile_options(hal_alsa PRIVATE -fno-exceptions -fno-rtti -fpermissive)
    target_include_directories(hal_alsa PRIVATE ${ALSA_INCLUDE_DIRS})

    # Diagnostic logging control.
    if(PI_AUDIO_DIAG_LOG)
        target_compile_definitions(hal_alsa PRIVATE PI_AUDIO_DIAG_LOG)
    endif()
endif()

# ------------------------------------------------------------------ #
#  Control plane — JSON parser, config loader, state writer          #
# ------------------------------------------------------------------
add_library(control_plane STATIC
    src/control/src/config_loader.cpp
    src/control/src/state_writer.cpp
)
target_include_directories(control_plane PUBLIC ${REWRITE_CTRL_INC} ${REWRITE_MSG_INC})
target_link_libraries(control_plane PRIVATE msg_lib)

# ------------------------------------------------------------------ #
#  Main binary                                                        #
# ------------------------------------------------------------------
add_executable(pi-audio-engine src/bin/pi-audio-engine.cpp)
target_include_directories(pi-audio-engine PRIVATE ${REWRITE_HAL_INC} ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC} ${REWRITE_CTRL_INC})
target_link_libraries(pi-audio-engine PRIVATE control_plane hal_sim msg_lib pthread)

if(UNIX AND NOT APPLE)
    target_link_libraries(pi-audio-engine PRIVATE hal_alsa)
endif()

# Diagnostic logging control.
if(PI_AUDIO_DIAG_LOG)
    target_compile_definitions(pi-audio-engine PRIVATE PI_AUDIO_DIAG_LOG)
endif()

# ------------------------------------------------------------------ #
#  Tests                                                              #
# ------------------------------------------------------------------
enable_testing()

add_executable(test_dsp_core tests/test_dsp_core.cpp)
target_include_directories(test_dsp_core PRIVATE ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_HAL_INC} ${REWRITE_MSG_INC})
target_link_libraries(test_dsp_core PRIVATE dsp_core hal_sim msg_lib pthread)

add_executable(test_command_queue tests/test_command_queue.cpp)
target_include_directories(test_command_queue PRIVATE ${REWRITE_MSG_INC})
target_link_libraries(test_command_queue PRIVATE msg_lib pthread)

add_executable(test_sim_hal tests/test_sim_hal.cpp)
target_include_directories(test_sim_hal PRIVATE ${REWRITE_HAL_INC} ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC})
target_link_libraries(test_sim_hal PRIVATE hal_sim dsp_core msg_lib pthread)

add_test(NAME test_dsp_core      COMMAND test_dsp_core)
add_test(NAME test_command_queue  COMMAND test_command_queue)
add_test(NAME test_sim_hal        COMMAND test_sim_hal)

# B3: Drift compensation tests (includes minimal DFT for THD+N measurement)
add_executable(test_drift_compensation tests/test_drift_compensation.cpp)
target_include_directories(test_drift_compensation PRIVATE ${REWRITE_CORE_INC} ${REWRITE_MSG_INC})
target_link_libraries(test_drift_compensation PRIVATE dsp_core msg_lib pthread m)
add_test(NAME test_drift_compensation COMMAND test_drift_compensation)

# B6: Clock state reaction tests
add_executable(test_clock_state tests/test_clock_state.cpp)
target_include_directories(test_clock_state PRIVATE ${REWRITE_CORE_INC} ${REWRITE_MSG_INC})
target_link_libraries(test_clock_state PRIVATE dsp_core msg_lib pthread)
add_test(NAME test_clock_state COMMAND test_clock_state)

# B4: Xrun recovery — SimHal (cross-platform)
add_executable(test_xrun_recovery_sim tests/test_xrun_recovery_sim.cpp)
target_include_directories(test_xrun_recovery_sim PRIVATE ${REWRITE_HAL_INC} ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC})
target_link_libraries(test_xrun_recovery_sim PRIVATE hal_sim dsp_core msg_lib pthread)
add_test(NAME test_xrun_recovery_sim COMMAND test_xrun_recovery_sim)

# A5: Extended SimHal tests (noise input, clock transitions, multiple ppm offsets)
add_executable(test_sim_hal_extended tests/test_sim_hal_extended.cpp)
target_include_directories(test_sim_hal_extended PRIVATE ${REWRITE_HAL_INC} ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC})
target_link_libraries(test_sim_hal_extended PRIVATE hal_sim dsp_core msg_lib pthread)
add_test(NAME test_sim_hal_extended COMMAND test_sim_hal_extended)

# B4: Xrun recovery — AlsaHal (Linux-only, conditional)
if(UNIX AND NOT APPLE)
    add_executable(test_xrun_recovery_alsa tests/test_xrun_recovery_alsa.cpp)
    target_include_directories(test_xrun_recovery_alsa PRIVATE ${REWRITE_HAL_INC} ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC} ${ALSA_INCLUDE_DIRS})
    target_link_libraries(test_xrun_recovery_alsa PRIVATE hal_alsa dsp_core msg_lib pthread)
    add_test(NAME test_xrun_recovery_alsa COMMAND test_xrun_recovery_alsa)
endif()

# C1-C5: RT thread hygiene tests
add_executable(test_rt_thread_hygiene tests/test_rt_thread_hygiene.cpp)
target_include_directories(test_rt_thread_hygiene PRIVATE ${REWRITE_HAL_INC} ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC})
target_link_libraries(test_rt_thread_hygiene PRIVATE hal_sim dsp_core msg_lib pthread)
add_test(NAME test_rt_thread_hygiene COMMAND test_rt_thread_hygiene)

# D1: Monitoring and networking tests (StateWriter, telemetry queue)
add_executable(test_monitoring_networking tests/test_monitoring_networking.cpp)
target_include_directories(test_monitoring_networking PRIVATE ${REWRITE_CTRL_INC} ${REWRITE_MSG_INC})
target_link_libraries(test_monitoring_networking PRIVATE control_plane msg_lib pthread)
add_test(NAME test_monitoring_networking COMMAND test_monitoring_networking)

# E1-E3: SHM/IPC/Config tests (shm lifecycle, header validation, config loader)
add_executable(test_shm_ipc_config tests/test_shm_ipc_config.cpp)
target_include_directories(test_shm_ipc_config PRIVATE ${REWRITE_CTRL_INC} ${REWRITE_MSG_INC})
target_link_libraries(test_shm_ipc_config PRIVATE control_plane msg_lib pthread m)
add_test(NAME test_shm_ipc_config COMMAND test_shm_ipc_config)

# Volume/Mute measurement tests (gain accuracy, mute override, clamping)
add_executable(test_volume_mute tests/test_volume_mute.cpp)
target_include_directories(test_volume_mute PRIVATE ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC})
target_link_libraries(test_volume_mute PRIVATE dsp_core msg_lib pthread m)
add_test(NAME test_volume_mute COMMAND test_volume_mute)

# Input routing tests (PGM_L duplication, PGM_R isolation, Mic metering)
add_executable(test_input_routing tests/test_input_routing.cpp)
target_include_directories(test_input_routing PRIVATE ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC})
target_link_libraries(test_input_routing PRIVATE dsp_core msg_lib pthread m)
add_test(NAME test_input_routing COMMAND test_input_routing)

# BiquadStage tests (Phase 1: crossover stages)
add_executable(test_biquad_stage tests/test_biquad_stage.cpp)
target_include_directories(test_biquad_stage PRIVATE ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC})
target_link_libraries(test_biquad_stage PRIVATE dsp_core msg_lib pthread m)
add_test(NAME test_biquad_stage COMMAND test_biquad_stage)

# CrossoverStage tests (Phase 3: cascaded filter chain)
add_executable(test_crossover_stage tests/test_crossover_stage.cpp)
target_include_directories(test_crossover_stage PRIVATE ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC})
target_link_libraries(test_crossover_stage PRIVATE dsp_core msg_lib pthread m)
add_test(NAME test_crossover_stage COMMAND test_crossover_stage)

# Engine integration tests (Phase 4: crossover pipeline wiring)
add_executable(test_engine_integration tests/test_engine_integration.cpp)
target_include_directories(test_engine_integration PRIVATE ${REWRITE_HAL_INC} ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC} ${REWRITE_CTRL_INC})
target_link_libraries(test_engine_integration PRIVATE hal_sim dsp_core control_plane msg_lib pthread m)
add_test(NAME test_engine_integration COMMAND test_engine_integration)

# End-to-end crossover validation (Phase 5: full pipeline + bare-metal audit)
add_executable(test_crossover_e2e tests/test_crossover_e2e.cpp)
target_include_directories(test_crossover_e2e PRIVATE ${REWRITE_HAL_INC} ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC} ${REWRITE_CTRL_INC})
target_link_libraries(test_crossover_e2e PRIVATE hal_sim dsp_core control_plane msg_lib pthread m)
add_test(NAME test_crossover_e2e COMMAND test_crossover_e2e)

# TrimStage tests (fixed linear gain with phase inversion)
add_executable(test_trim tests/test_trim.cpp)
target_include_directories(test_trim PRIVATE ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC})
target_link_libraries(test_trim PRIVATE dsp_core msg_lib pthread m)
add_test(NAME test_trim COMMAND test_trim)

# DelayStage tests (circular buffer delay line)
add_executable(test_delay tests/test_delay.cpp)
target_include_directories(test_delay PRIVATE ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC})
target_link_libraries(test_delay PRIVATE dsp_core msg_lib pthread m)
add_test(NAME test_delay COMMAND test_delay)

# LimiterStage tests (peak limiter with attack/release envelope follower)
add_executable(test_limiter tests/test_limiter.cpp)
target_include_directories(test_limiter PRIVATE ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC})
target_link_libraries(test_limiter PRIVATE dsp_core msg_lib pthread m)
add_test(NAME test_limiter COMMAND test_limiter)

# EqChainStage tests (cascaded peaking EQ biquad chain)
add_executable(test_eq_chain tests/test_eq_chain.cpp)
target_include_directories(test_eq_chain PRIVATE ${REWRITE_CORE_INC} ${REWRITE_CORE_SRC} ${REWRITE_MSG_INC} ${REWRITE_CTRL_INC})
target_link_libraries(test_eq_chain PRIVATE dsp_core control_plane msg_lib pthread m)
add_test(NAME test_eq_chain COMMAND test_eq_chain)

# ConfigLoader EQ parsing tests (WebUI bands schema)
add_executable(test_config_loader_eq tests/test_config_loader_eq.cpp)
target_include_directories(test_config_loader_eq PRIVATE ${REWRITE_CTRL_INC} ${REWRITE_MSG_INC})
target_link_libraries(test_config_loader_eq PRIVATE control_plane msg_lib pthread m)
add_test(NAME test_config_loader_eq COMMAND test_config_loader_eq)

# ------------------------------------------------------------------ #
#  Install                                                            #
# ------------------------------------------------------------------ #
#  Install                                                            #
# ------------------------------------------------------------------
install(TARGETS pi-audio-engine DESTINATION bin)
