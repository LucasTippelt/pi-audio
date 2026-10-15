cmake_minimum_required(VERSION 3.16)
project(pi_audio_dsp_rewrite VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------------------------------------------------------ #
#  Common include paths                                               #
# ------------------------------------------------------------------ #
set(REWRITE_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src)

# ------------------------------------------------------------------ #
#  Message library (header-only + compile unit)                       #
# ------------------------------------------------------------------ #
add_library(msg_lib INTERFACE)
target_include_directories(msg_lib INTERFACE ${REWRITE_INCLUDE_DIR})
target_sources(msg_lib PRIVATE src/msg/src/queue.cpp)

# ------------------------------------------------------------------ #
#  DSP Core — portable, no exceptions, no RTTI, no ALSA              #
# ------------------------------------------------------------------ #
add_library(dsp_core STATIC
    src/core/src/dsp_core.cpp
    src/core/src/routing.cpp
    src/core/src/stages/gain_stage.cpp
)
target_include_directories(dsp_core PUBLIC ${REWRITE_INCLUDE_DIR})
target_link_libraries(dsp_core PRIVATE msg_lib)

# Portability constraints (A4).
target_compile_options(dsp_core PRIVATE -fno-exceptions -fno-rtti -ffast-math -Wdouble-promotion -Werror=return-type)
set_target_properties(dsp_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ------------------------------------------------------------------ #
#  SimHal — no ALSA dependency, compiles on any platform             #
# ------------------------------------------------------------------ #
add_library(hal_sim STATIC
    src/hal/src/sim_hal.cpp
    src/hal/src/engine.cpp
)
target_include_directories(hal_sim PUBLIC ${REWRITE_INCLUDE_DIR})
target_link_libraries(hal_sim PRIVATE dsp_core msg_lib)
target_compile_options(hal_sim PRIVATE -fno-exceptions -fno-rtti)

# ------------------------------------------------------------------ #
#  AlsaHal — Linux-only, links ALSA                                  #
# ------------------------------------------------------------------ #
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED alsa)

    add_library(hal_alsa STATIC
        src/hal/src/alsa_hal.cpp
        src/hal/src/engine.cpp
    )
    target_include_directories(hal_alsa PUBLIC ${REWRITE_INCLUDE_DIR})
    target_link_libraries(hal_alsa PRIVATE dsp_core msg_lib ${ALSA_LIBRARIES} rt)
    target_compile_options(hal_alsa PRIVATE -fno-exceptions -fno-rtti)
    target_include_directories(hal_alsa PRIVATE ${ALSA_INCLUDE_DIRS})
endif()

# ------------------------------------------------------------------ #
#  Control plane — JSON parser, config loader, state writer          #
# ------------------------------------------------------------------ #
add_library(control_plane STATIC
    src/control/src/config_loader.cpp
    src/control/src/state_writer.cpp
)
target_include_directories(control_plane PUBLIC ${REWRITE_INCLUDE_DIR})
target_link_libraries(control_plane PRIVATE msg_lib)

# ------------------------------------------------------------------ #
#  Main binary                                                        #
# ------------------------------------------------------------------ #
add_executable(pi-audio-engine src/bin/pi-audio-engine.cpp)
target_include_directories(pi-audio-engine PRIVATE ${REWRITE_INCLUDE_DIR})
target_link_libraries(pi-audio-engine PRIVATE control_plane hal_sim msg_lib)

if(UNIX AND NOT APPLE)
    target_link_libraries(pi-audio-engine PRIVATE hal_alsa)
endif()

# ------------------------------------------------------------------ #
#  Tests                                                              #
# ------------------------------------------------------------------ #
enable_testing()

add_executable(test_dsp_core tests/test_dsp_core.cpp)
target_include_directories(test_dsp_core PRIVATE ${REWRITE_INCLUDE_DIR})
target_link_libraries(test_dsp_core PRIVATE dsp_core hal_sim msg_lib)

add_executable(test_command_queue tests/test_command_queue.cpp)
target_include_directories(test_command_queue PRIVATE ${REWRITE_INCLUDE_DIR})
target_link_libraries(test_command_queue PRIVATE msg_lib)

add_executable(test_sim_hal tests/test_sim_hal.cpp)
target_include_directories(test_sim_hal PRIVATE ${REWRITE_INCLUDE_DIR})
target_link_libraries(test_sim_hal PRIVATE hal_sim dsp_core msg_lib)

add_test(NAME test_dsp_core      COMMAND test_dsp_core)
add_test(NAME test_command_queue  COMMAND test_command_queue)
add_test(NAME test_sim_hal        COMMAND test_sim_hal)

# ------------------------------------------------------------------ #
#  Install                                                            #
# ------------------------------------------------------------------ #
install(TARGETS pi-audio-engine DESTINATION bin)
