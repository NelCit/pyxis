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
    GpuSceneCamera.cpp
    SceneResolver.cpp
    UsdShadeToOpenPBR.cpp

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
)

target_include_directories(pyxis_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/sources/pyxis_renderer/Private
    ${CMAKE_SOURCE_DIR}/sources/pyxis_app/Private
)

# pyxis_unit_tests is the one place we *do* allow exceptions — gtest
# raises on EXPECT_THROW / FAIL so /EHsc is required even though our
# perimeter is exception-free.
pyxis_set_target_compile_options(pyxis_unit_tests)
pyxis_enable_exceptions_and_rtti(pyxis_unit_tests)

find_package(pxr CONFIG REQUIRED)

target_link_libraries(pyxis_unit_tests
    PRIVATE
        Pyxis::Platform
        Pyxis::Renderer
        Pyxis::MaterialTranslation
        usd
        usdShade
        sdf
        tf
        vt
        gf
        GTest::gtest
        GTest::gtest_main
)

# UsdShadeToOpenPBR.cpp pulls USD's headers which trip C++20's
# is_pod deprecation; same silence the ingest libs use.
target_compile_definitions(pyxis_unit_tests PRIVATE
    _SILENCE_CXX20_IS_POD_DEPRECATION_WARNING=1
)

include(GoogleTest)
gtest_discover_tests(pyxis_unit_tests
    DISCOVERY_MODE PRE_TEST)
