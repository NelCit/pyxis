# pyxis_material_translation — STATIC. Plan §3 / §11 / §25.E.
#
# UsdShadeMaterial / MaterialX / RenderMan → OpenPBRMaterialDesc on
# CPU. Used by both ingest adapters (pyxis_hydra +
# pyxis_usd_ingest); never linked by pyxis_renderer (the renderer
# stays USD-free per §1 / §30.3).
#
# /EHsc + /GR — USD's API throws and uses RTTI heavily, so this lib
# ships exceptions enabled (the §30.1 "perimeter is exception-free"
# rule applies to renderer/platform; this is an ingest-side lib that
# crosses into USD's world).

add_library(pyxis_material_translation STATIC)
add_library(Pyxis::MaterialTranslation ALIAS pyxis_material_translation)

pyxis_define_module(pyxis_material_translation)
pyxis_enable_exceptions_and_rtti(pyxis_material_translation)

target_sources(pyxis_material_translation PUBLIC
    FILE_SET pyxis_material_translation_public TYPE HEADERS
    BASE_DIRS Public
    FILES
        Public/Pyxis/MaterialTranslation/UsdShadeToOpenPBR.h
)

target_sources(pyxis_material_translation PRIVATE
    Private/UsdShadeToOpenPBR.cpp
)

# RFC 0006: link Kit's nv-usd 25.11 (not vcpkg USD 26.3) so the same ingest +
# delegate sources build for both the standalone host and Omniverse Kit. The
# usd_* import libs + python312 + the std::is_pod silence + /EHsc /GR all come
# from pyxis_use_nv_usd.
include(PyxisNvUsd)

target_link_libraries(pyxis_material_translation
    PUBLIC
        Pyxis::Renderer        # OpenPBRMaterialDesc lives in pyxis_renderer's Public/.
)
pyxis_use_nv_usd(pyxis_material_translation)
