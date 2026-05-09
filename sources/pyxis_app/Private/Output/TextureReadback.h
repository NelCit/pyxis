// Pyxis app — synchronous texture readback helper.
//
// Folds the staging-texture / copy / waitForIdle / map / unmap dance
// shared between ViewerMode's --screenshot path and HeadlessMode's
// --headless EXR path into a single RAII handle. Both call sites used
// to inline ~30 lines of identical NVRHI plumbing; consolidating
// keeps state-tracking (debug name, format, dims) and lifetime
// (unmap-on-destruct) in one place.
//
// Two-phase by design — the caller owns command-list lifetime:
//
//   1. RecordCopy(device, commandList, source, debugName)
//        Creates a CpuAccessMode::Read staging texture sized to
//        `source` and records `copyTexture(staging, source)` on the
//        caller's already-open command list. The caller then drives
//        any further state transitions (e.g. viewer puts the
//        backbuffer back into ResourceStates::Present), closes the
//        command list, executes it, and synchronises (waitForIdle).
//
//   2. Map()
//        Maps the staging texture for read. Caller MUST have ensured
//        the GPU has retired the copy before this is called, typically
//        via device->waitForIdle() right before. Data() / RowPitch() /
//        Width() / Height() / Format() are valid only between Map() and
//        destruction.
//
// On destruction the staging texture is unmapped and released. The
// helper holds a raw `nvrhi::IDevice*`, so the device must outlive the
// readback handle (true at every call site today — both modes destroy
// readbacks within the same scope as the device manager).

#pragma once

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace pyxis::app {

class TextureReadback {
 public:
  // Phase 1. `commandList` must already be open. `source` and `device`
  // must be non-null.
  [[nodiscard]] static std::expected<TextureReadback, std::string> RecordCopy(
      nvrhi::IDevice* device, nvrhi::ICommandList* commandList, nvrhi::ITexture* source,
      const char* debugName) noexcept;

  // Phase 2. Caller is responsible for closing + executing the
  // command list passed to RecordCopy and synchronising (waitForIdle)
  // before this returns valid data.
  [[nodiscard]] std::expected<void, std::string> Map() noexcept;

  [[nodiscard]] const void* Data() const noexcept { return _mapped; }
  [[nodiscard]] std::size_t RowPitch() const noexcept { return _rowPitch; }
  [[nodiscard]] uint32_t Width() const noexcept { return _width; }
  [[nodiscard]] uint32_t Height() const noexcept { return _height; }
  [[nodiscard]] nvrhi::Format Format() const noexcept { return _format; }

  TextureReadback(const TextureReadback&) = delete;
  TextureReadback& operator=(const TextureReadback&) = delete;
  TextureReadback(TextureReadback&&) noexcept;
  TextureReadback& operator=(TextureReadback&&) noexcept;
  ~TextureReadback() noexcept;

 private:
  TextureReadback() = default;

  nvrhi::IDevice* _device = nullptr;
  nvrhi::StagingTextureHandle _staging = nullptr;
  const void* _mapped = nullptr;
  std::size_t _rowPitch = 0;
  uint32_t _width = 0;
  uint32_t _height = 0;
  nvrhi::Format _format = nvrhi::Format::UNKNOWN;
};

}  // namespace pyxis::app
