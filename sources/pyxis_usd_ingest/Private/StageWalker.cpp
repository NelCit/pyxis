// Pyxis USD-direct ingest — StageWalker implementation.
//
// Walks a UsdStage in SdfPath-sorted order (the §25.O.3 P0 byte-equal
// invariant) and translates UsdGeomMesh + UsdGeomCamera prims into
// GpuScene mutations. Lights / materials are recognised + counted but
// not yet wired into GpuScene at M4 — they land at M7 (lights) / M5
// (materials) when the renderer-side closesthit consumes them.
//
// Triangulation: M4 only handles meshes whose faces are all triangles
// (faceVertexCounts entirely 3). Quads / ngons fan-triangulate at M5+
// when the §25.O.1 inline triangulator lands. The M4 fixture uses
// triangle-list meshes only, which keeps both adapters in lockstep.

#include "Pyxis/UsdIngest/StageWalker.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/InstanceDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/GpuScene.h>

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>

#include <hlsl++.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace pyxis::usd_ingest {

namespace {

// Convert USD's row-major double-precision matrix to Pyxis's
// column-vector + row-major float4x4 (plan §10). USD's GfMatrix4d
// stores in row-major order; hlslpp::float4x4 is also row-major
// when fed via the (row0, row1, row2, row3) ctor. Translation lives
// in row 3 of GfMatrix4d (USD's row-vector convention) and column 3
// of the §10 column-vector form, so we transpose.
hlslpp::float4x4 ToPyxisMatrix(const pxr::GfMatrix4d& usdMatrix) noexcept {
  // GfMatrix4d M[row][col]; v_world = v_local * M (USD row-vector).
  // Pyxis: v_world = M_pyxis * v_local; translation in last column.
  // Transposing flips row<->column.
  return hlslpp::float4x4(
      hlslpp::float4{static_cast<float>(usdMatrix[0][0]),
                     static_cast<float>(usdMatrix[1][0]),
                     static_cast<float>(usdMatrix[2][0]),
                     static_cast<float>(usdMatrix[3][0])},
      hlslpp::float4{static_cast<float>(usdMatrix[0][1]),
                     static_cast<float>(usdMatrix[1][1]),
                     static_cast<float>(usdMatrix[2][1]),
                     static_cast<float>(usdMatrix[3][1])},
      hlslpp::float4{static_cast<float>(usdMatrix[0][2]),
                     static_cast<float>(usdMatrix[1][2]),
                     static_cast<float>(usdMatrix[2][2]),
                     static_cast<float>(usdMatrix[3][2])},
      hlslpp::float4{static_cast<float>(usdMatrix[0][3]),
                     static_cast<float>(usdMatrix[1][3]),
                     static_cast<float>(usdMatrix[2][3]),
                     static_cast<float>(usdMatrix[3][3])});
}

// Emit a single UsdGeomMesh into `scene`. Returns true iff the mesh
// was emitted (false skips the matching instance increment). M4
// constraint: all faceVertexCounts must equal 3 (triangle list); ngons
// are dropped with a single Warn log per prim.
bool EmitMesh(const pxr::UsdPrim& prim, pxr::UsdGeomXformCache& xformCache,
              GpuScene& scene) noexcept {
  auto& log = Logging::Get();
  const pxr::UsdGeomMesh meshPrim(prim);
  if (!meshPrim.GetPrim().IsValid())
    return false;

  pxr::VtArray<pxr::GfVec3f> usdPoints;
  pxr::VtArray<int> usdCounts;
  pxr::VtArray<int> usdIndices;
  meshPrim.GetPointsAttr().Get(&usdPoints);
  meshPrim.GetFaceVertexCountsAttr().Get(&usdCounts);
  meshPrim.GetFaceVertexIndicesAttr().Get(&usdIndices);

  if (usdPoints.empty() || usdCounts.empty() || usdIndices.empty())
    return false;

  // M4 stub triangulation: every face must already be a triangle.
  for (const int faceCount : usdCounts)
  {
    if (faceCount != 3)
    {
      log.Warn(log::APP,
               "StageWalker: skipping non-triangle mesh " + prim.GetPath().GetString()
                   + " (face has " + std::to_string(faceCount)
                   + " vertices; M4 supports triangle-list only).");
      return false;
    }
  }

  // Convert positions + indices into the §18.4 contiguous layout.
  std::vector<hlslpp::float3> positions;
  positions.reserve(usdPoints.size());
  for (const pxr::GfVec3f& point : usdPoints)
    positions.emplace_back(point[0], point[1], point[2]);

  std::vector<uint32_t> indices;
  indices.reserve(usdIndices.size());
  for (const int idx : usdIndices)
    indices.push_back(static_cast<uint32_t>(idx));

  const std::string debugName = prim.GetPath().GetString();
  MeshDesc meshDesc;
  meshDesc.positions = positions;
  meshDesc.indices = indices;
  meshDesc.debugName = debugName;
  const auto meshHandle = scene.CreateMesh(meshDesc);
  if (!meshHandle.has_value())
  {
    log.Error(log::APP, "StageWalker: CreateMesh failed for "
                            + prim.GetPath().GetString() + ": "
                            + std::string{meshHandle.error().message.View()});
    return false;
  }

  // World transform → instance.
  const pxr::GfMatrix4d worldFromLocal = xformCache.GetLocalToWorldTransform(prim);
  InstanceDesc instanceDesc;
  instanceDesc.mesh = *meshHandle;
  instanceDesc.worldFromLocal = ToPyxisMatrix(worldFromLocal);
  instanceDesc.debugName = debugName;
  const auto instanceHandle = scene.AppendInstance(instanceDesc);
  if (!instanceHandle.has_value())
  {
    log.Error(log::APP, "StageWalker: AppendInstance failed for "
                            + prim.GetPath().GetString() + ": "
                            + std::string{instanceHandle.error().message.View()});
    return false;
  }
  return true;
}

// Emit a UsdGeomCamera as the active camera. The plan §29.4.a default
// scene only has one camera; the M4 contract is "first camera in
// SdfPath-sorted order wins" so both adapters pick the same one.
void EmitCamera(const pxr::UsdPrim& prim, pxr::UsdGeomXformCache& xformCache,
                GpuScene& scene) noexcept {
  const pxr::UsdGeomCamera cameraPrim(prim);
  if (!cameraPrim.GetPrim().IsValid())
    return;

  // GetCamera returns a GfCamera at the default time code.
  const pxr::GfCamera gfCamera = cameraPrim.GetCamera(pxr::UsdTimeCode::Default());
  const pxr::GfMatrix4d worldFromLocal = xformCache.GetLocalToWorldTransform(prim);

  CameraDesc cameraDesc;
  // viewFromWorld = inverse(worldFromLocal). USD's GfMatrix4d.
  // GetInverse() handles the inversion.
  const pxr::GfMatrix4d viewFromWorld = worldFromLocal.GetInverse();
  cameraDesc.viewFromWorld = ToPyxisMatrix(viewFromWorld);
  // Projection: GfCamera::GetFrustum().ComputeProjectionMatrix() at
  // M5+ when the perspective/ortho switch matters. M4 stub: ship a
  // simple perspective projection from focalLength + aperture so
  // both adapters compute identically.
  cameraDesc.projFromView = ToPyxisMatrix(gfCamera.GetFrustum().ComputeProjectionMatrix());
  cameraDesc.focalLengthMm = gfCamera.GetFocalLength();
  cameraDesc.apertureFStop = gfCamera.GetFStop();
  cameraDesc.focusDistance = gfCamera.GetFocusDistance();
  const pxr::GfRange1f clipRange = gfCamera.GetClippingRange();
  cameraDesc.nearClip = clipRange.GetMin();
  cameraDesc.farClip = clipRange.GetMax();

  scene.SetCamera(cameraDesc);
}

}  // namespace

IngestStats StageWalker::WalkFile(std::string_view usdPath, GpuScene& scene) {
  auto& log = Logging::Get();
  const std::string pathString{usdPath};
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(pathString);
  if (!stage)
  {
    log.Error(log::APP, std::string{"StageWalker: failed to open "} + pathString);
    return {};
  }
  const IngestStats stats = WalkStage(stage, scene);
  log.Info(log::APP,
           "StageWalker: " + pathString + " walked — "
               + std::to_string(stats.meshesEmitted) + " meshes, "
               + std::to_string(stats.instancesEmitted) + " instances, "
               + std::to_string(stats.lightsEmitted) + " lights, "
               + std::to_string(stats.camerasEmitted) + " cameras, "
               + std::to_string(stats.skipped) + " skipped.");
  return stats;
}

IngestStats StageWalker::WalkStage(const pxr::UsdStageRefPtr& stage,
                                   GpuScene& scene) {
  IngestStats stats{};
  if (!stage)
    return stats;

  // Collect every prim, sort by SdfPath. §25.O.3: SdfPath-sorted is
  // the P0 byte-equal invariant — both adapters must emit instances
  // in this exact order.
  std::vector<pxr::UsdPrim> prims;
  for (const pxr::UsdPrim& prim : stage->Traverse())
    prims.push_back(prim);
  std::stable_sort(prims.begin(), prims.end(),
                   [](const pxr::UsdPrim& lhs, const pxr::UsdPrim& rhs) {
                     return lhs.GetPath() < rhs.GetPath();
                   });

  pxr::UsdGeomXformCache xformCache;
  bool cameraSet = false;

  for (const pxr::UsdPrim& prim : prims)
  {
    if (prim.IsA<pxr::UsdGeomMesh>())
    {
      if (EmitMesh(prim, xformCache, scene))
      {
        ++stats.meshesEmitted;
        ++stats.instancesEmitted;
      }
    }
    else if (prim.IsA<pxr::UsdGeomCamera>())
    {
      if (!cameraSet)
      {
        EmitCamera(prim, xformCache, scene);
        cameraSet = true;
      }
      ++stats.camerasEmitted;
    }
    else if (prim.IsA<pxr::UsdLuxDistantLight>() || prim.IsA<pxr::UsdLuxDomeLight>()
             || prim.IsA<pxr::UsdLuxRectLight>())
    {
      // M4 stub: lights count but aren't yet pushed into GpuScene
      // (the path-trace closesthit ignores lights at M4 — barycentric
      // visualisation only; M7 wires light sampling).
      ++stats.lightsEmitted;
    }
    else if (prim.IsA<pxr::UsdShadeMaterial>())
    {
      // M4 stub: materials count but aren't yet pushed (closesthit
      // ignores materials too at M4; M5 wires the OpenPBR branch).
      ++stats.materialsEmitted;
    }
    else
    {
      if (!prim.GetTypeName().IsEmpty() && prim.GetTypeName().GetString() != "Xform"
          && prim.GetTypeName().GetString() != "Scope")
      {
        ++stats.skipped;
      }
    }
  }

  return stats;
}

}  // namespace pyxis::usd_ingest
