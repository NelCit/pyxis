# pyxis_hydra — SHARED. Plan §3 / §7 / §25.
#
# Hydra render delegate ingest adapter. Loadable as a USD plugin so
# usdview / any HdEngine host can pick "Pyxis" as a renderer.
#
# /EHsc + /GR — USD throws + uses RTTI heavily. The §30.1
# exception-free perimeter applies to renderer/platform; this is an
# ingest-side lib that crosses into USD's world.

add_library(pyxis_hydra SHARED)
add_library(Pyxis::Hydra ALIAS pyxis_hydra)

pyxis_define_module(pyxis_hydra)
pyxis_enable_exceptions_and_rtti(pyxis_hydra)

target_sources(pyxis_hydra PUBLIC
    FILE_SET pyxis_hydra_public TYPE HEADERS
    BASE_DIRS Public
    FILES
        Public/Pyxis/Hydra/HydraApi.h
        Public/Pyxis/Hydra/HdPyxisRendererPlugin.h
)

target_sources(pyxis_hydra PRIVATE
    Private/HdPyxisRendererPlugin.cpp
    Private/HdPyxisRenderDelegate.h
    Private/HdPyxisRenderDelegate.cpp
    Private/HdPyxisRenderParam.h
    # RFC 0006: the delegate now OWNS its render engine (own Vulkan device +
    # GpuScene + PyxisRenderer + GpuInteropExporter), so the single delegate
    # source loads as a plugin in any HdEngine host (usdview / Omniverse Kit).
    # This is the engine the pyxis_hydra_omni Kit fork used to carry separately.
    Private/PyxisEngine.h
    Private/PyxisEngine.cpp
)

# RFC 0006: link Kit's nv-usd 25.11 (not vcpkg USD 26.3) so this delegate is the
# single source compiled for both the standalone host and Omniverse Kit. The full
# usd_* import set (incl. hd/hdsi/hf/usdImaging) + python312 + the std::is_pod
# silence + /EHsc /GR come from pyxis_use_nv_usd.
include(PyxisNvUsd)

target_link_libraries(pyxis_hydra
    PUBLIC
        Pyxis::Renderer
        Pyxis::MaterialTranslation     # Sync impls call FromUsdShade.
        Pyxis::UsdIngest               # §25.O.3 — ingest the stage via StageWalker (same as standalone).
    PRIVATE
        # RFC 0006: PyxisEngine stands up a Pyxis Vulkan device + GpuScene +
        # renderer + GpuInteropExporter. Platform links nvrhi PUBLIC, so the
        # nvrhi headers PyxisEngine.cpp needs ride along transitively.
        Pyxis::Platform
)
pyxis_use_nv_usd(pyxis_hydra)

# ----------------------------------------------------------------------------
# plugInfo.json — written next to pyxis_hydra.dll at build time so Hydra's
# plugin registry finds it. The registry walks every PXR_PLUGINPATH_NAME
# entry (an env var the host application sets) for `plugInfo.json` files
# and ingests them; we ship one keyed on `LibraryPath` pointing at the dll.
#
# `Root: ".."` makes LibraryPath relative to the *parent* of `resources/`,
# so `<bin>/Resources/usd/hdPyxis/resources/plugInfo.json` resolves
# `LibraryPath` relative to `<bin>/Resources/usd/hdPyxis/`. The dll lives
# at `<bin>/pyxis_hydra.dll`, so LibraryPath is `../../../pyxis_hydra.dll`.
# ----------------------------------------------------------------------------
set(PYXIS_HYDRA_PLUGIN_LIBPATH "../../../pyxis_hydra.dll")
set(_pyxis_hydra_plugin_dst
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/Resources/usd/hdPyxis/resources")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/plugInfo.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json"
    @ONLY)
add_custom_command(TARGET pyxis_hydra POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_pyxis_hydra_plugin_dst}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json"
        "${_pyxis_hydra_plugin_dst}/plugInfo.json"
    COMMENT "Copying pyxis_hydra plugInfo.json to <bin>/Resources/usd/hdPyxis/resources/")
unset(_pyxis_hydra_plugin_dst)
unset(PYXIS_HYDRA_PLUGIN_LIBPATH)
