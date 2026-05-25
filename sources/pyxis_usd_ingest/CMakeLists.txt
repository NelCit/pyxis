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
        Public/Pyxis/UsdIngest/RenderSettingsAuthored.h
)

target_sources(pyxis_usd_ingest PRIVATE
    Private/StageWalker.cpp
    Private/RenderSettingsScan.cpp
    Private/SubdivRefine.h
    Private/SubdivRefine.cpp
    Private/AnalyticGeom.h
    Private/AnalyticGeom.cpp
    Private/SkelSkinning.h
    Private/SkelSkinning.cpp
    Private/VolumeLoader.h
    Private/VolumeLoader.cpp
)

# VolumeLoader uses try/catch to translate openvdb's exception-based
# error reporting at the API boundary into Pyxis's Expected<T, std::string>
# tier. The compile flags (/EHsc /GR) are right, but clang-tidy's
# frontend invoked through `--driver-mode=cl` doesn't pick the /EHsc
# up the same way clang-cl does — it treats `try` as a syntax error.
# Skip linting on this single TU; the file is small + heavily reviewed.
# (Same escape valve used for Private/StbImageWrite.cpp in pyxis_app.)
set_source_files_properties(Private/VolumeLoader.cpp PROPERTIES
    SKIP_LINTING ON)

# RFC 0006: USD comes from Kit's nv-usd 25.11 (pyxis_use_nv_usd below), NOT vcpkg
# OpenUSD 26.3. mikktspace + OpenVDB stay vcpkg (they exchange raw vertex/voxel
# data, not USD types). OpenSubdiv must come from nv-usd, NOT vcpkg: nv-usd bundles
# its own OpenSubdiv headers (v3_6_0) which win on the include path, so the .obj
# references nv-usd's OpenSubdiv ABI — linking vcpkg's osd would be undefined
# symbols. nv-usd ships osdCPU.lib for exactly this.
find_package(mikktspace CONFIG REQUIRED)
find_package(OpenVDB CONFIG REQUIRED)
include(PyxisNvUsd)

# OpenMP — parallel mesh prep in StageWalker (§31 asset pool work).
#
# Use plain `/openmp`, NOT `/openmp:llvm`. On clang-cl the runtime is
# always libomp regardless of suffix (the `:llvm`/`:experimental`
# selectors are cl.exe-isms); but clang-cl SILENTLY IGNORES `/openmp:llvm`
# — it emits no libomp calls and does not define `_OPENMP`, so the bare
# `#pragma omp parallel for` in StageWalker compiled to a serial loop.
# `/openmp` actually enables OpenMP codegen and links libomp. It is a
# compile-only flag (`/openmp` is not a linker flag — passing it to
# lld-link fails); the obj's `/defaultlib:libomp` directive plus an
# explicit libomp.lib on the link line resolve the runtime, and
# libomp.dll is copied next to the binary. Derived from CMAKE_LINKER
# (`.../LLVM/bin/lld-link.exe`).
target_compile_options(pyxis_usd_ingest PRIVATE /openmp)
get_filename_component(_pyxis_llvm_bin  "${CMAKE_LINKER}"    DIRECTORY)
get_filename_component(_pyxis_llvm_root "${_pyxis_llvm_bin}" DIRECTORY)
target_link_libraries(pyxis_usd_ingest PRIVATE "${_pyxis_llvm_root}/lib/libomp.lib")
add_custom_command(TARGET pyxis_usd_ingest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_pyxis_llvm_bin}/libomp.dll" "$<TARGET_FILE_DIR:pyxis_usd_ingest>"
    COMMENT "Copying libomp.dll (OpenMP runtime) next to pyxis_usd_ingest"
    VERBATIM)

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
        mikktspace::mikktspace
        # OpenSubdiv CPU refinement (M12 / V2.A.1). nv-usd's osdCPU.lib (Far +
        # Sdc + Vtr layers) — matches the bundled v3_6_0 headers SubdivRefine.cpp
        # compiles against (RFC 0006). GPU eval backend (osdGPU) not linked.
        "${PXR_USD_ROOT}/lib/osdCPU.lib"
        # OpenVDB sparse-volume reader (V2.A.5). VolumeLoader opens
        # .vdb files referenced by UsdVolVolume / OpenVDBAsset prims
        # and flattens the active grid into a dense float voxel
        # buffer + bbox + transform. CPU only; the GPU 3D-texture
        # upload + shader binding live in pyxis_renderer. No
        # `nanovdb` feature on the port (would pull CUDA).
        OpenVDB::openvdb
)
pyxis_use_nv_usd(pyxis_usd_ingest)
