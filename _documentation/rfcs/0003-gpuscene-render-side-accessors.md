# RFC 0003: Move render-side `GpuScene` accessors to a `SceneResources` view

- Status: Accepted
- Author(s): Pyxis renderer team
- Created: 2026-05-10
- Last updated: 2026-05-10
- Implementation PRs: (not yet — RFC pending acceptance)

## Summary

Migrate the cluster of `GpuScene::Get<RawNvrhiHandle>()` getters
that were added across M5–M8a from the public `GpuScene` surface to
a renderer-internal `SceneResources` accessor. Restores the §18.1
"narrow public surface" rule for ingest adapters and the app while
keeping the renderer-side render passes (PathTracePass today, the
M9+ render-graph compiles tomorrow) able to bind raw GPU resources.

## Motivation

The audit on the M8a branch flagged `GpuScene.h:201–284` — specifically
the 16 `Get*` methods that expose internal NVRHI handles to every
caller of `pyxis_renderer.dll`:

```cpp
[[nodiscard]] nvrhi::IBuffer*    GetMaterialBuffer() const noexcept;
[[nodiscard]] nvrhi::IBuffer*    GetInstanceMaterialBuffer() const noexcept;
[[nodiscard]] nvrhi::IBuffer*    GetLightBuffer() const noexcept;
[[nodiscard]] nvrhi::IBuffer*    GetInstanceMeshBuffer() const noexcept;
[[nodiscard]] nvrhi::IBuffer*    GetMeshFaceNormalsBuffer() const noexcept;
[[nodiscard]] nvrhi::IBuffer*    GetMeshFaceOffsetsBuffer() const noexcept;
[[nodiscard]] nvrhi::IBuffer*    GetMeshUvsBuffer() const noexcept;
[[nodiscard]] nvrhi::IBuffer*    GetMeshUvOffsetsBuffer() const noexcept;
[[nodiscard]] nvrhi::IBuffer*    GetMeshIndicesBuffer() const noexcept;
[[nodiscard]] nvrhi::IBuffer*    GetMeshIndexOffsetsBuffer() const noexcept;
[[nodiscard]] nvrhi::ITexture*   GetDomeEnvMapTexture() const noexcept;
[[nodiscard]] nvrhi::ISampler*   GetBindlessSampler() const noexcept;
[[nodiscard]] nvrhi::ITexture*   GetMissingTexture() const noexcept;
[[nodiscard]] uint32_t           GetBindlessTextureCount() const noexcept;
[[nodiscard]] nvrhi::ITexture*   GetBindlessTextureAt(uint32_t bindlessSlot) const noexcept;
[[nodiscard]] nvrhi::rt::IAccelStruct* GetTlas() const noexcept;
```

Plan §18.1 calls the public surface "exhaustive" and "anything else
is `Private/`". These 16 getters legitimately return data that the
render thread needs to bind into descriptor sets, but they leak
that data to anyone holding a `GpuScene*` — including the ingest
adapters (`pyxis_hydra` / `pyxis_usd_ingest`) and `pyxis_app` that
have no business binding raw NVRHI resources.

## Detailed design

Introduce a new renderer-internal accessor type `SceneResources`,
declared in `sources/pyxis_renderer/Private/Scene/SceneResources.h`,
not exported from the DLL. Render passes receive a `SceneResources&`
via the `PassContext` (already in flight) instead of holding a
`GpuScene*`.

```cpp
// sources/pyxis_renderer/Private/Scene/SceneResources.h
namespace pyxis {
struct SceneResources {
  // Same getter set, returned by reference from
  // GpuScene::Impl::GetRenderResources(). PathTracePass etc. consume
  // this struct directly. Never crosses the DLL boundary.
  nvrhi::rt::IAccelStruct*  tlas                     = nullptr;
  nvrhi::IBuffer*           materialBuffer           = nullptr;
  nvrhi::IBuffer*           instanceMaterialBuffer   = nullptr;
  nvrhi::IBuffer*           lightBuffer              = nullptr;
  nvrhi::IBuffer*           instanceMeshBuffer       = nullptr;
  nvrhi::IBuffer*           meshFaceNormalsBuffer    = nullptr;
  nvrhi::IBuffer*           meshFaceOffsetsBuffer    = nullptr;
  nvrhi::IBuffer*           meshUvsBuffer            = nullptr;
  nvrhi::IBuffer*           meshUvOffsetsBuffer      = nullptr;
  nvrhi::IBuffer*           meshIndicesBuffer        = nullptr;
  nvrhi::IBuffer*           meshIndexOffsetsBuffer   = nullptr;
  nvrhi::ITexture*          domeEnvMapTexture        = nullptr;
  nvrhi::ISampler*          bindlessSampler          = nullptr;
  nvrhi::ITexture*          missingTexture           = nullptr;
  // Bindless texture iteration (count + per-slot pointer). Same
  // semantics as today's GetBindlessTextureCount/At getters.
  uint32_t                  bindlessTextureCount     = 0;
  nvrhi::ITexture* const*   bindlessTextures         = nullptr; // sparse, size = count
};
}  // namespace pyxis
```

`PyxisRenderer::RenderFrame` populates a fresh `SceneResources` per
frame from `GpuScene::Impl` (via a renderer-internal friend
accessor) and threads it into the `PassContext`.

Public `GpuScene` keeps only:
- The mutation verbs (`CreateMesh`, `AcquireMaterial`, …).
- `GetCamera` / `HasCamera` / `LastFrameStats` / `Clear` / `CommitResources`.
- Editor introspection (`GetLiveLightCount` / `GetLightHandleAt` / etc. — these are NOT NVRHI handles).
- `LookupInstanceMaterialBySlot` (pure data lookup, no NVRHI).

## Alternatives considered

1. **Keep current pattern + document as renderer-internal contract** —
   the audit notes the pattern is pre-existing and not newly
   introduced by M8a. Rejected because §18.1 is normative; pre-
   existing violations don't excuse new ones, and the cleanup is
   straightforward.
2. **Make `GpuScene::Get*` methods explicitly `friend PyxisRenderer`**
   — doesn't actually narrow the public ABI surface (the symbols
   still export from the DLL); only narrows source-level visibility.
   Rejected.
3. **Move the entire render-side surface into a separate
   `pyxis_renderer_internal.dll`** — overkill for v1; would require
   re-architecting how passes link.

## Drawbacks / risks

- Touching the public `GpuScene.h` is a §22 MAJOR break for the 16
  removed methods. Pre-1.0 (§22) means no SemVer obligation; still,
  the cleanup is best done in a single PR and documented in
  `version.txt` notes.
- `PassContext` gains a new `SceneResources*` field — minor PIMPL
  shake-up.
- `pyxis_hydra` + `pyxis_usd_ingest` should never have used these
  getters anyway (they never did); confirming with a `Grep` over
  both modules before merge.

## Migration & impact

- Renderer code (PathTracePass): switch `_scene->GetX()` → `context.resources->X`.
- Public header: remove the 16 `Get*` methods.
- `version.txt`: bump MAJOR (or MINOR if pre-1.0 is still in effect).
- Affected milestones: applies to M8b polish window or M9 (no rush;
  cleanup is mechanical).
- No regression-image impact (move only).

## Open questions

- Does `SceneResources` belong in `Private/Scene/` or in a new
  `Private/Render/`? `Private/Scene/` keeps it co-located with
  `SceneWorld` which is the conceptual owner.
- Should `bindlessTextures` be a `std::span<nvrhi::ITexture* const>`
  (header-public via `<span>`, no STL container, OK) or a raw
  pointer + count? Span is cleaner; raw is consistent with the rest
  of the struct.
