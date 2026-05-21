// Pyxis renderer — single-TU home for the stb_dxt.h implementation.
//
// stb_dxt is the public-domain BC1 / BC3 / BC4 / BC5 encoder shipped
// alongside stb_image via vcpkg's `Stb` config. It uses the standard
// stb single-header pattern: defining `STB_DXT_IMPLEMENTATION` once
// across the whole linked image emits the static definitions; every
// other TU pulling the header gets declarations only.
//
// Why a dedicated TU rather than #defining the macro inside
// `Commit.cpp` (the consumer)? clang-tidy's
// `readability-identifier-naming` flags `STB_DXT_IMPLEMENTATION` as
// a non-`PYXIS_SCREAM`-namespaced macro; NOLINTNEXTLINE doesn't
// suppress it (the diagnostic engine reports against the macro's
// token wherever it appears). Same pattern + same fix as
// `pyxis_app/Private/StbImageWrite.cpp`: ship a separate TU + add
// `SKIP_LINTING ON` on it in `CMakeLists.txt`.

#include <cstring>  // stb_dxt's implementation uses memcpy without including <string.h>

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>
