// Pyxis platform — DLL export macro.
//
// Plan §22.4 — every public class is decorated with PYXIS_PLATFORM_API.
// PYXIS_PLATFORM_BUILDING_DLL is set on the building target via
// pyxis_define_module() (see _cmake/Compiler.cmake); consumers see the
// PYXIS_PLATFORM_USING_DLL form which evaluates to __declspec(dllimport).

#pragma once

#if defined(_WIN32)
#  if defined(PYXIS_PLATFORM_BUILDING_DLL)
#    define PYXIS_PLATFORM_API __declspec(dllexport)
#  elif defined(PYXIS_PLATFORM_USING_DLL)
#    define PYXIS_PLATFORM_API __declspec(dllimport)
#  else
#    define PYXIS_PLATFORM_API
#  endif
#else
#  define PYXIS_PLATFORM_API
#endif
