# Third-party dependency management for MediaMax.
#
# All dependencies are pinned to immutable versions (tags or commit hashes)
# and fetched at configure time into the build tree. Nothing is installed on
# the host system and nothing is committed to the repository.
#
# Dependency matrix:
#   glm          header-only math               (core, render)
#   glfw         windowing + Vulkan surface     (playback)
#   nvrhi        graphics abstraction (Vulkan)  (render)
#   miniaudio    audio device output            (playback)
#   nlohmann/json  JSON (protocol + show files) (core, control)
#   doctest      unit test framework            (tests, build-time only)
#
# FFmpeg is handled separately in cmake/FFmpeg.cmake because its distribution
# model differs per platform (prebuilt binaries on Windows, system packages
# on Linux).

include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------------------
# glm - header-only math library
# ---------------------------------------------------------------------------

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glm)

# ---------------------------------------------------------------------------
# GLFW - window creation and input, Vulkan surface support
# ---------------------------------------------------------------------------

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
# GLFW fetches its own copy of the Vulkan headers for its surface functions.

FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw)

# ---------------------------------------------------------------------------
# NVRHI - graphics abstraction, Vulkan backend
#
# Built from source as static libraries (targets: nvrhi, nvrhi_vk).
# Only the Vulkan backend is enabled: MediaMax uses Vulkan on all platforms.
#
# The SDK's Vulkan headers must be the single vulkan.hpp for the whole build:
# find_package(Vulkan) creates the Vulkan::Headers target (SDK include dir),
# which NVRHI picks up before it would otherwise fetch its own pinned copy.
# NVRHI links its vulkan-headers target into nvrhi_vk only PRIVATELY, so a
# fetched copy would be invisible to our translation units -- we would then
# compile our TUs against the SDK's vulkan.hpp while NVRHI's TUs use its own
# copy, and the two versions of the vk-hpp default-dispatcher class layout
# would silently disagree (and their init() overloads differ).
find_package(Vulkan REQUIRED glslangValidator)

set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_VALIDATION ${MEDIAMAX_ENABLE_VALIDATION} CACHE BOOL "" FORCE)
set(NVRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(nvrhi
    GIT_REPOSITORY https://github.com/NVIDIA-RTX/NVRHI.git
    GIT_TAG 6b96fb03e07539f08327aea76c56d55f1de9d906
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(nvrhi)

# ---------------------------------------------------------------------------
# miniaudio - single-header audio library
#
# The implementation (MINIAUDIO_IMPLEMENTATION) is compiled exactly once, in
# the playback engine (playback/src/AudioSink.cpp).
# ---------------------------------------------------------------------------

FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.25
    GIT_SHALLOW TRUE)
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
    FetchContent_Populate(miniaudio)
endif()

add_library(miniaudio INTERFACE)
target_include_directories(miniaudio INTERFACE "${miniaudio_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# nlohmann/json - JSON for the control protocol and show files
# ---------------------------------------------------------------------------

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# doctest - unit test framework (build/test time only)
# ---------------------------------------------------------------------------

if(MEDIAMAX_BUILD_TESTS)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.12
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(doctest)
endif()
