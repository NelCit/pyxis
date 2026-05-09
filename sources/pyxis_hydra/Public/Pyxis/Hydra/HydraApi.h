// Pyxis Hydra — DLL export macro.
//
// Mirrors the pattern in Pyxis/Renderer/RendererApi.h. pyxis_hydra is
// SHARED so usdview can dynamically load it; the plugin entry point
// is dllexported, everything else stays internal.

#pragma once

#if defined(PYXIS_HYDRA_BUILDING_DLL)
#  define PYXIS_HYDRA_API __declspec(dllexport)
#elif defined(PYXIS_HYDRA_USING_DLL)
#  define PYXIS_HYDRA_API __declspec(dllimport)
#else
#  define PYXIS_HYDRA_API
#endif
