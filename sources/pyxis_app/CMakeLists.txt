# pyxis_app — single executable, viewer + headless. Plan §1 / §3 / §41.

add_executable(pyxis WIN32)
add_executable(Pyxis::App ALIAS pyxis)

# pyxis_app is the consumer of the renderer + platform DLLs; it links them
# PUBLIC so transitive consumers (the unit test executable) see the right
# include paths.
pyxis_set_target_compile_options(pyxis)
# The Application owns SceneWorldFacade::Init() which catches Flecs's
# bad_alloc internally; the renderer/platform perimeter is exception-free.
# pyxis_app proper has no need for exceptions either, so keep them off.
pyxis_disable_exceptions_and_rtti(pyxis)

target_sources(pyxis PRIVATE
    Main.cpp

    Private/Application.h
    Private/Application.cpp
    Private/CliArgs.h
    Private/CliArgs.cpp
    Private/HeadlessMode.h
    Private/HeadlessMode.cpp
)

target_include_directories(pyxis PRIVATE
    Private
)

target_link_libraries(pyxis PRIVATE
    Pyxis::Platform
    Pyxis::Renderer
)

# WIN32 + the standard /SUBSYSTEM:CONSOLE for our CLI tool. The WIN32 flag
# above defaults to /SUBSYSTEM:WINDOWS, which we override here so headless
# stdout/stderr print to the host console.
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set_target_properties(pyxis PROPERTIES
        WIN32_EXECUTABLE FALSE)
    target_link_options(pyxis PRIVATE /SUBSYSTEM:CONSOLE)
endif()

# install(TARGETS pyxis ... ) — wired in M11 alongside the install rules
# from §49.  M0 leaves it out so we don't accidentally `install` an
# incomplete tree.
