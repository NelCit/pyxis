// Pyxis app — stb_image_write implementation TU.
//
// stb_image_write.h's source has missing-field-initializer patterns that
// our /W4 + clang-tidy gate would otherwise reject. The header is
// third-party and we don't modify it; instead we ship its implementation
// here in its own TU so ViewerMode.cpp (and any future caller) can
// include the header normally.

// NOLINTNEXTLINE(readability-identifier-naming) -- macro name dictated by stb_image_write.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
