# Pyxis Vulkan setup — plan §5 / §5.b.
#
# Locates a Vulkan SDK 1.3.x install (via the Khronos installer or the
# VULKAN_SDK env var) and exposes a single imported target `Pyxis::Vulkan`
# that aggregates Vulkan-Headers + Vulkan-Loader.
#
# Required Vulkan extensions / features at runtime are checked at device
# creation time (plan §5.b) and not at configure time — those checks live
# in pyxis_platform/Private/Vulkan/.

include_guard(GLOBAL)

find_package(Vulkan 1.3 REQUIRED COMPONENTS glslc)

if(NOT TARGET Pyxis::Vulkan)
    add_library(Pyxis::Vulkan INTERFACE IMPORTED GLOBAL)
    target_link_libraries(Pyxis::Vulkan INTERFACE Vulkan::Vulkan)
    target_include_directories(Pyxis::Vulkan INTERFACE ${Vulkan_INCLUDE_DIRS})
endif()

message(STATUS "Pyxis: Vulkan SDK at ${Vulkan_INCLUDE_DIRS} (version ${Vulkan_VERSION})")
