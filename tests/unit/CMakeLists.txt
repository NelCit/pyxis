# pyxis_unit_tests — gtest harness. Plan §35 / §36.
#
# M0 ships SceneWorldInit (the §41 M0 exit-criterion test). Future tests
# add: HandleBimap generation roundtrip, OpenPBR conversion fields, math
# row-major byte-equality, JSON round-trip per POD, etc.

find_package(GTest CONFIG REQUIRED)

add_executable(pyxis_unit_tests)
add_executable(Pyxis::UnitTests ALIAS pyxis_unit_tests)

# Unit tests link the same DLLs the application uses; they run in the
# real renderer process, never against a faked-up shim.
target_sources(pyxis_unit_tests PRIVATE
    SceneWorldInit.cpp
    RenderGraphSmoke.cpp
    ErrorAndExpected.cpp
    PublicDescLayout.cpp
    GpuSceneMesh.cpp
    GpuSceneInstance.cpp
    GpuSceneLight.cpp
    GpuSceneVolume.cpp
    GpuSceneCamera.cpp
    GpuSceneClear.cpp
    GpuSceneLookupInstanceMaterial.cpp
    SceneResolver.cpp
    UsdShadeToOpenPBR.cpp
    AnalyticGeomTessellation.cpp
    XformOpsComposition.cpp
    UdimAndInvisibleIds.cpp
    SceneBoundsAggregate.cpp
    UsdVolDetection.cpp
    TextureMemoryReporting.cpp
    ArResolverVisibility.cpp
    MaterialSourceCounts.cpp
    FrameFlagParsing.cpp
    NurbsSkelDetection.cpp
    UsdRenderDetection.cpp
    LoadModeAndMaskParsing.cpp
    NetworkCoverageDetection.cpp
    PopulationMaskParsing.cpp
    MaterialCoverageFields.cpp
    MdlTranslation.cpp
    UdimMultiTile.cpp
    MaterialXTranslation.cpp
    NurbsTessellation.cpp
    TextureLruEviction.cpp
    DdsParserFixture.cpp
    VolumeLoaderFixture.cpp
    SubdivCreaseFixture.cpp
    InstanceDoubleSided.cpp

    # V2.A.1 follow-up — SubdivRefine.cpp lives in pyxis_usd_ingest
    # Private. The fixture compiles it directly.
    ${CMAKE_SOURCE_DIR}/sources/pyxis_usd_ingest/Private/SubdivRefine.cpp

    # V2.A.14 DDS parser lives in pyxis_renderer's Private. The unit
    # test compiles it directly (same convention as RenderGraph + CliArgs).
    ${CMAKE_SOURCE_DIR}/sources/pyxis_renderer/Private/GpuScene/DdsParser.cpp

    # V2.A.5 VolumeLoader lives in pyxis_usd_ingest's Private. The
    # unit test compiles it directly so it doesn't need to import the
    # symbol across the DLL boundary. SKIP_LINTING the test sibling
    # too (try/catch trips clang-tidy's frontend the same way; the
    # production TU already has it skipped).
    ${CMAKE_SOURCE_DIR}/sources/pyxis_usd_ingest/Private/VolumeLoader.cpp

    # The §9.2 RenderGraph + IRenderPass live in pyxis_renderer/Private
    # and don't cross the SHARED-DLL boundary. The unit-test harness is
    # the documented §35 exception allowed to peek into Private/; we
    # compile the .cpp directly into the test executable so the linker
    # resolves the symbols locally instead of trying to import them
    # from pyxis_renderer.dll.
    ${CMAKE_SOURCE_DIR}/sources/pyxis_renderer/Private/RenderGraph/RenderGraph.cpp

    # SceneResolver lives in pyxis_app's Private (the executable, not
    # a DLL) so we recompile its .cpp directly here. CliArgs.h /
    # Configuration.h are pure POD declarations — we don't need to
    # drag the matching .cpp files in.
    ${CMAKE_SOURCE_DIR}/sources/pyxis_app/Private/Scene/SceneResolver.cpp

    # M19 — CliArgs.cpp recompiled directly so FrameFlagParsing tests
    # can call Parse() without linking against the pyxis_app exe.
    ${CMAKE_SOURCE_DIR}/sources/pyxis_app/Private/CliArgs.cpp

    # M13 / V2.A.3 — analytic-prim tessellators live in
    # pyxis_usd_ingest/Private; recompile directly so the test exe
    # doesn't need to import them across the DLL boundary.
    ${CMAKE_SOURCE_DIR}/sources/pyxis_usd_ingest/Private/AnalyticGeom.cpp
)

target_include_directories(pyxis_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/sources/pyxis_renderer/Private
    ${CMAKE_SOURCE_DIR}/sources/pyxis_app/Private
    ${CMAKE_SOURCE_DIR}/sources/pyxis_usd_ingest/Private
)

# pyxis_unit_tests is the one place we *do* allow exceptions — gtest
# raises on EXPECT_THROW / FAIL so /EHsc is required even though our
# perimeter is exception-free.
pyxis_set_target_compile_options(pyxis_unit_tests)
pyxis_enable_exceptions_and_rtti(pyxis_unit_tests)

find_package(pxr CONFIG REQUIRED)
find_package(OpenVDB CONFIG REQUIRED)
find_package(OpenSubdiv CONFIG REQUIRED)

target_link_libraries(pyxis_unit_tests
    PRIVATE
        Pyxis::Platform
        Pyxis::Renderer
        Pyxis::MaterialTranslation
        usd
        usdGeom
        usdShade
        usdRender
        usdSkel
        usdVol
        ar
        sdf
        tf
        vt
        gf
        # V2.A.5 — VolumeLoaderFixture authors a synthetic .vdb at
        # test setup time + the recompiled VolumeLoader.cpp pulls
        # openvdb's read path. CPU only; no nanovdb feature.
        OpenVDB::openvdb
        # V2.A.1 follow-up — SubdivCreaseFixture recompiles
        # SubdivRefine.cpp which uses OpenSubdiv's Far layer.
        OpenSubdiv::osdCPU_static
        GTest::gtest
        GTest::gtest_main
)

# V2.A.5 — VolumeLoader.cpp uses try/catch (openvdb's API is
# exception-based) which trips clang-tidy's frontend through the
# clang-cl driver. Same SKIP_LINTING escape the production target
# uses; the test TU compiling the same .cpp must skip too.
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/sources/pyxis_usd_ingest/Private/VolumeLoader.cpp
    PROPERTIES SKIP_LINTING ON)

# UsdShadeToOpenPBR.cpp pulls USD's headers which trip C++20's
# is_pod deprecation; same silence the ingest libs use.
target_compile_definitions(pyxis_unit_tests PRIVATE
    _SILENCE_CXX20_IS_POD_DEPRECATION_WARNING=1
)

include(GoogleTest)
gtest_discover_tests(pyxis_unit_tests
    DISCOVERY_MODE PRE_TEST)
