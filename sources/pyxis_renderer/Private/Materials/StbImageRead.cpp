// Pyxis renderer — stb_image IMPLEMENTATION TU.
//
// stb_image is header-only — its IMPLEMENTATION macro must land in
// exactly one TU per binary or the linker reports duplicate
// symbols (decode helpers, jpeg DCT tables, etc.). This file is
// the single source of truth for pyxis_renderer.dll; GpuScene
// includes <stb_image.h> normally to call the API.
//
// pyxis_app has the matching stb_image_WRITE TU
// (Private/StbImageWrite.cpp) since the read + write halves of stb
// are independent header pairs. Both libs depending on stb is fine —
// they're separate DLLs with their own symbol scope.
//
// SKIP_LINTING in the CMake side because stb_image's
// missing-field-initializer pattern would otherwise trip /W4 /WX.

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
