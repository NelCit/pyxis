# Pyxis — core map

C++23 real-time path tracer (NVRHI/Vulkan, Slang), Windows-only v1. Target: Amazon Lumberyard World Lobby USD end-to-end on one workstation.

**Source of truth: `plan_final.md` (~5700 lines).** `CLAUDE.md` = section index (§N ↔ plan sections). Plan is normative — deviations need an RFC (plan §44). Read the relevant § before substantive design/code answers.

Four-layer stack (only `pyxis_renderer`'s Public/ API crosses layers):
- `sources/pyxis_app/` — exe, viewer + headless modes
- `sources/pyxis_hydra/` (Hydra 2.0 Scene-Index delegate) and `sources/pyxis_usd_ingest/` (direct UsdStage walker) — two ingest adapters, one active per run, must produce byte-identical EXRs (P0 invariant, §25.O.3)
- `sources/pyxis_renderer/` — `GpuScene` (owns Flecs world in `Private/GpuScene/`), `RenderGraph` (linear, no DAG), `Private/Scene/` ECS primitives
- `sources/pyxis_platform/` — NVRHI device, GLFW, spdlog, Tracy
- `sources/pyxis_material_translation/` — static lib, UsdPreviewSurface/MaterialX/RenderMan → OpenPBR, shared by both adapters

Layout rule: `sources/pyxis_<module>/Public/Pyxis/<Module>/...` is the exhaustive public surface (§18.1); everything else `Private/`. Never widen Public/ without RFC.

Key invariants: byte-frozen public PODs (§22.3 `_reserved` slots); no STL across DLL boundary; strong handles `enum class : uint32_t` 24-bit slot + 8-bit generation; headless determinism (§33.7) pins seed/ordering/jitter → EXR regression is the only test artefact.

Related: `mem:tech_stack` (deps/build), `mem:conventions` (§30 rules incl. Flecs), `mem:suggested_commands` (presets, run/test), `mem:task_completion` (definition of done).
