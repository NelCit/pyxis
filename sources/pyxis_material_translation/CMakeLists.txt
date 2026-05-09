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

# pxr targets: usd, usdShade, sdf, tf, vt, gf — needed for the
# UsdShadeMaterial walk + token math + Vec3 conversions.
find_package(pxr CONFIG REQUIRED)

# Same C++20 std::is_pod deprecation silence as pyxis_hydra — see
# the comment block there.
target_compile_definitions(pyxis_material_translation PRIVATE
    _SILENCE_CXX20_IS_POD_DEPRECATION_WARNING=1
)

target_link_libraries(pyxis_material_translation
    PUBLIC
        Pyxis::Renderer        # OpenPBRMaterialDesc lives in pyxis_renderer's Public/.
    PRIVATE
        usd
        usdShade
        sdf
        tf
        vt
        gf
)
