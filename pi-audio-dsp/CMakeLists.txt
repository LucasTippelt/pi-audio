cmake_minimum_required(VERSION 3.10)
project(pi_audio_dsp VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find ALSA
find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)

# Include directories (build dir for generated headers)
include_directories(${CMAKE_BINARY_DIR})
include_directories(${CMAKE_SOURCE_DIR}/src/include)
include_directories(${ALSA_INCLUDE_DIRS})

# Core library
add_library(audio_core
    src/lib/core/engine.cpp
    src/lib/core/config.cpp
    src/lib/core/logger.cpp
    src/lib/core/monitor.cpp
    src/lib/core/tcp_relay.cpp
    src/lib/adapters/apply.cpp
    src/lib/adapters/measure.cpp
    src/lib/adapters/status.cpp
    src/lib/adapters/test.cpp
)
target_link_libraries(audio_core ${ALSA_LIBRARIES})
target_include_directories(audio_core PUBLIC ${CMAKE_SOURCE_DIR}/src/include)

# Generate version info (must run before audio_core compiles)
add_custom_target(gen_version
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/gen-version.sh ${CMAKE_BINARY_DIR}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating version info"
)
add_dependencies(audio_core gen_version)

# Binaries
add_executable(pi-audio-engine src/bin/pi-audio-engine.cpp)
target_link_libraries(pi-audio-engine audio_core)

add_executable(pi-audio-apply src/bin/pi-audio-apply.cpp)
target_link_libraries(pi-audio-apply audio_core)

add_executable(pi-audio-status src/bin/pi-audio-status.cpp)
target_link_libraries(pi-audio-status audio_core)

add_executable(pi-audio-measure src/bin/pi-audio-measure.cpp)
target_link_libraries(pi-audio-measure audio_core)

add_executable(pi-audio-test src/bin/pi-audio-test.cpp)
target_link_libraries(pi-audio-test audio_core)

# Install targets
install(TARGETS pi-audio-engine pi-audio-apply pi-audio-status pi-audio-measure pi-audio-test
        DESTINATION /usr/local/bin)
