# MediaMax control interface executable.
#
# Links the core only (no GPU, no audio): a thin console client for the
# control protocol (one-shot commands and an interactive REPL).

add_executable(mediamax_control
    src/main.cpp)
add_executable(mediamax::control ALIAS mediamax_control)

target_link_libraries(mediamax_control
    PRIVATE
        mediamax::mediamax)

mediamax_apply_compiler_settings(mediamax_control)
