# Compiler settings shared by all first-party targets.
#
# MediaMax is a production codebase: first-party code is compiled with a
# strict warning set. Warnings are not fatal (third-party interaction and
# toolchain differences can produce false positives), but a clean build
# should have no warnings from first-party sources.

function(mediamax_apply_compiler_settings target)
    target_compile_features(${target} PUBLIC cxx_std_20)

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /bigobj)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wunused
            -Woverloaded-virtual
            -Wformat=2)
    endif()

    # Deterministic, sane preprocessor behavior.
    target_compile_definitions(${target} PRIVATE
        $<$<CONFIG:Debug>:_DEBUG>)

    # windows.h (via NVRHI and GLFW) defines min/max macros that break
    # std::min/std::max in the headers that include it.
    if(WIN32)
        target_compile_definitions(${target} PRIVATE NOMINMAX)
    endif()
endfunction()
