// Pyxis Hydra — Vulkan→GL external-memory interop (RFC 0008). See GlVkInterop.h.

#include "GlVkInterop.h"

#include <windows.h>

#include <cstdio>

namespace pyxis::hydra {

namespace {

// --- Minimal GL types / constants (avoid a glext.h dependency) ---------------
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLuint64 = unsigned long long;

using GLbitfield = unsigned int;

constexpr GLenum TEXTURE_2D = 0x0DE1;
constexpr GLenum RGBA16F = 0x881A;
constexpr GLenum HANDLE_TYPE_OPAQUE_WIN32 = 0x9587;
constexpr GLenum TEXTURE_TILING = 0x9580;
constexpr GLenum OPTIMAL_TILING = 0x9584;
constexpr GLenum LINEAR_TILING = 0x9585;
constexpr GLenum COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLbitfield COLOR_BUFFER_BIT = 0x00004000;
constexpr GLenum NEAREST = 0x2600;

using PFNglCreateTextures = void(__stdcall*)(GLenum, GLsizei, GLuint*);
using PFNglDeleteTextures = void(__stdcall*)(GLsizei, const GLuint*);
using PFNglCreateMemoryObjectsEXT = void(__stdcall*)(GLsizei, GLuint*);
using PFNglDeleteMemoryObjectsEXT = void(__stdcall*)(GLsizei, const GLuint*);
using PFNglImportMemoryWin32HandleEXT = void(__stdcall*)(GLuint, GLuint64, GLenum, void*);
using PFNglTextureParameteri = void(__stdcall*)(GLuint, GLenum, GLint);
using PFNglTextureStorageMem2DEXT =
    void(__stdcall*)(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
using PFNglCreateFramebuffers = void(__stdcall*)(GLsizei, GLuint*);
using PFNglDeleteFramebuffers = void(__stdcall*)(GLsizei, const GLuint*);
using PFNglNamedFramebufferTexture = void(__stdcall*)(GLuint, GLenum, GLuint, GLint);
using PFNglBlitNamedFramebuffer = void(__stdcall*)(GLuint, GLuint, GLint, GLint, GLint, GLint, GLint,
                                                   GLint, GLint, GLint, GLbitfield, GLenum);
using PFNglGetError = GLenum(__stdcall*)();

// Process-global GL entry points (Kit has a single GL context; load once).
struct GlFns {
  PFNglCreateTextures createTextures = nullptr;
  PFNglDeleteTextures deleteTextures = nullptr;
  PFNglCreateMemoryObjectsEXT createMemoryObjects = nullptr;
  PFNglDeleteMemoryObjectsEXT deleteMemoryObjects = nullptr;
  PFNglImportMemoryWin32HandleEXT importMemoryWin32Handle = nullptr;
  PFNglTextureParameteri textureParameteri = nullptr;
  PFNglTextureStorageMem2DEXT textureStorageMem2D = nullptr;
  PFNglCreateFramebuffers createFramebuffers = nullptr;
  PFNglDeleteFramebuffers deleteFramebuffers = nullptr;
  PFNglNamedFramebufferTexture namedFramebufferTexture = nullptr;
  PFNglBlitNamedFramebuffer blitNamedFramebuffer = nullptr;
  PFNglGetError getError = nullptr;
};
GlFns g_fns;
int g_loadState = 0;  // 0 = untried, 1 = loaded, 2 = failed

[[nodiscard]] bool LoadOnce() noexcept {
  if (g_loadState == 1)
    return true;
  if (g_loadState == 2)
    return false;
  HMODULE glModule = ::GetModuleHandleW(L"opengl32.dll");
  if (glModule == nullptr)
    glModule = ::LoadLibraryW(L"opengl32.dll");
  if (glModule == nullptr) {
    std::fprintf(stderr, "GlVkInterop: opengl32.dll not loaded; GL interop unavailable\n");
    g_loadState = 2;
    return false;
  }
  using PFNwglGetProcAddress = PROC(__stdcall*)(LPCSTR);
  auto wglGetProc =
      reinterpret_cast<PFNwglGetProcAddress>(::GetProcAddress(glModule, "wglGetProcAddress"));
  if (wglGetProc == nullptr) {
    g_loadState = 2;
    return false;
  }
  auto loadExt = [&](const char* name) -> void* {
    return reinterpret_cast<void*>(wglGetProc(name));
  };
  auto load11 = [&](const char* name) -> void* {
    return reinterpret_cast<void*>(::GetProcAddress(glModule, name));
  };
  g_fns.createTextures = reinterpret_cast<PFNglCreateTextures>(loadExt("glCreateTextures"));
  g_fns.deleteTextures = reinterpret_cast<PFNglDeleteTextures>(load11("glDeleteTextures"));
  g_fns.createMemoryObjects =
      reinterpret_cast<PFNglCreateMemoryObjectsEXT>(loadExt("glCreateMemoryObjectsEXT"));
  g_fns.deleteMemoryObjects =
      reinterpret_cast<PFNglDeleteMemoryObjectsEXT>(loadExt("glDeleteMemoryObjectsEXT"));
  g_fns.importMemoryWin32Handle =
      reinterpret_cast<PFNglImportMemoryWin32HandleEXT>(loadExt("glImportMemoryWin32HandleEXT"));
  g_fns.textureParameteri = reinterpret_cast<PFNglTextureParameteri>(loadExt("glTextureParameteri"));
  g_fns.textureStorageMem2D =
      reinterpret_cast<PFNglTextureStorageMem2DEXT>(loadExt("glTextureStorageMem2DEXT"));
  g_fns.createFramebuffers =
      reinterpret_cast<PFNglCreateFramebuffers>(loadExt("glCreateFramebuffers"));
  g_fns.deleteFramebuffers =
      reinterpret_cast<PFNglDeleteFramebuffers>(loadExt("glDeleteFramebuffers"));
  g_fns.namedFramebufferTexture =
      reinterpret_cast<PFNglNamedFramebufferTexture>(loadExt("glNamedFramebufferTexture"));
  g_fns.blitNamedFramebuffer =
      reinterpret_cast<PFNglBlitNamedFramebuffer>(loadExt("glBlitNamedFramebuffer"));
  g_fns.getError = reinterpret_cast<PFNglGetError>(load11("glGetError"));

  if (!g_fns.createTextures || !g_fns.deleteTextures || !g_fns.createMemoryObjects
      || !g_fns.deleteMemoryObjects || !g_fns.importMemoryWin32Handle || !g_fns.textureParameteri
      || !g_fns.textureStorageMem2D || !g_fns.createFramebuffers || !g_fns.deleteFramebuffers
      || !g_fns.namedFramebufferTexture || !g_fns.blitNamedFramebuffer) {
    std::fprintf(stderr, "GlVkInterop: required GL 4.5 / EXT_memory_object_win32 entry points "
                         "missing; falling back to CPU readback\n");
    g_loadState = 2;
    return false;
  }
  g_loadState = 1;
  std::fprintf(stderr, "GlVkInterop: GL external-memory interop loaded (RFC 0008 direct AOV)\n");
  return true;
}

}  // namespace

GlVkInterop::~GlVkInterop() { ReleaseImport(); }

bool GlVkInterop::EnsureLoaded() noexcept {
  _loaded = LoadOnce();
  return _loaded;
}

uint32_t GlVkInterop::ImportExportedImage(void* win32Handle, uint64_t allocationSize,
                                          uint32_t width, uint32_t height,
                                          bool optimalTiling) noexcept {
  if (!EnsureLoaded() || win32Handle == nullptr || width == 0 || height == 0)
    return 0;
  if (_srcTexture != 0 && _importedHandle == win32Handle && _width == width && _height == height)
    return _srcTexture;
  ReleaseImport();

  g_fns.createMemoryObjects(1, &_memoryObject);
  // The exporter owns + closes the original Win32 handle; GL imports a reference.
  g_fns.importMemoryWin32Handle(_memoryObject, allocationSize, HANDLE_TYPE_OPAQUE_WIN32,
                                win32Handle);
  g_fns.createTextures(TEXTURE_2D, 1, &_srcTexture);
  // Tiling MUST match the Vulkan image (optimal for our exported color), set before
  // storage, or GL reads detiled garbage.
  g_fns.textureParameteri(_srcTexture, TEXTURE_TILING,
                          optimalTiling ? static_cast<GLint>(OPTIMAL_TILING)
                                        : static_cast<GLint>(LINEAR_TILING));
  g_fns.textureStorageMem2D(_srcTexture, 1, RGBA16F, static_cast<GLsizei>(width),
                            static_cast<GLsizei>(height), _memoryObject, 0);

  if (g_fns.getError != nullptr) {
    const GLenum err = g_fns.getError();
    if (err != 0) {
      std::fprintf(stderr, "GlVkInterop: GL error 0x%x importing exported image\n", err);
      ReleaseImport();
      return 0;
    }
  }
  _importedHandle = win32Handle;
  _width = width;
  _height = height;
  return _srcTexture;
}

bool GlVkInterop::CopyImportedInto(uint32_t dstGlTexture, uint32_t width, uint32_t height) noexcept {
  if (!_loaded || _srcTexture == 0 || dstGlTexture == 0)
    return false;
  if (_readFbo == 0)
    g_fns.createFramebuffers(1, &_readFbo);
  if (_drawFbo == 0)
    g_fns.createFramebuffers(1, &_drawFbo);
  g_fns.namedFramebufferTexture(_readFbo, COLOR_ATTACHMENT0, _srcTexture, 0);
  g_fns.namedFramebufferTexture(_drawFbo, COLOR_ATTACHMENT0, dstGlTexture, 0);
  const auto dimX = static_cast<GLint>(width);
  const auto dimY = static_cast<GLint>(height);
  // Source rows 0..h map to destination rows h..0 — the vertical flip the pxr
  // engine's bottom-up color present needs (mirrors WritePyxisColorToAov).
  g_fns.blitNamedFramebuffer(_readFbo, _drawFbo, 0, 0, dimX, dimY, 0, dimY, dimX, 0,
                             COLOR_BUFFER_BIT, NEAREST);
  // glGetError reads the sticky flag without a GPU sync, so this is cheap per frame.
  // On error the blit produced nothing usable; report it once and let the caller fall
  // back to the readback rather than present a stale AOV.
  if (g_fns.getError != nullptr) {
    const GLenum err = g_fns.getError();
    if (err != 0) {
      if (!_loggedCopyError) {
        _loggedCopyError = true;
        std::fprintf(stderr, "GlVkInterop: GL error 0x%x during AOV blit; falling back to readback\n",
                     err);
      }
      return false;
    }
  }
  return true;
}

void GlVkInterop::ReleaseImport() noexcept {
  // Requires Kit's GL context current (true for the per-frame re-import path, which
  // runs in _Execute). The destructor path may run without it on viewport teardown;
  // the glDelete* then no-op and the objects are reclaimed when Kit destroys the GL
  // context at shutdown — a bounded, process-lifetime release, not a steady leak.
  if (g_loadState != 1)
    return;
  if (_srcTexture != 0) {
    g_fns.deleteTextures(1, &_srcTexture);
    _srcTexture = 0;
  }
  if (_memoryObject != 0) {
    g_fns.deleteMemoryObjects(1, &_memoryObject);
    _memoryObject = 0;
  }
  if (_readFbo != 0) {
    g_fns.deleteFramebuffers(1, &_readFbo);
    _readFbo = 0;
  }
  if (_drawFbo != 0) {
    g_fns.deleteFramebuffers(1, &_drawFbo);
    _drawFbo = 0;
  }
  _importedHandle = nullptr;
  _width = 0;
  _height = 0;
}

}  // namespace pyxis::hydra
