// Pyxis USD-direct ingest — DLL export macro.

#pragma once

#if defined(PYXIS_USD_INGEST_BUILDING_DLL)
#  define PYXIS_USD_INGEST_API __declspec(dllexport)
#elif defined(PYXIS_USD_INGEST_USING_DLL)
#  define PYXIS_USD_INGEST_API __declspec(dllimport)
#else
#  define PYXIS_USD_INGEST_API
#endif
