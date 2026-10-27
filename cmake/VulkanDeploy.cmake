# Vulkan loader deployment.
#
# Older SDK layouts ship a loader DLL in <sdk>/Bin; in that case we copy it
# next to each executable so it resolves without relying on the host PATH,
# and the dist target (cmake/Deploy.cmake) picks it up too. Newer SDK
# installations put the loader system-wide (System32) and ship no
# Bin/vulkan-1.dll at all; there is then nothing to deploy.

if(WIN32 AND Vulkan_LIBRARY)
    get_filename_component(_vk_sdk_root "${Vulkan_LIBRARY}" DIRECTORY)
    get_filename_component(_vk_sdk_root "${_vk_sdk_root}" DIRECTORY)
    if(EXISTS "${_vk_sdk_root}/Bin/vulkan-1.dll")
        set(MEDIAMAX_VULKAN_LOADER "${_vk_sdk_root}/Bin/vulkan-1.dll")
    else()
        message(STATUS
            "Vulkan SDK (${_vk_sdk_root}) ships no Bin/vulkan-1.dll; "
            "assuming the loader is installed system-wide (System32).")
    endif()
endif()

function(mediamax_deploy_vulkan_loader target)
    if(NOT MEDIAMAX_VULKAN_LOADER)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MEDIAMAX_VULKAN_LOADER}" "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Deploying the Vulkan loader")
endfunction()
