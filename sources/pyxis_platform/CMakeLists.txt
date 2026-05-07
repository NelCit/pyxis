# pyxis_platform — plan §1 / §3 / §5.c.
#
# SHARED so spdlog/Tracy registries are single-instance across all DLLs
# (plan §33.10).

add_library(pyxis_platform SHARED)
add_library(Pyxis::Platform ALIAS pyxis_platform)

pyxis_define_module(pyxis_platform)
pyxis_disable_exceptions_and_rtti(pyxis_platform)

# ----------------------------------------------------------------------------
# Public headers (narrow surface — plan §1, plus Device + Logging + FileSystem
# slices required at M0 per §41).
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

    Private/Logging/SpdlogBackend.h
    Private/Logging/SpdlogBackend.cpp

    Private/FileSystem/AssetLocator.cpp
    Private/FileSystem/Path.cpp
)

# ----------------------------------------------------------------------------
# Dependencies.
# ----------------------------------------------------------------------------
find_package(spdlog CONFIG REQUIRED)

target_link_libraries(pyxis_platform
    PUBLIC
        # Vulkan-Headers ride along with the device-manager interface so
        # consumers inheriting IDeviceManager don't need to refind Vulkan.
        Pyxis::Vulkan
    PRIVATE
        spdlog::spdlog
)

# Windows libs needed for FormatMessage / WideCharToMultiByte on the Path
# helpers and SetUnhandledExceptionFilter on the future crash reporter.
target_link_libraries(pyxis_platform PRIVATE Dbghelp Shlwapi)

# Versioning (§22.2): the version symbols live in pyxis_renderer.dll
# (Version.h is decorated PYXIS_RENDERER_API). pyxis_platform doesn't
# generate anything here.
