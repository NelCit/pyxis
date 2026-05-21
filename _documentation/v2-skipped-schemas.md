# v2 — Explicitly skipped USD schemas

Pillar A's loading completeness doctrine says: *every USD prim type
either has a handler OR an explicit-skip log. Silent drops fail the
milestone.* (plan_for_v2.md §V2.A.11, exit gate.)

This page lists every USD schema Pyxis v2 deliberately drops on the
floor, with the rationale. Each entry is reachable from the ingest
log: the `StageWalker` emits a one-line `Info` per encountered prim
of these types, naming the path + the reason. CI fails the milestone
if any of these silently land in the catch-all `unknown prim type`
bucket instead.

Future v3 picks from this list.

## UsdGeomHermiteCurves (PR6)

**Why skipped**: Hermite curves author per-vertex tangents alongside
positions; the proper tessellation is cubic Hermite interpolation
between adjacent vertex pairs. Pyxis v2.0's curves pipeline only
supports `UsdGeomBasisCurves` + `UsdGeomNurbsCurves` (the latter via
polyline-CV approximation; see V2.A.4 plan).

**Where it'd matter**: niche — Maya / Houdini / Modo all default to
BasisCurves for hair / foliage. Hermite curves show up mostly in
research / VFX pipelines that need tangent-controlled interpolation.

**Catch-up plan**: when production scenes start authoring Hermite
curves in volume, extend `AnalyticGeom.cpp` with a
`TessellateHermiteCurves` that consumes the `tangents` primvar +
emits a denser ribbon-strip tessellation.

## UsdGeomTetMesh (PR6)

**Why skipped**: TetMesh authors volumetric finite-element style
geometry (`tetVertexIndices` over a `points` array, forming
tetrahedra rather than triangles). Rendering tetmeshes requires
either a volumetric integrator (ray-march through the tet partition)
or a surface-extraction pass (marching tets producing a triangle
shell). Neither ships at v2.0.

**Where it'd matter**: simulation / FEM / soft-body authoring
pipelines. Not present in any v1 / v2 target scene (World Lobby,
OpenPBR Playground, M12 lobby variants).

**Catch-up plan**: surface extraction (marching tets) is the cheap
v3 path; full volumetric integration ties in with §V2.A.5's
OpenVDB volume integrator follow-up.

## (Future entries)

Any v3 / v4 schema we explicitly drop lands here. Maintain the
log-line-per-encounter invariant: every skipped type must surface
exactly once in stdout so an operator never has to grep for
"why didn't my prim render".
