// Pyxis renderer — DLL export macro. Plan §22.4.

#pragma once

#if defined(_WIN32)
#if defined(PYXIS_RENDERER_BUILDING_DLL)
#define PYXIS_RENDERER_API __declspec(dllexport)
#elif defined(PYXIS_RENDERER_USING_DLL)
#define PYXIS_RENDERER_API __declspec(dllimport)
#else
#define PYXIS_RENDERER_API
#endif
#else
#define PYXIS_RENDERER_API
#endif
