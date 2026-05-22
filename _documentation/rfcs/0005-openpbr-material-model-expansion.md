# RFC 0005: Generalized planar projection (world + object space)

- Status: Draft
- Author(s): Pyxis renderer team
- Created: 2026-05-22
- Last updated: 2026-05-22
- Implementation PRs: (in progress)
- Supersedes: the V2.B wood-name world-projection patch in `TranslateMdl`

## Summary

Do **not** expand the OpenPBR BSDF surface. OpenPBR's parameter set (§11)
is the canonical, sufficient material model. The OmniPBR/MDL inputs the
converter drops are folded into existing OpenPBR parameters at translate
time or are real-time approximations a ray tracer doesn't carry as material
state. The one OmniPBR concern that is genuinely *not* a BSDF parameter and
is actually used in the World Lobby is **planar projection**: `project_uvw`
(on/off) and `world_or_object` (which space). This RFC generalizes the
existing `worldProjection` field (shipped V2.B, values 0/1) to
`projectionMode` (0 = none, 1 = world, 2 = object) — **reusing the same POD
slot**, so it is not an ABI expansion — drives it globally from the authored
inputs (removing the wood-name hack), and adds the object-space branch in
the closesthit. The secondary-UV-set idea from the earlier draft is
**dropped**: measurement (below) shows it is a no-op for the lobby.

## Motivation

The earlier 0005 draft proposed a large material-model expansion (sheen,
detail, AO, influences) and a `uv_space_index`/`uv1` second-UV pipeline to
fix oak-grain orientation. Both were rejected after grounding the design in
the actual scene. A temporary probe over the World Lobby's 122 MDL
materials and 951 meshes found:

- **`uv_space_index` is never `1`.** 110 materials author `0`, 12 leave it
  unauthored, zero author `1`. Oak/OakDark author `0`.
- **The 36 meshes that carry a second UV set (`st`+`st_1`) are all
  vegetation** (Flowers, Leaves, Pampas, …), never wood — and their
  materials still sample set 0. Honouring `uv_space_index`/`uv1` would
  therefore change **nothing** in this scene. The §42 multi-UV work stays
  deferred; it is not what fixes oak.
- **`project_uvw = 1` on 10 materials** — tiles/stone/metal/paint
  (Ceramic_Tile, Marble_Tile, RustedMetal, RustedMetalFrame, Slate,
  Steel_Carbon, Paint_Satin, Mulch_Brown) — of which **3 author
  `world_or_object = 1`** (object-space): Ceramic_Tile_18, Steel_Carbon,
  Mulch_Brown. This is the validated, genuinely-used need.
- **Oak/OakDark author `project_uvw = 0`, `world_or_object = 0`,
  `uv_space_index = 0`.** They are unwrapped on `st` and rely on
  `texture_rotate`/`texture_scale` (which Pyxis applies). The oak-grain
  question is therefore **out of scope for this RFC** — it is not a
  projection or UV-set issue. See "Oak grain" below.

How the other dropped OmniPBR inputs are handled without new fields:

| OmniPBR input(s) | Disposition (no new field) |
|---|---|
| `specular_level` | → existing `specularWeight` |
| `albedo_brightness` / `albedo_add` | fold into `baseColor` at convert time |
| `ao_texture` / `ao_to_diffuse` | dropped — ray tracer computes real occlusion |
| `*_texture_influence`, `detail_*` | dropped — no OpenPBR equivalent |
| `emissive_color_texture` | existing `emissionMap` slot (source fix) |

## Detailed design

### 1. `OpenPBRMaterialDesc` — widen one existing field (§18.4)

```cpp
  // RFC 0005 — supersedes `worldProjection` (V2.B, 0/1). Same POD slot.
  // 0 = none (use mesh UVs), 1 = world-space planar, 2 = object-space planar.
  uint32_t projectionMode = 0;   // was `worldProjection`
```

Offset/size unchanged (a `uint32_t` where the `uint32_t worldProjection`
was), so `_tools/check_exports.py` is unaffected (PODs aren't exported
symbols). Rename is a source-compat touch — internal callers only
(converter + the flag-packing site). `_reserved[2]` is untouched.

### 2. `MaterialFlag` (§11.6) — one new bit

```
ObjectProjection = 1u << 17   // projectionMode == 2
```

`WorldProjection` (1u<<16) is retained for `projectionMode == 1`. The
flag-packing site sets exactly one of the two from `projectionMode`.

### 3. Converter — global, no wood hack

```cpp
  const bool projectUvw  = ReadBoolish(shader, TfToken("project_uvw"), false, timeCode);
  const bool objectSpace = ReadBoolish(shader, TfToken("world_or_object"), false, timeCode);
  desc.projectionMode = projectUvw ? (objectSpace ? 2u : 1u) : 0u;
```

(The wood-name `sourceAsset` sniff is already removed.)

### 4. Closesthit (§11, one generic shader)

- `WorldProjection`: world-space planar UV from the world hit position
  (already implemented).
- `ObjectProjection`: identical, but the hit position is first transformed
  by `WorldToObject3x4()` so the projection follows the instance — the
  correct behaviour for tiled props placed at many transforms (the 3
  object-space materials are tiles/mulch on repeated props).

No new BSDF lobes; no vertex-layout or `MeshDesc` change.

## Alternatives considered

1. **Expand the desc (sheen/coat/detail/AO) + add `uv1`.** Rejected: OpenPBR
   is the canonical model, and measurement shows `uv1` is a no-op here.
2. **Keep the wood-name hack.** Rejected: a name-based special case that
   misrenders differently-named wood and applies projection a material never
   authored.
3. **Honour `project_uvw` only, skip object space.** Rejected: 3 materials
   genuinely author `world_or_object = 1`; world-projecting them is visibly
   wrong on repeated props (the projection wouldn't follow the instance).

## Drawbacks / risks

- **Regression images:** the 10 `project_uvw` materials now project from
  authored inputs (3 in object space) instead of never; oak is unchanged by
  this RFC (it authors `project_uvw=0`). Goldens for the affected tile/
  stone/metal materials re-bake. Determinism (§33.7) unaffected (no RNG).
- **Source-compat:** `worldProjection` → `projectionMode` rename; internal
  callers only, pre-1.0.
- **Perf:** one extra `WorldToObject` transform on object-projected hits;
  negligible, gated by flag.

## Migration & impact

- **Done:** wood-name hack removed; `worldProjection` driven from
  `project_uvw`.
- **This PR:** rename to `projectionMode`; add `ObjectProjection` flag;
  converter reads `world_or_object`; closesthit object-space branch.
- **Affected milestones:** M9 (tile/stone projection correctness). The §42
  multi-UV item stays deferred (not needed). Oak grain tracked separately.
- **Who:** renderer + material-translation owners.

## Findings (formerly open questions — now answered by measurement)

- **UV-set theory: falsified.** `uv_space_index` is never `1`; the only
  second-UV meshes are vegetation whose materials sample set 0. Dropped.
- **UV-set count:** moot — feature dropped.
- **Object-space demand:** real — 3 materials. Implemented (not deferred).

## Oak grain (out of scope — separate investigation)

Oak authors `project_uvw=0`, `world_or_object=0`, `uv_space_index=0`, so the
grain orientation is governed by the `st` unwrap plus the authored
`texture_rotate`/`texture_scale` (both applied by Pyxis). Removing the
wood-name hack reverts oak to that path. Whether it now matches Omniverse is
a visual question to confirm against a reference render; if it diverges, the
cause is in the `st` unwrap or the texture-transform application, **not**
projection or UV-set selection. Tracked outside this RFC.
