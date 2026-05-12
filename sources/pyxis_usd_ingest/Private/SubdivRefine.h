// Pyxis USD ingest — Catmull-Clark / Loop subdivision refinement.
//
// V2.A.1 / M12. UsdGeomMesh::subdivisionScheme = "catmullClark" or
// "loop" authors a polygon CAGE that USD-aware renderers refine to
// the limit surface. Pyxis v1 rendered the cage as-is, producing
// visible facets on every authored-as-subdiv surface (83 of the
// lobby's 942 meshes, mostly vases / banquettes / decorative trim).
//
// This helper wraps OpenSubdiv 3.5's CPU uniform-refinement path:
//   1. Build a Far::TopologyDescriptor from USD's (counts, indices).
//   2. Create a Far::TopologyRefiner under the matching SDC scheme.
//   3. RefineUniform to `maxLevel`.
//   4. Use Far::PrimvarRefiner to refine vertex positions + any
//      authored face-varying UVs through the levels.
//   5. Read the finest level back into VtArray-shaped output that
//      PrepareMesh consumes via its existing triangulation path.
//
// The boundary interpolation + face-varying interpolation modes are
// honoured per `UsdGeomMesh::interpolateBoundary` +
// `faceVaryingLinearInterpolation` attributes; defaults match the
// USD spec when the attributes aren't authored.

#pragma once

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usdGeom/mesh.h>

#include <cstdint>

namespace pyxis::usd_ingest {

// Output of a successful refinement. When `refined` is false the
// caller should use the original USD-authored arrays unchanged.
struct SubdivRefinedMesh {
  pxr::VtArray<pxr::GfVec3f> points;             // limit-surface positions
  pxr::VtArray<int>          faceVertexCounts;   // post-refinement (Catmark: all 4)
  pxr::VtArray<int>          faceVertexIndices;  // post-refinement
  pxr::VtArray<pxr::GfVec2f> faceVaryingUvs;     // empty when no st primvar refined
  bool refined = false;
};

// Refine a UsdGeomMesh if its subdivisionScheme is catmullClark or
// loop. `srcPoints` / `srcCounts` / `srcIndices` are the already-
// loaded cage arrays (PrepareMesh reads them once at the top).
// `srcUvs` may be empty (no st primvar) or non-empty (face-varying
// UV stream, length = sum of srcCounts when `stIsFaceVarying`, or
// length = srcPoints.size() when vertex-interp).
//
// Returns `refined=false` when the mesh is not subdiv-authored OR
// the OpenSubdiv refinement fails. Caller must fall back to the
// cage arrays in either case.
[[nodiscard]] SubdivRefinedMesh RefineSubdivMesh(
    const pxr::UsdGeomMesh& mesh,
    const pxr::VtArray<pxr::GfVec3f>& srcPoints,
    const pxr::VtArray<int>& srcCounts,
    const pxr::VtArray<int>& srcIndices,
    const pxr::VtArray<pxr::GfVec2f>& srcUvs,  // face-varying UVs; pass empty for non-FV or no UVs
    bool srcUvsAreFaceVarying,
    std::uint32_t maxLevel = 2) noexcept;

}  // namespace pyxis::usd_ingest
