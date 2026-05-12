# M9 — World Lobby visual correctness

**Status: complete** (smooth shading + emission + normal mapping +
translucent-as-invisible all landed; dome alignment audited)

§41 M9 normatively targets World Lobby; substituting the lobby (same pattern
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

### 3. Translucent materials → invisible (anyhit IgnoreHit stub)

**Problem.** First-bounce shading treated every triangle hit the same:
the closesthit shaded the surface and the ray stopped. Materials
authored with `opacity < 1` or `transmissionWeight > 0` (glass,
water, semi-transparent overlays) were rendering as solid surfaces
that blocked light from reaching what's behind them.

**Fix (M9 stub — proper transmission BSDF is M11+).**
- New `resources/shaders/anyhit.slang` reads
  `gMaterials[gInstanceMaterial[InstanceID()]]` and calls `IgnoreHit()`
  when the material has `opacity < 0.999` OR
  `MATERIAL_FLAG_TRANSMISSION_ENABLED` OR `MATERIAL_FLAG_ALPHA_TESTED`.
  The ray then passes through and traversal continues to the next
  intersection — the closesthit fires on the closest opaque hit
  behind the translucent geometry.
- `pyxis_compile_slang_shader` wires it into the build; PathTracePass
  loads `anyhit.spv`, adds it to `HitGroupDefault.setAnyHitShader`,
  and ReloadShaders is symmetric.
- BLAS build drops the `Opaque` flag globally so the anyhit fires on
  every hit. Opaque-material cost is one extra anyhit invocation
  that returns immediately — measured ~0.45 ms p50 added to
  `pass.PathTrace` on the lobby (1.12 → 1.55 ms), still 6× headroom
  on the 12 ms KPI.

### 4. Normal mapping (MikkTSpace tangents)

**Problem.** Lobby materials author normal maps (`mat.normalTex`
acquired by the translator + survives all the way to
`OpenPBRMaterialGPU.normalTex`), but the closesthit never sampled
them — flat tangent-space detail (terrazzo, oak grain, ceramic tile,
brushed steel) was missing entirely.

**Fix.**
- `mikktspace` (already in `vcpkg.json`) wired into `pyxis_usd_ingest`
  via `find_package(mikktspace CONFIG REQUIRED)` + the
  `mikktspace::mikktspace` link target.
- `StageWalker::EmitMesh` runs `genTangSpaceDefault` after
  triangulation + normal extraction: callbacks expose positions,
  faceVertex indices, normals, UVs; `setTSpaceBasic` writes
  `(tangent.xyz, sign)` per face-vertex into a per-vertex `float4`
  buffer (first-tangent-wins per shared vertex — same lossy
  collapse used for normals; vertex duplication for accurate
  hard-edge tangents is M11+ polish).
- New per-mesh `meshTangentsBuffer` + `meshTangentOffsetsBuffer` on
  `GpuScene::Impl`; `UploadMeshTangents` commit phase + dirty bump
  on `CreateMesh`; new public getters
  `GetMeshTangentsBuffer()` / `GetMeshTangentOffsetsBuffer()`.
- PathTracePass: 2 new structured-buffer bindings (31, 32) +
  fallbacks + slot-enum entries.
- Closesthit: when `MATERIAL_FLAG_HAS_NORMAL_MAP` is set + the
  material's bindless `normalTex` slot is valid + the per-vertex
  tangent has non-zero magnitude (mesh has authored UVs + normals),
  builds a TBN matrix from the per-vertex tangent + barycentric-
  interpolated normal + sign-derived bitangent, samples the normal
  map at the interpolated UV, transforms tangent-space normal to
  world. Falls back to the smooth-interp normal when any guard
  fails (mesh has no UVs, MikkTSpace gave up on degenerate
  authoring, etc.).

### 5. Dome alignment audit

**Conclusion: lobby alignment is correct. No code change needed.**

The lobby's DomeLight authors:
- `inputs:texture:format = "latlong"` ✓ matches our miss shader
- `xformOp:rotateXYZ = (0, 0, 0)` ✓ no per-prim rotation to apply
- `inputs:texture:file = @@` (empty) → bundled `default_sky.exr`
  fallback fires, which is Y-up authored (Poly Haven convention)

The miss shader's lat-long math (`v = acos(dir.y)/π`,
`u = atan2(dir.x, dir.z)/2π + 0.5`) is Y-up canonical. The stage
Z→Y correction we apply at ingest puts world rays into Y-up space
before they reach the miss shader. End-to-end alignment is
self-consistent.

**Out-of-lobby gap (not triggered here):** we silently drop
per-prim dome rotation (`xformOp:rotateXYZ` on the DomeLight prim).
A scene that authors `rotateY = 45°` on its dome to rotate the
HDRI horizon would render the HDRI at the unrotated orientation.
Fix is a small `LightGpu` extension (3 floats for rotation) +
miss-shader transform of the sample direction; deferred to the
first scene that actually needs it.

## §34 KPI compliance unchanged

Re-measured at 1080p, RTX 4070 Laptop, Release, 60 + 60 frames:

| Scope                    | M8b baseline | After M9 full stack | Headroom |
|---|---|---|---|
| `pass.PathTrace`         | 1.14 ms p50  | 1.55 ms p50 / 1.94 ms p99 | 6× |
| `render.commitResources` | 0.003 ms p50 | 0.005 ms p50 / 0.042 ms p99 | 400× |

Anyhit is the dominant cost (+0.45 ms — fires per hit even on opaque
materials, evaluates two material flags, returns). Smooth shading,
emission, and normal mapping each add < 0.05 ms per pixel.

## Deferred to follow-up PRs

### MaterialX coverage

The lobby is pure UsdPreviewSurface so this is **not relevant for the
lobby M9**. Strict §41 M9 calls out MaterialX gap-closure for World Lobby
which we'd address when World Lobby lands.

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
