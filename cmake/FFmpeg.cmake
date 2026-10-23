# FFmpeg integration for MediaMax.
#
# FFmpeg is required for decoding media files (video, audio, images).
#
# Windows:
#   Uses a pinned prebuilt shared build (BtbN/FFmpeg-Builds) downloaded at
#   configure time, unless MEDIAMAX_FFMPEG_DIR points at an already extracted
#   build (layout: <dir>/include, <dir>/lib/*.lib, <dir>/bin/*.dll).
#
# Linux (Ubuntu 24+):
#   Links against the system FFmpeg development packages:
#     sudo apt install libavcodec-dev libavformat-dev libavutil-dev \
#                      libswscale-dev libswresample-dev pkg-config
#
# Disable file decoding entirely with -DMEDIAMAX_ENABLE_FFMPEG=OFF. The engine
# then still works with procedural sources (test patterns) and is fully testable.

if(NOT MEDIAMAX_ENABLE_FFMPEG)
    set(MEDIAMAX_HAS_FFMPEG FALSE)
    return()
endif()

set(MEDIAMAX_HAS_FFMPEG TRUE)

find_package(PkgConfig QUIET)

if(WIN32)
    set(_ffmpeg_dir "${MEDIAMAX_FFMPEG_DIR}")

    if(NOT _ffmpeg_dir)
        # Pinned prebuilt FFmpeg (FFmpeg 9.0, GPL, shared) for Windows x64.
        set(_ffmpeg_url "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-21-13-06/ffmpeg-n9.0.2-31-gc89d6ee1ca-win64-gpl-shared-9.0.zip")
        set(_ffmpeg_hash "SHA256=242f3b24335b04511de006594b685d19bc3a46538e086322beb5797f91554413")
        set(_ffmpeg_zip "${CMAKE_BINARY_DIR}/_deps/mediamax-ffmpeg.zip")
        set(_ffmpeg_extract "${CMAKE_BINARY_DIR}/_deps/mediamax-ffmpeg")

        if(NOT EXISTS "${_ffmpeg_extract}/lib/avcodec.lib")
            # A previous run may have extracted the archive but failed before
            # the rename; reuse it instead of re-downloading.
            file(GLOB _ffmpeg_root "${CMAKE_BINARY_DIR}/_deps/ffmpeg-n*-win64-gpl-shared*")
            if(NOT _ffmpeg_root)
                message(STATUS "Downloading FFmpeg prebuilt: ${_ffmpeg_url}")
                file(DOWNLOAD "${_ffmpeg_url}" "${_ffmpeg_zip}" EXPECTED_HASH "${_ffmpeg_hash}" STATUS _dl_status)
                list(GET _dl_status 0 _dl_code)
                if(NOT _dl_code EQUAL 0)
                    file(REMOVE "${_ffmpeg_zip}")
                    message(FATAL_ERROR
                        "Failed to download FFmpeg from ${_ffmpeg_url} (${_dl_status}). "
                        "Re-run configure with network access, or point -DMEDIAMAX_FFMPEG_DIR at an "
                        "extracted build (needs include/, lib/, bin/).")
                endif()
                file(REMOVE_RECURSE "${_ffmpeg_extract}")
                execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf "${_ffmpeg_zip}"
                    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/_deps"
                    RESULT_VARIABLE _unzip_result)
                if(NOT _unzip_result EQUAL 0)
                    message(FATAL_ERROR "Failed to extract FFmpeg archive")
                endif()
                # The archive extracts to a single top-level folder named
                # ffmpeg-<version>-<arch>-<license>[-<release>]; find it.
                file(GLOB _ffmpeg_root "${CMAKE_BINARY_DIR}/_deps/ffmpeg-n*-win64-gpl-shared*")
                if(NOT _ffmpeg_root)
                    message(FATAL_ERROR "FFmpeg archive did not extract as expected into ${CMAKE_BINARY_DIR}/_deps")
                endif()
            endif()
            list(GET _ffmpeg_root 0 _ffmpeg_root)
            file(RENAME "${_ffmpeg_root}" "${_ffmpeg_extract}")
            file(REMOVE "${_ffmpeg_zip}")
        endif()

        set(_ffmpeg_dir "${_ffmpeg_extract}")
    endif()

    # Header layout varies by build: include/avcodec/avcodec.h (flat) or
    # include/libavcodec/avcodec.h (lib-prefixed).
    if(NOT EXISTS "${_ffmpeg_dir}/include/avcodec/avcodec.h" AND
       NOT EXISTS "${_ffmpeg_dir}/include/libavcodec/avcodec.h")
        message(FATAL_ERROR
            "MEDIAMAX_FFMPEG_DIR (${_ffmpeg_dir}) does not contain an FFmpeg build. "
            "Expected layout: include/avcodec/avcodec.h (or include/libavcodec/avcodec.h), lib/*.lib, bin/*.dll")
    endif()

    # One imported target per library, umbrellaed by an INTERFACE target.
    # A single target whose IMPORTED_LOCATION lists several files is
    # serialized by CMake's Visual Studio generator as ONE percent-encoded
    # (%3B) entry in AdditionalDependencies; the MSVC linker mis-parses that
    # blob and fails with LNK1181 on a phantom '.obj'. Distinct targets keep
    # each import library a separate linker input.
    set(_ffmpeg_libs avcodec avformat avutil swscale swresample)
    set(_ffmpeg_link_libs "")
    foreach(_ffmpeg_lib IN LISTS _ffmpeg_libs)
        add_library(ffmpeg::${_ffmpeg_lib} UNKNOWN IMPORTED GLOBAL)
        set_target_properties(ffmpeg::${_ffmpeg_lib} PROPERTIES
            IMPORTED_LOCATION "${_ffmpeg_dir}/lib/${_ffmpeg_lib}.lib")
        list(APPEND _ffmpeg_link_libs "ffmpeg::${_ffmpeg_lib}")
    endforeach()

    add_library(ffmpeg::ffmpeg INTERFACE IMPORTED GLOBAL)
    set_target_properties(ffmpeg::ffmpeg PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_ffmpeg_dir}/include"
        INTERFACE_LINK_LIBRARIES "${_ffmpeg_link_libs}")

    # FFmpeg's shared build depends on runtime DLLs; copy them next to each
    # executable that links FFmpeg.
    file(GLOB _ffmpeg_dlls "${_ffmpeg_dir}/bin/*.dll")
    set(MEDIAMAX_FFMPEG_DLLS "${_ffmpeg_dlls}" CACHE INTERNAL "FFmpeg runtime DLLs to deploy")

    # The prebuilt ships the ffmpeg/ffprobe CLIs; the tests use them to
    # generate media files.
    set(MEDIAMAX_FFMPEG_TOOLS_DIR "${_ffmpeg_dir}/bin")
else()
    if(NOT PkgConfig_FOUND)
        message(FATAL_ERROR
            "Building MediaMax with FFmpeg on Linux requires pkg-config. "
            "Install it with: sudo apt install pkg-config")
    endif()

    pkg_check_modules(FFMPEG REQUIRED
        libavcodec
        libavformat
        libavutil
        libswscale
        libswresample)

    add_library(ffmpeg::ffmpeg UNKNOWN IMPORTED GLOBAL)
    set_target_properties(ffmpeg::ffmpeg PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIRS}"
        IMPORTED_LOCATION "${FFMPEG_LIBRARIES}")

    # The tests use the ffmpeg CLI to generate media files; on Linux it is
    # expected on PATH (libavtools is a separate package).
    find_program(MEDIAMAX_FFMPEG_TOOLS_EXE NAMES ffmpeg)
    if(MEDIAMAX_FFMPEG_TOOLS_EXE)
        get_filename_component(MEDIAMAX_FFMPEG_TOOLS_DIR "${MEDIAMAX_FFMPEG_TOOLS_EXE}" DIRECTORY)
    else()
        set(MEDIAMAX_FFMPEG_TOOLS_DIR "")
    endif()
endif()

set(MEDIAMAX_FFMPEG_LIBS "ffmpeg::ffmpeg")
