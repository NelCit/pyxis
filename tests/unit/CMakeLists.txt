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
)

# pyxis_unit_tests is the one place we *do* allow exceptions — gtest
# raises on EXPECT_THROW / FAIL so /EHsc is required even though our
# perimeter is exception-free.
pyxis_set_target_compile_options(pyxis_unit_tests)
pyxis_enable_exceptions_and_rtti(pyxis_unit_tests)

target_link_libraries(pyxis_unit_tests
    PRIVATE
        Pyxis::Platform
        Pyxis::Renderer
        GTest::gtest
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(pyxis_unit_tests
    DISCOVERY_MODE PRE_TEST)
