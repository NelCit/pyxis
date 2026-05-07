# Pyxis custom vcpkg triplet — plan §33.10 / §49.
#
# Most ports use the default x64-windows-static linkage so we ship a single
# pyxis_renderer.dll without a forest of third-party DLLs. spdlog and Tracy
# are explicitly DYNAMIC because the §33.10 cross-DLL logging contract
# requires a single spdlog registry shared between pyxis_platform.dll,
# pyxis_renderer.dll, pyxis_hydra.dll and pyxis_usd_ingest.dll. Same logic
# applies to Tracy's process-global client.
#
# CI verifies the linkage rule with two dumpbin checks (plan §33.10):
#   dumpbin /exports pyxis_platform.dll | findstr spdlog   # must succeed
#   dumpbin /exports pyxis_renderer.dll | findstr spdlog   # must be empty

set(VCPKG_TARGET_ARCHITECTURE   x64)
set(VCPKG_CRT_LINKAGE           dynamic)
set(VCPKG_LIBRARY_LINKAGE       static)

if(PORT MATCHES "^(spdlog|tracy)$")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()

# Pyxis builds against the Universal CRT; pin the platform toolset.
set(VCPKG_CMAKE_SYSTEM_NAME     "")
set(VCPKG_PLATFORM_TOOLSET      v143)
