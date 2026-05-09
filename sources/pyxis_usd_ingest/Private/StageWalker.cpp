// Pyxis USD-direct ingest — StageWalker implementation.
//
// Walks a UsdStage in SdfPath-sorted order (the §25.O.3 P0 byte-equal
// invariant) and translates UsdGeomMesh + UsdGeomCamera + UsdShadeMaterial
// prims into GpuScene mutations. Lights are recognised + counted but
// not yet wired into GpuScene at M5 — they land at M7 when NEE + MIS
// arrive.
//
// Two-pass walk: materials first (so instances can reference them by
// MaterialHandle), then meshes / camera / lights. The first pass
// builds a SdfPath → MaterialHandle map keyed on the material prim's
// path; the second pass resolves each mesh's bound material via
// UsdShadeMaterialBindingAPI and stamps the resulting MaterialHandle
// onto its InstanceDesc.
//
// Triangulation: M5 still handles only triangle-list meshes
// (faceVertexCounts entirely 3). Quads / ngons fan-triangulate at M6+
// when the §25.O.1 inline triangulator lands. The fixtures used at
// M5 (default.usd) are authored as triangle lists already.

#include "Pyxis/UsdIngest/StageWalker.h"

#include <Pyxis/MaterialTranslation/UsdShadeToOpenPBR.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/InstanceDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>
#include <Pyxis/Renderer/GpuScene.h>

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>

#include <hlsl++.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

using MaterialHandleByPath = std::unordered_map<std::string, MaterialHandle>;
using ConsumedPrototypePaths = std::unordered_set<std::string>;

// Build a Pyxis MeshDesc from a UsdGeomMesh prim. Returns
// std::nullopt for ngon-only / empty / invalid meshes — the M5
// triangle-list constraint is shared with EmitMesh's main path.
struct ProtoMeshBuffers {
  std::vector<hlslpp::float3> positions;
  std::vector<std::uint32_t>  indices;
};
[[nodiscard]] bool ExtractTriangleListMesh(const pxr::UsdGeomMesh& meshPrim,
                                           ProtoMeshBuffers& out) noexcept {
  pxr::VtArray<pxr::GfVec3f> usdPoints;
  pxr::VtArray<int> usdCounts;
  pxr::VtArray<int> usdIndices;
  meshPrim.GetPointsAttr().Get(&usdPoints);
  meshPrim.GetFaceVertexCountsAttr().Get(&usdCounts);
  meshPrim.GetFaceVertexIndicesAttr().Get(&usdIndices);

  if (usdPoints.empty() || usdCounts.empty() || usdIndices.empty())
    return false;
  for (const int faceCount : usdCounts)
  {
    if (faceCount != 3)
      return false;
  }

  out.positions.clear();
  out.positions.reserve(usdPoints.size());
  for (const pxr::GfVec3f& point : usdPoints)
    out.positions.emplace_back(point[0], point[1], point[2]);

  out.indices.clear();
  out.indices.reserve(usdIndices.size());
  for (const int idx : usdIndices)
    out.indices.push_back(static_cast<uint32_t>(idx));
  return true;
}

// Resolve `meshPrim`'s bound UsdShadeMaterial via the
// MaterialBindingAPI. Returns MaterialHandle::Invalid if no binding
// is authored, the binding points at a material we never translated
// (e.g. a different render context than the one we walked), or the
// caller passed an empty map. Lookup is by SdfPath string so the
// key matches whatever the first pass populated.
MaterialHandle ResolveBoundMaterial(const pxr::UsdPrim& meshPrim,
                                    const MaterialHandleByPath& materialsByPath) noexcept
{
  if (materialsByPath.empty())
    return MaterialHandle::Invalid;
  const pxr::UsdShadeMaterialBindingAPI bindingApi(meshPrim);
  const pxr::UsdShadeMaterial bound = bindingApi.ComputeBoundMaterial();
  if (!bound.GetPrim().IsValid())
    return MaterialHandle::Invalid;
  const std::string boundPath = bound.GetPrim().GetPath().GetString();
  const auto found = materialsByPath.find(boundPath);
  if (found == materialsByPath.end())
    return MaterialHandle::Invalid;
  return found->second;
}

// M6: walk a UsdGeomPointInstancer and expand it into N AppendInstance
// calls against ONE prototype mesh per `prototypes` rel target. The
// prototype meshes are registered exactly once (BLAS sharing per §15
// kicks in here — N TLAS instances all reference the same MeshHandle
// → same BLAS), and their SdfPaths get added to `consumedPrototypes`
// so the main mesh pass doesn't double-emit them as standalone meshes.
//
// Per-instance transform per the UsdGeomPointInstancer spec:
//     world = instancerWorldFromLocal · translate(positions[i])
//             · rotate(orientations[i]) · scale(scales[i])
// computed in USD's row-vector convention; ToPyxisMatrix() handles
// the column-vector flip downstream.
//
// Defers to M7+: invisibleIds / inactiveIds masking, velocities /
// accelerations / angularVelocities (animation, post-v1 §42),
// non-Mesh prototypes (Xform-wrapped hierarchies — log + skip).
void EmitPointInstancer(const pxr::UsdPrim& instancerPrim,
                        pxr::UsdGeomXformCache& xformCache,
                        const MaterialHandleByPath& materialsByPath,
                        GpuScene& scene, IngestStats& stats,
                        ConsumedPrototypePaths& consumedPrototypes) noexcept {
  auto& log = Logging::Get();
  const pxr::UsdGeomPointInstancer instancer(instancerPrim);
  if (!instancer.GetPrim().IsValid())
    return;

  // Prototype targets — a relationship pointing at one or more
  // SdfPaths. Each target is the root of a prototype hierarchy.
  pxr::SdfPathVector prototypePaths;
  instancer.GetPrototypesRel().GetTargets(&prototypePaths);
  if (prototypePaths.empty())
  {
    log.Warn(log::APP, "StageWalker: PointInstancer "
                           + instancerPrim.GetPath().GetString()
                           + " has no prototypes — skipping.");
    return;
  }

  // Register each prototype mesh once. Build parallel arrays of
  // MeshHandle + bound MaterialHandle indexed by prototype index;
  // the per-instance loop below indexes into these via protoIndices.
  std::vector<MeshHandle> protoMeshes(prototypePaths.size(), MeshHandle::Invalid);
  std::vector<MaterialHandle> protoMaterials(prototypePaths.size(),
                                             MaterialHandle::Invalid);
  for (std::size_t protoIdx = 0; protoIdx < prototypePaths.size(); ++protoIdx)
  {
    const pxr::SdfPath& protoPath = prototypePaths[protoIdx];
    const pxr::UsdPrim protoPrim = instancerPrim.GetStage()->GetPrimAtPath(protoPath);
    if (!protoPrim.IsValid())
      continue;
    consumedPrototypes.emplace(protoPath.GetString());

    // M6 limitation: only direct UsdGeomMesh prototypes (no nested
    // Xform-wrapped hierarchies). Bistro foliage prototypes typically
    // ARE direct meshes; the deeper nested-prototype case lands at M9.
    if (!protoPrim.IsA<pxr::UsdGeomMesh>())
    {
      log.Warn(log::APP, "StageWalker: PointInstancer prototype "
                             + protoPath.GetString()
                             + " is not a UsdGeomMesh (M6 supports direct mesh "
                               "prototypes only) — skipping.");
      continue;
    }
    const pxr::UsdGeomMesh meshPrim(protoPrim);
    ProtoMeshBuffers buffers;
    if (!ExtractTriangleListMesh(meshPrim, buffers))
    {
      log.Warn(log::APP, "StageWalker: PointInstancer prototype "
                             + protoPath.GetString()
                             + " is empty or non-triangle-list — skipping.");
      continue;
    }
    MeshDesc meshDesc;
    meshDesc.positions = buffers.positions;
    meshDesc.indices = buffers.indices;
    meshDesc.debugName = protoPath.GetString();
    const auto meshHandle = scene.CreateMesh(meshDesc);
    if (!meshHandle.has_value())
    {
      log.Error(log::APP, "StageWalker: PointInstancer CreateMesh failed for "
                              + protoPath.GetString() + ": "
                              + std::string{meshHandle.error().message.View()});
      continue;
    }
    protoMeshes[protoIdx] = *meshHandle;
    protoMaterials[protoIdx] = ResolveBoundMaterial(protoPrim, materialsByPath);
    ++stats.meshesEmitted;
  }

  // Per-instance transforms.
  pxr::VtArray<int> protoIndices;
  pxr::VtArray<pxr::GfVec3f> positions;
  pxr::VtArray<pxr::GfQuath> orientations;
  pxr::VtArray<pxr::GfVec3f> scales;
  instancer.GetProtoIndicesAttr().Get(&protoIndices);
  instancer.GetPositionsAttr().Get(&positions);
  instancer.GetOrientationsAttr().Get(&orientations);
  instancer.GetScalesAttr().Get(&scales);

  if (protoIndices.empty())
  {
    log.Warn(log::APP, "StageWalker: PointInstancer "
                           + instancerPrim.GetPath().GetString()
                           + " has no protoIndices — no instances emitted.");
    return;
  }
  // positions, orientations, scales are optional per the spec —
  // identity defaults apply when absent. Resize-or-skip the optionals
  // so the per-instance loop is uniform.
  const std::size_t instanceCount = protoIndices.size();
  if (!positions.empty() && positions.size() != instanceCount)
  {
    log.Warn(log::APP, "StageWalker: PointInstancer "
                           + instancerPrim.GetPath().GetString()
                           + " positions length mismatch — skipping.");
    return;
  }

  const pxr::GfMatrix4d instancerWorld =
      xformCache.GetLocalToWorldTransform(instancerPrim);

  ++stats.instancersEmitted;
  for (std::size_t instIdx = 0; instIdx < instanceCount; ++instIdx)
  {
    const int proto = protoIndices[instIdx];
    if (proto < 0 || static_cast<std::size_t>(proto) >= protoMeshes.size())
      continue;
    const MeshHandle protoMesh = protoMeshes[proto];
    if (protoMesh == MeshHandle::Invalid)
      continue;

    // S · R · T · instancerWorld — USD row-vector convention.
    pxr::GfMatrix4d perInstance;
    perInstance.SetIdentity();
    if (!scales.empty())
    {
      pxr::GfMatrix4d scaleMat;
      scaleMat.SetScale(pxr::GfVec3d(scales[instIdx][0], scales[instIdx][1],
                                     scales[instIdx][2]));
      perInstance = perInstance * scaleMat;
    }
    if (!orientations.empty())
    {
      pxr::GfMatrix4d rotMat;
      rotMat.SetRotate(pxr::GfRotation(pxr::GfQuatd(orientations[instIdx])));
      perInstance = perInstance * rotMat;
    }
    if (!positions.empty())
    {
      pxr::GfMatrix4d transMat;
      transMat.SetTranslate(pxr::GfVec3d(positions[instIdx][0],
                                         positions[instIdx][1],
                                         positions[instIdx][2]));
      perInstance = perInstance * transMat;
    }
    const pxr::GfMatrix4d worldFromLocal = perInstance * instancerWorld;

    InstanceDesc instanceDesc;
    instanceDesc.mesh = protoMesh;
    instanceDesc.material = protoMaterials[proto];
    instanceDesc.worldFromLocal = ToPyxisMatrix(worldFromLocal);
    instanceDesc.debugName = instancerPrim.GetPath().GetString();
    const auto instanceHandle = scene.AppendInstance(instanceDesc);
    if (!instanceHandle.has_value())
    {
      // TLAS-cap exhaustion or similar — log once + bail.
      log.Error(log::APP, "StageWalker: PointInstancer AppendInstance failed: "
                              + std::string{instanceHandle.error().message.View()});
      return;
    }
    ++stats.instancesEmitted;
  }
}

// Emit a single UsdGeomMesh into `scene`. Returns true iff the mesh
// was emitted (false skips the matching instance increment). M4
// constraint: all faceVertexCounts must equal 3 (triangle list); ngons
// are dropped with a single Warn log per prim.
bool EmitMesh(const pxr::UsdPrim& prim, pxr::UsdGeomXformCache& xformCache,
              const MaterialHandleByPath& materialsByPath, GpuScene& scene) noexcept {
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

  // World transform → instance. Material binding (if any) is
  // resolved via UsdShadeMaterialBindingAPI and stamped into
  // InstanceDesc::material so the closesthit can read
  // materials[InstanceID()] (where InstanceID is the material slot
  // — see GpuScene::CommitResources's TLAS instance pack).
  const pxr::GfMatrix4d worldFromLocal = xformCache.GetLocalToWorldTransform(prim);
  InstanceDesc instanceDesc;
  instanceDesc.mesh = *meshHandle;
  instanceDesc.worldFromLocal = ToPyxisMatrix(worldFromLocal);
  instanceDesc.material = ResolveBoundMaterial(prim, materialsByPath);
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
               + std::to_string(stats.instancesEmitted) + " instances ("
               + std::to_string(stats.instancersEmitted) + " instancers), "
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

  // Pass 1 — materials. Translate every UsdShadeMaterial to an
  // OpenPBRMaterialDesc and AcquireMaterial it; record the resulting
  // handle by SdfPath so the mesh pass can resolve bindings without
  // re-walking the stage. Both adapters share this code path
  // (HydraEngine routes through StageWalker at M5 — see HydraEngine.h)
  // so any difference in material translation breaks the §25.O.3
  // byte-equal invariant.
  MaterialHandleByPath materialsByPath;
  for (const pxr::UsdPrim& prim : prims)
  {
    if (!prim.IsA<pxr::UsdShadeMaterial>())
      continue;
    const pxr::UsdShadeMaterial materialPrim(prim);
    const std::string primPath = prim.GetPath().GetString();
    OpenPBRMaterialDesc materialDesc =
        material_translation::FromUsdShade(materialPrim);
    materialDesc.sourcePrim = primPath;
    const MaterialHandle handle = scene.AcquireMaterial(materialDesc);
    materialsByPath.emplace(primPath, handle);
    ++stats.materialsEmitted;
  }

  // Pass 2 — UsdGeomPointInstancer (M6). Walked BEFORE the standalone
  // mesh pass so any prototype meshes referenced by an instancer get
  // registered + recorded in `consumedPrototypes`; the mesh pass then
  // skips them so we don't emit a non-instanced duplicate of each
  // prototype. SdfPath-sorted iteration order is preserved per
  // §25.O.3 so both adapters expand instancers identically.
  ConsumedPrototypePaths consumedPrototypes;
  for (const pxr::UsdPrim& prim : prims)
  {
    if (!prim.IsA<pxr::UsdGeomPointInstancer>())
      continue;
    EmitPointInstancer(prim, xformCache, materialsByPath, scene, stats,
                       consumedPrototypes);
  }

  // Pass 3 — meshes / camera / lights, with material handles resolved
  // from pass 1 and instancer prototypes skipped (pass 2 already
  // expanded them).
  for (const pxr::UsdPrim& prim : prims)
  {
    if (prim.IsA<pxr::UsdGeomMesh>())
    {
      if (consumedPrototypes.contains(prim.GetPath().GetString()))
        continue;
      if (EmitMesh(prim, xformCache, materialsByPath, scene))
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
      // M5 stub: lights count but aren't yet pushed into GpuScene
      // (the path-trace closesthit ignores lights at M5 — material
      // baseColor only; M7 wires light sampling).
      ++stats.lightsEmitted;
    }
    else if (prim.IsA<pxr::UsdShadeMaterial>())
    {
      // Already translated + counted in pass 1.
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
