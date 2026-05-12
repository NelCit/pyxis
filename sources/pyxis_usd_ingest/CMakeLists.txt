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
    Private/SubdivRefine.h
    Private/SubdivRefine.cpp
    Private/AnalyticGeom.h
    Private/AnalyticGeom.cpp
)

find_package(pxr CONFIG REQUIRED)
find_package(mikktspace CONFIG REQUIRED)
find_package(OpenSubdiv CONFIG REQUIRED)

# OpenMP — used for parallel mesh prep in StageWalker. Use the LLVM
# runtime (`/openmp:llvm`) rather than MSVC's legacy `/openmp` since
# clang-cl ships with libomp and supports the modern OpenMP 4.5+
# pragmas (the legacy MSVC runtime is OpenMP 2.0). Both Debug and
# Release link against the same libomp.dll.
target_compile_options(pyxis_usd_ingest PRIVATE -openmp:llvm)
target_link_options(pyxis_usd_ingest    PRIVATE -openmp:llvm)

# MikkTSpace is a tiny third-party C library (~600 lines) that we hit
# very hard during ingest — once per sub-mesh, with N internal qsorts
# and N tangent computations. vcpkg ships an unoptimised Debug build
# (`mikktspaced.lib`, /Od) that runs ~10× slower than its Release
# variant; on the World Lobby that turned 10s of MikkTSpace work
# into 130s of MikkTSpace work in our Debug builds.
#
# Override the CONFIG-imported target's IMPORTED_LOCATION_DEBUG to
# point at the Release variant. Mikktspace has no debug-only state
# (pure C, no TLS, no debug iterators) so this is safe — the
# Release lib runs in a Debug app without complaint, and we keep
# our own ingest TU in Debug for breakpoint debugging.
get_target_property(_pyxis_mikk_release_lib mikktspace::mikktspace IMPORTED_LOCATION_RELEASE)
if(_pyxis_mikk_release_lib)
    set_target_properties(mikktspace::mikktspace PROPERTIES
        IMPORTED_LOCATION_DEBUG "${_pyxis_mikk_release_lib}")
    message(STATUS "Pyxis: forcing Release-built mikktspace.lib in Debug "
                   "(${_pyxis_mikk_release_lib}) — ~10× ingest speedup vs "
                   "vcpkg's mikktspaced.lib.")
endif()

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
        ar
        usd
        usdGeom
        usdLux
        usdRender
        usdShade
        usdSkel
        usdVol
        sdf
        tf
        gf
        vt
        mikktspace::mikktspace
        # OpenSubdiv CPU refinement (M12 / V2.A.1). The osdCPU_static
        # target ships the Far + Sdc + Vtr layers; GPU evaluation
        # backends (osdGPU_static) intentionally not linked — we
        # refine on the asset I/O pool at ingest time, not on the GPU.
        OpenSubdiv::osdCPU_static
)
