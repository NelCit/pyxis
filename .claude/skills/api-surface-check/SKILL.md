---
name: api-surface-check
description: Audit a Pyxis PR that touches sources/pyxis_renderer/Public/ against the §18 public API contract and §22 ABI rules. Invoke whenever a header, POD descriptor, handle enum, or public method under Public/Pyxis/Renderer/ is added, modified, or removed. Reports violations grouped by §18.x / §22.x rule and proposes the version.txt bump.
---

# api-surface-check

The renderer's public surface is the **only** contract `pyxis_hydra`, `pyxis_usd_ingest`, and `pyxis_app` may rely on. §18 freezes it byte-for-byte; §22 governs how it may evolve. Reviewers reject PRs that widen or break it without explicit version handling.

## When the user invokes this

1. Diff the public surface vs `main`:
   - `git diff main -- sources/pyxis_renderer/Public/`
   - `git status sources/pyxis_renderer/Public/`
2. For each changed file, classify the change as **MAJOR / MINOR / PATCH** per §22.1, then check it against the rules below.
3. If anything in `_tools/golden_exports.txt` would change, flag it.

## §18.1 Header inventory — exhaustive

Allowed `Public/Pyxis/Renderer/` headers:

`RendererApi.h`, `Forward.h`, `Error.h`, `GpuScene.h`, `PyxisRenderer.h`, `Profiler.h`, `Version.h`, and `Descs/{MeshDesc,OpenPBRMaterialDesc,TextureKey,InstanceDesc,CameraDesc,LightDesc,RenderSettings,RenderTargets,FrameStats,FrameProfile,GpuSceneCreateDesc,RendererCreateDesc}.h`.

Any other header under `Public/` is a violation. Any reference in those headers to `MeshTable`, `BlasCache`, `TlasBuilder`, `OpenPBRMaterialGPU`, `RenderGraph`, `IRenderPass`, `ShaderLibrary`, `DescriptorTableManager`, `ProfilerData`, `GpuTimestampPool`, `SceneWorld`, or any Flecs type is a §18.1 violation.

## §18.9 / §22 — what to grep for

PR-blocking patterns inside `sources/pyxis_renderer/Public/`:

| Pattern | Rule |
|---|---|
| `std::vector`, `std::string`, `std::map`, `std::unordered_*`, `std::deque`, `std::list` | §18.9 — no STL container in any public POD or signature |
| `<windows.h>`, `<Windows.h>` | §30.3 — public headers must not include windows.h |
| `pxr/`, `#include <pxr` | §30.3 — renderer is USD-free |
| `nvrhi::` other than `IDevice*` / `ICommandList*` (forward-declared) | §30.3 — no NVRHI impl types in public |
| Returning `std::span<...>` or `std::string_view` | §18.9 — input-only, never returned, never persisted |
| Field of type `std::span<...>` / `std::string_view` in any `Descs/*.h` POD that is stored | §18.9 |
| `enum class : ` with width other than `uint32_t` for any handle | §19.7 — handle underlying type pinned to `uint32_t` |
| New value inserted in the middle of an existing `enum class` | §22.3 — enums are append-only |
| Method without `[[nodiscard]]` returning `Expected<T>` | §18.5 — mandatory |
| Default argument on any public method | §30.5 — forbidden on public API |
| `throw`, `try`, `catch` | §30.1 — exceptions must not cross the DLL boundary |

## POD layout (§18.4, §18.9)

For each modified `Descs/*.h` POD:

1. Confirm `static_assert(sizeof(...) % 16 == 0)` is present (interop alignment, §23).
2. New fields must consume a trailing `_reservedN` slot left at the previous MAJOR — not appended raw. If no reserved slot is available, the change is MAJOR.
3. `sizeof`, `alignof`, member offsets, and padding are **part of the contract**. Renaming a `_reservedN` field to a typed member while keeping the offset and width is MINOR; anything else is MAJOR.
4. No constructors, no virtuals, no non-trivial members. Designated-initialiser-friendly.

## Version & golden-file impact

After classifying:

- **MAJOR** (signature removed/renamed, `sizeof` changed without reserved consumption, enum renumbered, handle layout changed): bump `version.txt` major; PR title must contain `[abi-break]`; both `_tools/golden_exports.txt` and `pyxis_renderer.def` need updating.
- **MINOR** (new method, new enum value at end, new POD field consuming a `_reservedN`): bump `version.txt` minor; update `_tools/golden_exports.txt`.
- **PATCH**: no version bump; golden files unchanged.

## Output

A markdown report grouped by severity:

```
## Violations (PR-blocking)
- [§18.9] sources/pyxis_renderer/Public/Pyxis/Renderer/Descs/MeshDesc.h:42 — std::vector<float> normals in public POD
- ...

## Required changes
- Bump version.txt to 1.1.0 (MINOR — new method PyxisRenderer::PickAt)
- Update _tools/golden_exports.txt: 3 new exports

## Notes / acceptable
- ...
```

Cite the plan section for every finding. Don't auto-fix — report only.
