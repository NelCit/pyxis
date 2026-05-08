---
name: ingest-parity-check
description: Render the same fixture through both the Hydra adapter and the USD-direct adapter and verify byte-identical (or RMSE-tolerant) EXR output, per §25.O.3 / §40.1. Invoke when a PR touches sources/pyxis_hydra/, sources/pyxis_usd_ingest/, or sources/pyxis_material_translation/, or whenever a regression is suspected to come from an adapter mismatch. Reports per-fixture diffs, identifies which adapter regressed.
---

# ingest-parity-check

A P0 invariant of the project: **both ingest adapters produce byte-identical EXRs against the same scene** (§25.O.3, §40.1, M4 exit criterion). Each Moana regression image is rendered twice in CI, once per adapter. Drift between adapters is a release-blocker.

## When to run

- PR diff touches any of:
  - `sources/pyxis_hydra/`
  - `sources/pyxis_usd_ingest/`
  - `sources/pyxis_material_translation/` (shared by both adapters)
  - `sources/pyxis_renderer/Private/Scene/Components/`, `Systems/`, or anything that changes how `MeshDesc` / `OpenPBRMaterialDesc` / `InstanceDesc` is consumed
- Investigating an unexpected regression-image change: which adapter shifted?
- Before approving a PR that explicitly claims "ingest-only refactor".

## Procedure

The adapter is selected at startup via `app.ingest = "hydra" | "usd_direct"` (CLI / `parameters.json`). For each parity fixture:

1. Render twice from the same `parameters.json`:
   - Once with `app.ingest = "hydra"` → `out_hydra.exr`
   - Once with `app.ingest = "usd_direct"` → `out_usd_direct.exr`
2. Diff with the regression harness — RMSE / MAE / PSNR / SSIM. **Required: RMSE = 0** on the pinned hardware/driver matrix (§33.7); per-test tolerance otherwise.
3. If RMSE ≠ 0: which pixels differ? Save the diff EXR. Drill into:
   - Mesh extraction: did `MeshDesc` for the same prim differ in vertex count / index order / primvar layout?
   - Material translation: did `OpenPBRMaterialDesc` hash differ between adapters? (Both run through `pyxis_material_translation`; a hash mismatch points at a code path one adapter takes but not the other.)
   - Instance flattening: nested instancers, point instancers, native instancers — order / transforms.
   - Camera / light: same `CameraDesc` / `LightDesc` from each adapter?

## Determinism preconditions (§35, §33.7)

For byte-identical EXR output, the fixture **must** set:

- `accumulationFrameLimit > 0` (rejected otherwise — §21.2)
- `seed > 0` (rejected otherwise — §18.4)
- Fixed resolution, camera, sample count, exposure, tone-map, max bounces
- `framesInFlight = 3` (pinned for headless byte-identity — §33.7)

Byte-identity is scoped to: RTX 4080, pinned NVIDIA Game Ready Driver range, pinned Vulkan SDK, Win 11 23H2/24H2. Outside that matrix → per-test RMSE/MAE tolerance.

## Fixtures to run (default set)

Start with the smallest parity sentinels, then escalate:

- `triangle.usda` — first-pixel sanity (M2/M4)
- `quad_triangulated.usda` — `HdMeshUtil::ComputeTriangleIndices` parity vs StageWalker's own triangulation
- `uv_cube.usda` — UV0 + tangent generation (MikkTSpace) parity
- `cube_with_subsets.usda` — `GeomSubsets` per-face material binding parity
- `instanced_cubes.usda` + `point_instancer_rocks.usda` + `nested_instancers.usda` — instance flattening parity
- `usd_preview_complete.usda` — every UsdPreviewSurface field
- `mtlx_open_pbr_surface.usda`, `mtlx_standard_surface.usda` — MaterialX paths
- `udim_textures.usda` — UDIM resolver (M9)
- `dome_light_envmap.usda`, `distant_plus_rect.usda` — lighting (M7)
- `moana_subset/` — nightly seed (M8a)

A PR that adds a new fixture must add it to the parity set unless the fixture is by design adapter-specific (rare; flag if claimed).

## Output

```
## Parity check (Hydra vs USD-direct)

| Fixture | RMSE | MAE | PSNR | Verdict |
|---|---|---|---|---|
| triangle.usda | 0 | 0 | inf | PASS (byte-identical) |
| uv_cube.usda | 4.2e-3 | 1.1e-3 | 47.8 | FAIL — exceeds tolerance |
| ...

## Diagnosis (failures)
- uv_cube.usda: tangent vectors differ at vertex indices [3, 7, 11].
  USD-direct StageWalker computes MikkTSpace on un-triangulated quads;
  HdMeshUtil triangulates first. See sources/pyxis_usd_ingest/Private/StageWalker.cpp:142.

## Action
- Block PR until RMSE = 0 on the pinned matrix
- Open RFC if the divergence is intentional (§44)
```

Run it; don't auto-fix.
