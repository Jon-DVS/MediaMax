# Vulkan loader deployment.
#
# Older SDK layouts ship a loader DLL in <sdk>/Bin; in that case we copy it
# next to each executable so it resolves without relying on the host PATH.
# Newer SDK installations put the loader system-wide (System32) and ship no
# Bin/vulkan-1.dll at all; there is then nothing to deploy.

function(mediamax_deploy_vulkan_loader target)
    if(NOT WIN32 OR NOT Vulkan_LIBRARY)
        return()
    endif()

    get_filename_component(_vk_sdk_root "${Vulkan_LIBRARY}" DIRECTORY)
    get_filename_component(_vk_sdk_root "${_vk_sdk_root}" DIRECTORY)
    set(_vk_loader "${_vk_sdk_root}/Bin/vulkan-1.dll")

    if(EXISTS "${_vk_loader}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_vk_loader}" "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Deploying the Vulkan loader")
    else()
        message(STATUS
            "${target}: Vulkan SDK (${_vk_sdk_root}) ships no Bin/vulkan-1.dll; "
            "assuming the loader is installed system-wide (System32).")
    endif()
endfunction()
