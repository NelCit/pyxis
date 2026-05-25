# RFC 0007 — One Hydra module: the delegate ingests via StageWalker; retire pyxis_hydra_omni

- **Status**: Draft
- **Author(s)**: Pyxis team
- **Created**: 2026-05-25
- **Last updated**: 2026-05-25
- **Supersedes**: RFC 0006 Phase 4 ("the delegate is the single source of truth …
  real mesh/camera/light Sync"). Amends the §25 / §25.O.3 two-adapter rationale.
- **Implementation PRs**: (this branch — `migrate-nv-usd`)

## Summary

Collapse the Hydra integration to a **single C++ module, `pyxis_hydra`**, and make
its render delegate **ingest the USD stage through the same `pyxis_usd_ingest::StageWalker`
as the standalone**, instead of re-implementing per-prim translation in
`HdPyxis{Mesh,Light,Camera}::Sync`. Retire the separate `pyxis_hydra_omni` C++
module entirely. Omniverse Kit integration reduces to: (1) select **Pyxis** as the
viewport render engine, and (2) edit **Pyxis render settings** — no custom viewport
panel, no native Carbonite extension plugin.

## Motivation

RFC 0006 removed vcpkg OpenUSD 26.3 and standardised on Kit's nv-usd 25.11. The
sole reason `pyxis_hydra_omni` existed as a separate, out-of-tree build was the
**ABI split**: a delegate built against vcpkg `pxrInternal_v0_26_3` could not load
in Kit (nv-usd). That split is gone — a `pyxis_hydra.dll` built in `build/dev` is
now nv-usd 25.11 and loads directly in Kit. The module's reason-for-being is
eliminated; what remained was duplicated work:

- **Build duplication.** `build/omni` recompiled `HdPyxisRenderDelegate.cpp`
  (~1700 lines) + `PyxisEngine.cpp` + `UsdShadeToOpenPBR.cpp` into 5 targets; with
  the build/dev copy that's 6 compilations of identical sources under identical
  flags (C++23, `/EHs c /GR`, nv-usd 25.11, py312) — a pure flag-drift hazard.
- **Source duplication.** The delegate's per-prim Sync translation
  (`EmitToScene` incl. GeomSubset splitting, `ResolveMaterial`, light/camera Sync)
  re-implemented what `StageWalker` already does. It was the copy that **drifted**
  (the wash-out, dark-render, missing-subset, camera-conform, and sRGB bugs fixed
  on this branch were all the delegate's copy diverging from StageWalker).

Standardising on StageWalker makes the World Lobby (geometry, face-subsets,
materials incl. MDL, lights, analytic geom, instancing, camera) land **byte-identical**
across both adapters by construction, and lets the delegate, host, tests, and the
parity suite all live in one module.

## Detailed design

**Ingest.** `HdPyxisRenderDelegate`'s render pass calls
`StageWalker::WalkStage(stage, gpuScene)` once per stage (the stage comes from the
host via `SetStage`, or from `UsdUtilsStageCache` under Kit's `omni.hydra.pxr`). The
per-prim `Sync` paths are **removed** — the Rprim/Sprim classes remain (Hydra's
lifecycle requires them) but their `Sync` is a no-op. The redundant
`HdsiImplicitSurfaceSceneIndex` in the host is removed (StageWalker tessellates
analytic prims itself).

**One module.** `pyxis_hydra` (built in `build/dev`, nv-usd) contains the delegate,
`PyxisEngine`, `PyxisHydraHost` (pure pxr/hd, zero Kit deps), the headless tests
(smoke / World-Lobby / multi-cycle-VRAM / plugin-load), and the §25.O.3 parity
suite. It produces one `pyxis_hydra.dll` (a standard USD `HdRendererPlugin`) used
by usdview **and** Kit. `pyxis_hydra_omni` is deleted.

**Kit integration = packaging only.** The Kit extension carries no compiled C++:
- plugin registration via Python (`Plug.Registry().RegisterPlugins(<bin>/resources)`
  in the extension `__init__.py`) — replaces the native Carbonite IExt
  (`PyxisExtModule`);
- the existing Python activator (`__init__.py`) that switches `omni.hydra.pxr` to
  Pyxis via `set_hd_engine` and the **render-settings** stack (`render_settings.py`)
  stay;
- the **custom viewport panel** (`PyxisViewportPanel` + `PyxisViewportBridge` +
  the zero-copy `GpuInteropExporter` display path) is **dropped** — the product
  requirement is "select Pyxis as the render engine + edit its render settings",
  which `omni.hydra.pxr` already provides;
- a stage script copies `build/dev` `pyxis_hydra.dll` + its runtime deps + the
  extension tree into the Kit app.

## Alternatives considered

1. **Keep the per-prim Sync delegate as a genuinely independent adapter (Option C).**
   Most faithful to Hydra and preserves §25's *independent* cross-check, but it is
   the fragile path that already drifted; re-achieving byte-identical parity is
   continuous work, and v1 has no live-edit / non-USD-Hydra requirement (§40.5).
   Rejected for v1; revisit if procedural scene-index data or live editing becomes
   a requirement.
2. **Keep `pyxis_hydra_omni` but link the prebuilt delegate (factor only the build).**
   Removes the recompiles but leaves a second module + the dead Sync path + the
   Kit-only IExt/panel. Half the cleanup; rejected in favour of full collapse.

## Drawbacks / risks

- **§25.O.3 changes meaning.** "Two *independent* adapters, byte-identical EXR" →
  "one ingest (StageWalker), parity by construction." The parity suite now mostly
  guards the delegate's *host/camera/encoding* plumbing, not an independent
  re-implementation. This is an accepted reduction in cross-validation value.
- **Delegate is now USD-stage-bound.** It cannot render non-USD Hydra data
  (procedural scene indices) or do efficient incremental/live edits, and a generic
  Hydra host that never registers its stage in `UsdUtilsStageCache` won't render.
  Acceptable for v1 (USD scenes, Kit + usdview both populate the cache).
- **Python plugin registration must be as reliable as the native IExt** — requires
  live Kit verification (timing of `RegisterPlugins` on extension load).
- **Lost feature: the custom zero-copy viewport panel.** Deliberate — out of scope.

## Migration & impact

- `pyxis_hydra` gains `PyxisHydraHost`, the four tests, and the parity ctests
  (labelled; run locally on a GPU, excluded from CI like the `m2` byte-equal test).
- `sources/pyxis_hydra_omni/` is deleted; `_tools/omniverse/build.ps1` becomes a
  build-`pyxis_hydra` + stage-extension script.
- Affected milestones: none of the M0–M11 deliverables change behaviour; this is a
  structural consolidation of the RFC 0004 / RFC 0006 Hydra work.
- The renderer/platform stay USD-free (§1) — unchanged.

## Open questions

- Does Python `RegisterPlugins` on extension load reliably make Pyxis selectable in
  Kit's Render menu across cold start + stage reload? (Live-verify; fall back to a
  minimal IExt only if Python proves unreliable.)
