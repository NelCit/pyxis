# M9 — World Lobby visual correctness

**Status: in flight** (smooth shading + emission landed; normal mapping
deferred to a follow-up PR pending MikkTSpace integration)

§41 M9 normatively targets Bistro; substituting the lobby (same pattern
as M8b — see `_documentation/m8b-lobby-perf.md`).

## What landed in this milestone

### 1. Smooth shading via per-vertex normals

**Problem.** The M7-simple closesthit reads only per-triangle face
normals (`gMeshFaceNormals[faceOffset + PrimitiveIndex()]`) and
Lambert-shades against them, producing visibly faceted output on every
curved surface (lobby vases, lamp shades, seating, the reception
desk's curved counter).

**Fix.** Mirror of the M8a UV pipeline, but per-vertex:
- `StageWalker::EmitMesh` reads `meshPrim.GetNormalsAttr()` (schema-
  level, the lobby's convention) or falls back to
  `primvars:normals`. Handles `vertex` / `faceVarying` / `constant` /
  `uniform` interpolations; faceVarying collapses to per-vertex by
  taking the first normal at each shared vertex. Per-vertex vector
  fed to `MeshDesc::normals`.
- `GpuScene::Impl::UploadMeshVertexNormals` (new phase in
  `Commit.cpp`) packs every live mesh's vertex normals into one flat
  `float4` buffer + per-mesh start-offset table. Short / empty
  arrays pad to `vertexCount` so each mesh's slice stays aligned to
  its vertex range (same defensive pattern as the UV path).
- New public getters `GpuScene::GetMeshVertexNormalsBuffer()` /
  `GetMeshVertexNormalOffsetsBuffer()`.
- `PathTracePass`: 2 new structured-buffer bindings (29, 30) +
  matching fallback handles + the slot enum.
- Closesthit: reads three per-vertex normals via
  `gMeshIndices` (same triple it uses for UV interp), barycentric-
  interpolates, normalizes. Falls back to the M7 face-normal path
  when the interpolated magnitude is < 1e-3 (mesh authored no
  normals or extraction failed).

**Visual impact on lobby.** Curved surfaces stop showing facets;
shading is smooth across faceVarying-shared vertices.

### 2. Emissive materials (UsdPreviewSurface `emissiveColor`)

**Problem.** `OpenPBRMaterialDesc::emissionColor` is read by
`FromUsdShade` but `OpenPBRMaterialGPU` only carries `emissionLuminance`
(scalar) — the RGB tint was being silently dropped. The closesthit
also never added emission to the output. Result: lobby materials
authored with `emissiveColor = (warm tone)` rendered as grey
baseColor.

**Fix.**
- `OpenPBRMaterialGPU` extended from 80 → 96 bytes (5 → 6 rows of 16).
  New row 5 carries `emissionR` / `emissionG` / `emissionB` +
  `_reserved1` slot. Static-asserts updated.
- `PackMaterialGpu` packs `desc.emissionColor.{x,y,z}` into the new
  fields.
- `FromUsdShade` sets `desc.emissionLuminance = 1.0` when
  `emissiveColor` magnitude > 0 (UsdPreviewSurface authors emission as
  a single color3f with implicit luminance from the magnitude — there's
  no separate scalar input). Magnitude threshold keeps materials with
  explicit `emissiveColor = (0,0,0)` non-emissive.
- Closesthit: when `MATERIAL_FLAG_EMISSIVE` is set, computes
  `emission = emissionColor × emissionLuminance` and adds to
  `payload.color`. Multiplies by `gBindlessTextures[mat.emissionTex]`
  sample if the material flagged `HAS_EMISSION_MAP`.

**Visual impact on lobby.** Emissive practicals (8 materials in the
lobby author non-zero `emissiveColor`) now contribute their
authored radiance.

## §34 KPI compliance unchanged

Re-measured at 1080p, RTX 4070 Laptop, Release, 60 + 60 frames:

| Scope                       | Before M9 (p50/p99) | After M9 (p50/p99) | Δ |
|---|---|---|---|
| `pass.PathTrace`            | 1.14 / 1.61 ms      | 1.12 / 1.15 ms     | ~0 |
| `render.commitResources`    | 0.003 / 0.008 ms    | 0.002 / 0.029 ms   | ~0 |

The smooth-shading + emission additions cost a few extra GPU reads +
multiplies; well within the 12 ms `pass.PathTrace` budget.

## Deferred to follow-up PRs

### Normal mapping

`mat.normalTex` is acquired by the material translator and survives
through to `OpenPBRMaterialGPU.normalTex`, but the closesthit doesn't
sample it. Sampling requires:

1. **Tangent loading.** The lobby authors no `primvars:tangents`. We'd
   need to either compute tangents at load time via MikkTSpace
   (vcpkg has it as a single-file header) or skip tangents and use
   geometric tangent derivation in-shader (less accurate at UV seams).
2. **Per-mesh tangent buffer** — mirror of the vertex-normal pipeline
   above (per-mesh flat float4 + offset table; `tangent.w` carries
   the bitangent sign).
3. **Closesthit TBN matrix** + tangent-space normal sample +
   transform to world space.

This is a substantial follow-up; landing it as a single dedicated PR
keeps M9's review surface focused on the smooth-shading + emission
core.

### Dome+sun alignment audit

The miss shader's lat-long mapping assumes Y-up (v=0 at +Y, v=1 at -Y).
The Z→Y stage correction we apply at ingest already routes the dome
through the same world-space rotation as everything else. Spot-check
suggested no observable misalignment, but a rigorous audit needs visual
A/B against usdview / Storm output — out of scope for headless-only
testing.

### MaterialX coverage

The lobby is pure UsdPreviewSurface so this is **not relevant for the
lobby M9**. Strict §41 M9 calls out MaterialX gap-closure for Bistro
which we'd address when Bistro lands.

## How to reproduce

```powershell
cmake --build build/dev --config Release
& "build/dev/bin/Release/pyxis.exe" `
    --headless `
    --scene "resources/scenes/world_lobby/World_Lobby.usd" `
    --output "build/dev/lobby_m9.exr" `
    --width 1920 --height 1080 `
    --seed 1 `
    --bench-frames 60
```

Compare `lobby_m9.exr` to a pre-M9 render to see the smooth-shading
delta on curved surfaces + the emissive contribution from materials
with `emissiveColor` authored.
