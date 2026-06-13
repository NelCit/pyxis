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
    GpuSlotMapFixture.cpp
    GpuSceneIncrementalUpload.cpp
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
    VariantSelectionParsing.cpp
    MaterialNetworkV2A18.cpp
    MaterialBindingResolutionV2A18.cpp
    AnimatedAttributesV2A13.cpp
    PurposeFilterSpecParsing.cpp
    PrescanRenderSettingsV2A27.cpp
    MaterialCoverageFields.cpp
    MaterialDescHash.cpp
    MdlTranslation.cpp
    UdimMultiTile.cpp
    MaterialXTranslation.cpp
    NurbsTessellation.cpp
    TextureLruEviction.cpp
    DdsParserFixture.cpp
    VolumeLoaderFixture.cpp
    SubdivCreaseFixture.cpp
    InstanceDoubleSided.cpp
    RenderSettingsLayout.cpp
    LightDescExtendedDefaults.cpp
    OrthographicCameraV2A20.cpp
    BcEncoderV2A14.cpp
    MeshUvPackV2.cpp
    GpuInteropRoundTrip.cpp
    PyxisRenderToExportedImage.cpp
    ColorEncodingFixture.cpp
    AovColorEncodeFixture.cpp
    TextureReadbackFixture.cpp

    # V2.A.14 — BCn encoder helper. Recompiled here so the test exe
    # exercises the real helper directly instead of going through the
    # pyxis_renderer DLL boundary (which would require widening the
    # §18.1 public surface).
    ${CMAKE_SOURCE_DIR}/sources/pyxis_renderer/Private/GpuScene/BcEncoder.cpp

    # Mesh UV tight-packing helper — recompiled here for the same reason
    # (MeshUvPackV2 pins the gMeshUvs 8-byte stride contract directly).
    ${CMAKE_SOURCE_DIR}/sources/pyxis_renderer/Private/GpuScene/MeshUvPack.cpp
    # stb_dxt.h's STB_DXT_IMPLEMENTATION lives in a dedicated TU
    # (mirrors the StbImageWrite.cpp pattern). Compiled here so the
    # test exe owns its own copy of the implementation symbols
    # without trying to import them from pyxis_renderer.dll.
    ${CMAKE_SOURCE_DIR}/sources/pyxis_renderer/Private/GpuScene/StbDxt.cpp

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

    # RFC 0009 — SceneWorldInit pins the §30.11 phase pipeline; Phases.cpp is a
    # Private symbol not exported from pyxis_renderer.dll, so compile it directly.
    ${CMAKE_SOURCE_DIR}/sources/pyxis_renderer/Private/Scene/Phases.cpp

    # SceneResolver lives in pyxis_app's Private (the executable, not
    # a DLL) so we recompile its .cpp directly here. CliArgs.h /
    # Configuration.h are pure POD declarations — we don't need to
    # drag the matching .cpp files in.
    ${CMAKE_SOURCE_DIR}/sources/pyxis_app/Private/Scene/SceneResolver.cpp

    # M19 — CliArgs.cpp recompiled directly so FrameFlagParsing tests
    # can call Parse() without linking against the pyxis_app exe.
    ${CMAKE_SOURCE_DIR}/sources/pyxis_app/Private/CliArgs.cpp

    # V2.A.2 — VariantParser is pure-stdlib; recompile directly so the
    # unit test exercises the real parser instead of a copy.
    ${CMAKE_SOURCE_DIR}/sources/pyxis_app/Private/VariantParser.cpp

    # M13 / V2.A.3 — analytic-prim tessellators live in
    # pyxis_usd_ingest/Private; recompile directly so the test exe
    # doesn't need to import them across the DLL boundary.
    ${CMAKE_SOURCE_DIR}/sources/pyxis_usd_ingest/Private/AnalyticGeom.cpp
)

target_include_directories(pyxis_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/sources/pyxis_renderer/Private
    ${CMAKE_SOURCE_DIR}/sources/pyxis_app/Private
    ${CMAKE_SOURCE_DIR}/sources/pyxis_usd_ingest/Private
    # PR6 — PurposeFilterSpecParsing.cpp + future ingest unit tests
    # include the public UsdIngest header directly.
    ${CMAKE_SOURCE_DIR}/sources/pyxis_usd_ingest/Public
    # AovColorEncodeFixture pins the delegate's header-only AOV encode policy
    # (Private/AovColorEncode.h) — the single source of truth for the
    # Omniverse⇄engine color-parity contract.
    ${CMAKE_SOURCE_DIR}/sources/pyxis_hydra/Private
    # Q1 — MaterialDescHash pins the §11 dedup hash
    # (Private/GpuScene/Detail.h), which includes the dual-language
    # ShaderInterop.slang the same way the renderer target does.
    ${CMAKE_SOURCE_DIR}/resources/shaders
)

# pyxis_unit_tests is the one place we *do* allow exceptions — gtest
# raises on EXPECT_THROW / FAIL so /EHsc is required even though our
# perimeter is exception-free.
pyxis_set_target_compile_options(pyxis_unit_tests)
pyxis_enable_exceptions_and_rtti(pyxis_unit_tests)

# RFC 0006: USD comes from Kit's nv-usd 25.11 (pyxis_use_nv_usd below), not vcpkg.
# pyxis_unit_tests recompiles SubdivRefine.cpp / VolumeLoader.cpp / UsdShadeToOpenPBR
# directly, so it needs the nv-usd include path + import libs in its own scope.
find_package(OpenVDB CONFIG REQUIRED)
include(PyxisNvUsd)

find_package(flecs CONFIG REQUIRED)  # RFC 0009 — GpuSlotMapFixture links flecs.
target_link_libraries(pyxis_unit_tests
    PRIVATE
        Pyxis::Platform
        Pyxis::Renderer
        Pyxis::MaterialTranslation
        # PR6 — PurposeFilterSpecParsing links against the public
        # `ParsePurposeFilterSpec` symbol shipped from the
        # pyxis_usd_ingest DLL.
        Pyxis::UsdIngest
        # V2.A.5 — VolumeLoaderFixture authors a synthetic .vdb at
        # test setup time + the recompiled VolumeLoader.cpp pulls
        # openvdb's read path. CPU only; no nanovdb feature.
        OpenVDB::openvdb
        # V2.A.1 follow-up — SubdivCreaseFixture recompiles SubdivRefine.cpp which
        # uses OpenSubdiv's Far layer. nv-usd's osdCPU.lib matches its bundled
        # v3_6_0 headers (RFC 0006) — same lib the pyxis_usd_ingest target links.
        "${PXR_USD_ROOT}/lib/osdCPU.lib"
        # RFC 0009 — GpuSlotMapFixture white-box-tests the Flecs-backed handle table
        # (Private/GpuScene/GpuSlotMap.h), which is header-only over flecs. The test
        # exe creates its own flecs::world (its own runtime; independent of the one
        # inside pyxis_renderer.dll), so it links flecs directly.
        $<TARGET_NAME_IF_EXISTS:flecs::flecs_static>
        $<TARGET_NAME_IF_EXISTS:flecs::flecs>
        GTest::gtest
        GTest::gtest_main
)
# nv-usd includes + the usd_*/python312 import libs (the test exe links
# UsdStage/Sdf/etc. via the recompiled ingest TUs). Exception/RTTI already on
# (pyxis_enable_exceptions_and_rtti above), so the full entry point is consistent.
pyxis_use_nv_usd(pyxis_unit_tests)

# V2.A.5 — VolumeLoader.cpp uses try/catch (openvdb's API is
# exception-based) which trips clang-tidy's frontend through the
# clang-cl driver. Same SKIP_LINTING escape the production target
# uses; the test TU compiling the same .cpp must skip too.
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/sources/pyxis_usd_ingest/Private/VolumeLoader.cpp
    PROPERTIES SKIP_LINTING ON)

# V2.A.14 — stb_dxt's STB_DXT_IMPLEMENTATION sentinel is a third-
# party macro name that clang-tidy's readability-identifier-naming
# trips on; same SKIP_LINTING escape the production renderer target
# uses.
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/sources/pyxis_renderer/Private/GpuScene/StbDxt.cpp
    PROPERTIES SKIP_LINTING ON)

# V2.A.14 — BcEncoder.cpp and the test sibling both #include <stb_dxt.h>.
# vcpkg's `Stb` config exports the include dir as Stb_INCLUDE_DIR (no
# link target). pyxis_renderer already pulls this in its own scope;
# the test exe needs it too because the recompiled BcEncoder.cpp lives
# here.
find_package(Stb REQUIRED)
target_include_directories(pyxis_unit_tests PRIVATE ${Stb_INCLUDE_DIR})

# UsdShadeToOpenPBR.cpp pulls USD's headers which trip C++20's
# is_pod deprecation; same silence the ingest libs use.
target_compile_definitions(pyxis_unit_tests PRIVATE
    _SILENCE_CXX20_IS_POD_DEPRECATION_WARNING=1
)

# RFC 0004 — GpuInteropRoundTrip.cpp drives raw Vulkan (Win32 external-memory
# import) on a second logical device. It needs the Win32 platform entry points,
# and its dense raw-Vulkan body trips clang-tidy's readability rules the same
# way VolumeLoader does, so it takes the documented SKIP_LINTING escape.
target_compile_definitions(pyxis_unit_tests PRIVATE VK_USE_PLATFORM_WIN32_KHR)
set_source_files_properties(GpuInteropRoundTrip.cpp PROPERTIES SKIP_LINTING ON)
# Half-float decode + nvrhi readback magic numbers trip tidy; same escape.
set_source_files_properties(PyxisRenderToExportedImage.cpp PROPERTIES SKIP_LINTING ON)

include(GoogleTest)
gtest_discover_tests(pyxis_unit_tests
    DISCOVERY_MODE PRE_TEST)
