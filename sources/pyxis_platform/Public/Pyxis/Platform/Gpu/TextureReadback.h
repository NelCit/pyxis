// Pyxis platform — synchronous GPU→CPU texture readback helper.
//
// Folds the staging-texture / copy / map / unmap / row-pitch-strip dance shared by
// the viewer --screenshot path, the headless EXR path, the AOV-EXR saver, AND the
// Hydra delegate's per-frame color readback (PyxisEngine) into one RAII object.
// Each call site used to inline ~30 lines of identical NVRHI plumbing.
//
// Lives in pyxis_platform/Public because it is consumed across module boundaries
// (pyxis_app + pyxis_hydra + tests) and touches only the already-allowed opaque
// NVRHI types (IDevice*/ICommandList*/ITexture*/StagingTextureHandle) — no concrete
// NVRHI resources or STL containers cross a frozen ABI here (§18.9 / §30).
//
// Two phases (the caller owns the command list + synchronisation):
//   1. RecordCopy(device, commandList, source, debugName)
//        (Re)creates a CpuAccessMode::Read staging texture sized to `source`
//        (REUSED across calls on the same instance when the size matches — so a
//        per-frame caller pays the allocation only once) and records
//        copyTexture(staging, source) on the caller's already-open command list.
//        The caller then transitions/closes/executes and synchronises (waitForIdle
//        or a targeted EventQuery) before Map().
//   2. Map() → Data()/RowPitch()/Width()/Height()/Format() valid until Unmap()/dtor.
//
// Reuse: hold one TextureReadback as a member and call RecordCopy + Map + (Unmap)
// each frame; the staging texture is allocated once and reused (recreated only on a
// size change). One-shot callers use the static RecordCopyOnce convenience.

#pragma once

#include <Pyxis/Platform/PlatformApi.h>

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace pyxis {

class PYXIS_PLATFORM_API TextureReadback {
 public:
  TextureReadback() = default;
  ~TextureReadback() noexcept;

  TextureReadback(const TextureReadback&) = delete;
  TextureReadback& operator=(const TextureReadback&) = delete;
  TextureReadback(TextureReadback&&) noexcept;
  TextureReadback& operator=(TextureReadback&&) noexcept;

  // Phase 1 (instance, reusable). `commandList` must already be open; `device` and
  // `source` non-null. Reuses the held staging texture when it already matches
  // `source`'s dims+format; otherwise (re)creates it. Unmaps any prior mapping.
  [[nodiscard]] std::expected<void, std::string> RecordCopy(nvrhi::IDevice* device,
                                                            nvrhi::ICommandList* commandList,
                                                            nvrhi::ITexture* source,
                                                            const char* debugName) noexcept;

  // One-shot convenience: a fresh instance with the copy recorded. Equivalent to
  // `TextureReadback r; r.RecordCopy(...);`. For call sites that read once.
  [[nodiscard]] static std::expected<TextureReadback, std::string> RecordCopyOnce(
      nvrhi::IDevice* device, nvrhi::ICommandList* commandList, nvrhi::ITexture* source,
      const char* debugName) noexcept;

  // Phase 2. Caller must have closed+executed the command list and synchronised
  // before this returns valid data.
  [[nodiscard]] std::expected<void, std::string> Map() noexcept;
  void Unmap() noexcept;

  // Copy the mapped image into `out`, stripping the staging row pitch to a tight
  // width*bytesPerPixel stride. `out` is resized (capacity reused). Must be Mapped.
  void CopyTightlyInto(std::vector<uint8_t>& out) const noexcept;

  [[nodiscard]] const void* Data() const noexcept { return _mapped; }
  [[nodiscard]] std::size_t RowPitch() const noexcept { return _rowPitch; }
  [[nodiscard]] uint32_t Width() const noexcept { return _width; }
  [[nodiscard]] uint32_t Height() const noexcept { return _height; }
  [[nodiscard]] nvrhi::Format Format() const noexcept { return _format; }

 private:
  nvrhi::IDevice* _device = nullptr;
  nvrhi::StagingTextureHandle _staging = nullptr;
  const void* _mapped = nullptr;
  std::size_t _rowPitch = 0;
  uint32_t _width = 0;
  uint32_t _height = 0;
  nvrhi::Format _format = nvrhi::Format::UNKNOWN;
};

}  // namespace pyxis
