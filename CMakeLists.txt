cmake_minimum_required(VERSION 3.22)

project(MediaMax
    VERSION 0.1.0
    DESCRIPTION "MediaMax media server - playback engine, control interface and core engine"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(MEDIAMAX_BUILD_TESTS "Build the unit test suite" ON)
option(MEDIAMAX_ENABLE_FFMPEG "Enable FFmpeg based media decoding (file playback)" ON)
option(MEDIAMAX_ENABLE_VALIDATION "Build NVRHI with its validation backend" ON)

# ---------------------------------------------------------------------------
# Modules
# ---------------------------------------------------------------------------

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerSettings)
include(ThirdParty)
include(FFmpeg)
include(VulkanDeploy)

# ---------------------------------------------------------------------------
# Sub-projects
# ---------------------------------------------------------------------------

add_subdirectory(core)
add_subdirectory(render)
add_subdirectory(playback)
add_subdirectory(control)

# Single-output deployment: collects both executables, their runtime DLLs
# and the example shows into build/dist/ (issue 17).
include(Deploy)

if(MEDIAMAX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(core/tests)
    add_subdirectory(render/tests)
endif()
