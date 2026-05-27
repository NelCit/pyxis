// Pyxis Hydra — Vulkan→GL external-memory interop for the Kit (HgiGL) viewport.
//
// RFC 0008: Kit's pxr viewport runs HgiGL and queries the color AOV via
// HdRenderBuffer::GetResource() (a GL texture). PyxisEngine renders on its own
// Vulkan device into an EXPORTABLE image (Win32 handle). This helper imports that
// handle into Kit's GL context as a GL texture (GL_EXT_memory_object_win32) and
// GPU-copies it into the AOV's HgiGL texture each frame — so the rendered color
// reaches the viewport with NO CPU readback.
//
// ALL methods require Kit's GL context to be current on the calling thread (true on
// the host render thread, where the render pass _Execute / GetResource run). The
// helper loads its GL entry points via wglGetProcAddress on first use; if the
// required GL 4.5 / EXT_memory_object_win32 entry points are unavailable it reports
// failure and the caller falls back to the CPU readback path.

#pragma once

#include <cstdint>

namespace pyxis::hydra {

class GlVkInterop {
 public:
  GlVkInterop() = default;
  ~GlVkInterop();
  GlVkInterop(const GlVkInterop&) = delete;
  GlVkInterop& operator=(const GlVkInterop&) = delete;

  // Load the GL entry points (idempotent). Returns false if the GL context lacks
  // the required functions (caller then uses the readback fallback). Must be called
  // with Kit's GL context current.
  [[nodiscard]] bool EnsureLoaded() noexcept;

  // Import the Vulkan-exported image (`win32Handle`, `allocationSize` bytes, w×h,
  // RGBA16F, the given Vulkan tiling) as a GL texture. Recreated when the
  // handle/size changes; returns the GL texture id (0 on failure).
  [[nodiscard]] uint32_t ImportExportedImage(void* win32Handle, uint64_t allocationSize,
                                             uint32_t width, uint32_t height,
                                             bool optimalTiling) noexcept;

  // GPU copy the imported source texture into `dstGlTexture` (an HgiGL texture id),
  // both GL_TEXTURE_2D RGBA16F w×h, FLIPPING VERTICALLY. Pyxis renders top-row-first
  // (Vulkan); the pxr engine presents the color AOV bottom-row-first (GL convention),
  // so the copy must invert rows or the viewport shows the image upside-down — the
  // GPU equivalent of the WritePyxisColorToAov readback flip. Implemented as a
  // glBlitNamedFramebuffer with the destination Y range reversed (glCopyImageSubData
  // cannot flip). Caller guarantees the Vulkan render has retired (writes visible)
  // before calling. Returns false (error logged once) if the blit raised a GL error,
  // so the caller can take the CPU readback path: that surfaces the problem in the log
  // and feeds any host that reads Map(); a GL host samples GetResource() so its AOV
  // stays as the (failed) blit left it until the next good frame.
  [[nodiscard]] bool CopyImportedInto(uint32_t dstGlTexture, uint32_t width,
                                      uint32_t height) noexcept;

 private:
  void ReleaseImport() noexcept;

  bool _loaded = false;
  // Imported state (recreated on size/handle change).
  uint32_t _srcTexture = 0;
  uint32_t _memoryObject = 0;
  void* _importedHandle = nullptr;
  uint32_t _width = 0;
  uint32_t _height = 0;
  // Framebuffers for the flipping blit (created lazily; src + dst color attachment).
  uint32_t _readFbo = 0;
  uint32_t _drawFbo = 0;
  bool _loggedCopyError = false;  // one-shot guard for the per-frame blit error log

  // GL entry points (loaded via wglGetProcAddress). Opaque void* kept in the .cpp.
  struct Fns;
  Fns* _fns = nullptr;
};

}  // namespace pyxis::hydra
