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
)

find_package(pxr CONFIG REQUIRED)

# USD's tf/type_Impl.h uses std::is_pod which is deprecated in C++20+.
# We compile against C++23 so MSVC's STL fires `STL4025` at every
# TF_REGISTRY_FUNCTION expansion. Silence the deprecation in this
# DLL only — the choice belongs to USD, not us.
target_compile_definitions(pyxis_hydra PRIVATE
    _SILENCE_CXX20_IS_POD_DEPRECATION_WARNING=1
)

target_link_libraries(pyxis_hydra
    PUBLIC
        Pyxis::Renderer
        Pyxis::MaterialTranslation     # Sync impls call FromUsdShade.
    PRIVATE
        # USD core + Hydra. The full set of Hd* / UsdImaging* targets the
        # delegate touches at M4. The plug / tf set are required for
        # the TF_REGISTRY_FUNCTION macro that hooks the plugin into
        # the Hd renderer-plugin registry.
        usd
        usdGeom
        usdLux
        usdShade
        sdf
        plug
        tf
        gf
        vt
        hd
        hdsi
        hf
        hio
)

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
