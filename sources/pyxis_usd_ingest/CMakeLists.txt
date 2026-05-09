# pyxis_usd_ingest — SHARED. Plan §3 / §25.O.
#
# Direct USD ingest adapter (no Hydra). One-shot importer:
# StageWalker reads UsdStage prim-by-prim and emits MeshDesc /
# InstanceDesc / OpenPBRMaterialDesc / LightDesc / CameraDesc into
# a supplied GpuScene via the public §18 API. The renderer never
# holds the UsdStage* afterwards (§25.O.2).
#
# /EHsc + /GR — same rationale as pyxis_hydra (USD throws + uses
# RTTI).

add_library(pyxis_usd_ingest SHARED)
add_library(Pyxis::UsdIngest ALIAS pyxis_usd_ingest)

pyxis_define_module(pyxis_usd_ingest)
pyxis_enable_exceptions_and_rtti(pyxis_usd_ingest)

target_sources(pyxis_usd_ingest PUBLIC
    FILE_SET pyxis_usd_ingest_public TYPE HEADERS
    BASE_DIRS Public
    FILES
        Public/Pyxis/UsdIngest/UsdIngestApi.h
        Public/Pyxis/UsdIngest/StageWalker.h
)

target_sources(pyxis_usd_ingest PRIVATE
    Private/StageWalker.cpp
)

find_package(pxr CONFIG REQUIRED)

# Same C++20 std::is_pod deprecation silence as pyxis_hydra — see
# the comment block there. USD's tf headers use the deprecated
# trait; the silence belongs in every TU that pulls them in.
target_compile_definitions(pyxis_usd_ingest PRIVATE
    _SILENCE_CXX20_IS_POD_DEPRECATION_WARNING=1
)

target_link_libraries(pyxis_usd_ingest
    PUBLIC
        Pyxis::Renderer
        Pyxis::MaterialTranslation
    PRIVATE
        usd
        usdGeom
        usdLux
        usdShade
        sdf
        tf
        gf
        vt
)
