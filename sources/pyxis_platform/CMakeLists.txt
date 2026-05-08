# pyxis_platform — plan §1 / §3 / §5.c.
#
# SHARED so spdlog/Tracy registries are single-instance across all DLLs
# (plan §33.10).

# Pull NVRHI before the target is defined; the require_* helper handles
# clang-tidy + /WX neutralisation for the third-party tree.
pyxis_thirdparty_require_nvrhi()

add_library(pyxis_platform SHARED)
add_library(Pyxis::Platform ALIAS pyxis_platform)

pyxis_define_module(pyxis_platform)
pyxis_disable_exceptions_and_rtti(pyxis_platform)

# ----------------------------------------------------------------------------
# Public headers (plan §1: Device + Logging + FileSystem at M0; +Window at M1).
# ----------------------------------------------------------------------------
target_sources(pyxis_platform PUBLIC
    FILE_SET pyxis_platform_public TYPE HEADERS
    BASE_DIRS Public
    FILES
        Public/Pyxis/Platform/Forward.h
        Public/Pyxis/Platform/PlatformApi.h

        Public/Pyxis/Platform/Device/AdapterInfo.h
        Public/Pyxis/Platform/Device/DeviceCreationParams.h
        Public/Pyxis/Platform/Device/IDeviceManager.h
        Public/Pyxis/Platform/Device/Resolution.h

        Public/Pyxis/Platform/Logging/Log.h
        Public/Pyxis/Platform/Logging/LogCategories.h
        Public/Pyxis/Platform/Logging/LogConfig.h

        Public/Pyxis/Platform/FileSystem/AssetLocator.h
        Public/Pyxis/Platform/FileSystem/Path.h

        Public/Pyxis/Platform/Window/IWindow.h
        Public/Pyxis/Platform/Window/WindowDesc.h
        Public/Pyxis/Platform/Window/InputEvent.h
)

# ----------------------------------------------------------------------------
# Private implementation.
# ----------------------------------------------------------------------------
target_sources(pyxis_platform PRIVATE
    Private/Device/VkDeviceManager.h
    Private/Device/VkDeviceManager.cpp
    Private/Device/VkDeviceManagerHeadless.h
    Private/Device/VkDeviceManagerHeadless.cpp
    Private/Device/VulkanFeatureCheck.h
    Private/Device/VulkanFeatureCheck.cpp
    Private/Device/VulkanHppStorage.cpp

    Private/Logging/SpdlogBackend.h
    Private/Logging/SpdlogBackend.cpp

    Private/FileSystem/AssetLocator.cpp
    Private/FileSystem/Path.cpp

    Private/Window/GlfwWindow.h
    Private/Window/GlfwWindow.cpp
)

# ----------------------------------------------------------------------------
# Dependencies.
# ----------------------------------------------------------------------------
find_package(spdlog CONFIG REQUIRED)
find_package(glfw3  CONFIG REQUIRED)

target_link_libraries(pyxis_platform
    PUBLIC
        # Vulkan-Headers ride along with the device-manager interface so
        # consumers inheriting IDeviceManager don't need to refind Vulkan.
        Pyxis::Vulkan
        # NVRHI is a public dependency because IDeviceManager exposes
        # `nvrhi::IDevice*` and `nvrhi::ITexture*` in its public headers.
        nvrhi
        nvrhi_vk
    PRIVATE
        spdlog::spdlog
        glfw
)

# Windows libs needed for FormatMessage / WideCharToMultiByte on the Path
# helpers and SetUnhandledExceptionFilter on the future crash reporter.
target_link_libraries(pyxis_platform PRIVATE Dbghelp Shlwapi)

# GLFW_INCLUDE_VULKAN is defined here, not via #define in GlfwWindow.cpp
# (the all-caps macro otherwise trips clang-tidy's identifier-naming
# rule which expects non-PYXIS_ macros not to exist).
target_compile_definitions(pyxis_platform PRIVATE GLFW_INCLUDE_VULKAN)
