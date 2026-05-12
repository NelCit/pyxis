// Pyxis USD ingest — analytic prim + curves + points tessellators.
//
// M13 / V2.A.3 + V2.A.21. UsdGeomMesh covers the vast majority of
// production geometry but real scenes also author:
//
//   * UsdGeomSphere / Cube / Cylinder / Cone / Capsule — analytic
//     primitives (proxy geom, procedural rigs, layout placeholders).
//   * UsdGeomBasisCurves — hair, foliage, wires.
//   * UsdGeomPoints — particles, scatter clouds.
//
// None of these are renderable by Pyxis's mesh-only path. We
// tessellate each to a triangle mesh at ingest and feed it through
// the existing MeshDesc → CreateMesh → AppendInstance pipeline. No
// new GpuScene handle types, no new BLAS shapes, no new closesthit
// code. Tessellation density is fixed (32-segment lat/long for
// spheres, etc.) and tuned so the resulting mesh is dense enough
// that flat-per-tri shading already looks smooth.
//
// Curves + points are tessellated as world-axis-aligned ribbons /
// billboards. Real renderers re-orient ribbons per-frame to face
// the camera; v2.0 ingests once with a fixed orientation, which is
// visually acceptable when curves are short or oriented around the
// scene roughly along a world axis. Per-frame re-orientation is a
// Pillar B post-task.

#pragma once

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/capsule.h>
#include <pxr/usd/usdGeom/cone.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/sphere.h>

namespace pyxis::usd_ingest {

// SOA result of any analytic-prim tessellation. Triangle-list
// topology (faceCounts trivial; left explicit so the caller can
// reuse the same downstream triangulator the mesh path uses).
// Normals + UVs are vertex-interp, sized to `points.size()`.
struct AnalyticGeomResult {
  pxr::VtArray<pxr::GfVec3f> points;      // local space (stage units)
  pxr::VtArray<int>          faceCounts;  // all 3 (triangle list)
  pxr::VtArray<int>          faceIndices; // triangle indices into `points`
  pxr::VtArray<pxr::GfVec3f> normals;     // vertex-interp, normalised
  pxr::VtArray<pxr::GfVec2f> uvs;         // vertex-interp, [0,1]² (best-effort)
  bool                       success = false;
};

// Each helper reads the prim's typed attributes (radius, height,
// axis, etc.) and emits a triangle mesh. `success=false` on
// degenerate inputs (radius<=0, etc.); caller falls back to skip.

[[nodiscard]] AnalyticGeomResult TessellateSphere(
    const pxr::UsdGeomSphere& sphere) noexcept;

[[nodiscard]] AnalyticGeomResult TessellateCube(
    const pxr::UsdGeomCube& cube) noexcept;

[[nodiscard]] AnalyticGeomResult TessellateCylinder(
    const pxr::UsdGeomCylinder& cylinder) noexcept;

[[nodiscard]] AnalyticGeomResult TessellateCone(
    const pxr::UsdGeomCone& cone) noexcept;

[[nodiscard]] AnalyticGeomResult TessellateCapsule(
    const pxr::UsdGeomCapsule& capsule) noexcept;

// Curves are tessellated as ribbon strips. The strip plane is set
// by `worldUp` (the orientation axis we use as a fallback for "face
// the camera"). Result is a triangle mesh; one quad per segment,
// width comes from the `widths` primvar (or 1.0 unit fallback).
[[nodiscard]] AnalyticGeomResult TessellateBasisCurves(
    const pxr::UsdGeomBasisCurves& curves,
    const pxr::GfVec3f& worldUp) noexcept;

// Points are tessellated as billboard quads in the world XY plane.
// One quad per point, width from `widths` primvar (or 1.0 fallback).
[[nodiscard]] AnalyticGeomResult TessellatePoints(
    const pxr::UsdGeomPoints& points,
    const pxr::GfVec3f& worldUp) noexcept;

}  // namespace pyxis::usd_ingest
