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
#include <Pyxis/Renderer/Descs/LightDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>
#include <Pyxis/Renderer/Descs/TextureKey.h>
#include <Pyxis/Renderer/GpuScene.h>

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/base/gf/matrix4d.h>

#include <cmath>
#include <optional>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>

#include <hlsl++.h>

#include <algorithm>
#include <chrono>
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

// StageContext — derived once per WalkStage from the stage's own
// metadata, threaded through every Emit* path. Captures the
// stage-to-world correction matrix that bakes:
//   1. metersPerUnit scale (Pyxis works in metres; USD authors in
//      whatever the asset shipped with — Omniverse "Collected"
//      scenes are typically in centimetres, metersPerUnit = 0.01).
//   2. up-axis remap if the scene authors Z-up (most Omniverse +
//      DCC content); Pyxis convention is Y-up.
// Composes onto every per-prim worldFromLocal so the renderer sees a
// uniform Y-up + metres world regardless of how the asset shipped.
struct StageContext
{
  hlslpp::float4x4 stageToWorld;  // R_zy(if Z-up) * Scale(metersPerUnit)
  bool             zUp           = false;
  float            metersPerUnit = 1.0f;
};

// Build the stage-to-world correction matrix from the stage metadata.
// Pure function of stage-level tokens — doesn't touch any prim.
StageContext BuildStageContext(const pxr::UsdStageRefPtr& stage) noexcept
{
  StageContext ctx;
  if (!stage)
  {
    ctx.stageToWorld = hlslpp::float4x4::identity();
    return ctx;
  }
  ctx.metersPerUnit = static_cast<float>(pxr::UsdGeomGetStageMetersPerUnit(stage));
  if (ctx.metersPerUnit <= 0.0f)
    ctx.metersPerUnit = 1.0f;  // defensive — corrupt metadata
  const pxr::TfToken upAxis = pxr::UsdGeomGetStageUpAxis(stage);
  ctx.zUp = (upAxis == pxr::UsdGeomTokens->z);

  // Scale-by-metersPerUnit matrix (uniform diagonal).
  const float scaleFactor = ctx.metersPerUnit;
  const hlslpp::float4x4 scale(
      hlslpp::float4{scaleFactor, 0.0f,        0.0f,        0.0f},
      hlslpp::float4{0.0f,        scaleFactor, 0.0f,        0.0f},
      hlslpp::float4{0.0f,        0.0f,        scaleFactor, 0.0f},
      hlslpp::float4{0.0f,        0.0f,        0.0f,        1.0f});

  if (ctx.zUp)
  {
    // Z-up → Y-up rotation. Maps stage basis vectors to Pyxis world:
    //   stage +X → pyxis +X
    //   stage +Y → pyxis -Z   (rotation around X by -90°)
    //   stage +Z → pyxis +Y
    // Column-vector form (Pyxis convention §10): matrix columns are
    // where the basis vectors land.
    const hlslpp::float4x4 rot(
        hlslpp::float4{1.0f, 0.0f,  0.0f, 0.0f},
        hlslpp::float4{0.0f, 0.0f,  1.0f, 0.0f},
        hlslpp::float4{0.0f, -1.0f, 0.0f, 0.0f},
        hlslpp::float4{0.0f, 0.0f,  0.0f, 1.0f});
    ctx.stageToWorld = mul(rot, scale);
  }
  else
  {
    ctx.stageToWorld = scale;
  }
  return ctx;
}

// Compose a USD prim's localToWorld with the stage's correction. The
// returned Pyxis matrix is ready to drop straight into
// InstanceDesc::worldFromLocal / CameraDesc::viewFromWorld inverse /
// LightDesc::position.
hlslpp::float4x4 ComposeWorldFromLocal(const pxr::UsdPrim& prim,
                                       pxr::UsdGeomXformCache& xformCache,
                                       const StageContext& stageCtx) noexcept
{
  const pxr::GfMatrix4d usdLocalToWorld = xformCache.GetLocalToWorldTransform(prim);
  return mul(stageCtx.stageToWorld, ToPyxisMatrix(usdLocalToWorld));
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

// M7: translate a UsdLuxDistantLight / UsdLuxDomeLight / UsdLuxRectLight
// prim into a LightDesc + push via GpuScene::AddLight. The simple
// closesthit consumes color × intensity per kind (NdotL Lambert for
// Distant + Rect, uniform ambient for Dome). The user's M7-full pass
// replaces the closesthit body alongside NEE + MIS + IBL importance
// sampling per §7.
//
// USD light conventions:
//   * UsdLuxDistantLight emits along the prim's local -Z axis. Its
//     "direction the light is travelling" (away from surface) =
//     worldFromLocal · (0, 0, -1, 0). The prim's translation is
//     irrelevant.
//   * UsdLuxRectLight is positioned at the prim's translation. The
//     emitting face is +Z in local space; the M7-simple closesthit
//     ignores axisU/axisV (treats as point-at-center) but we pack
//     them for the M7-full pass.
//   * UsdLuxDomeLight has color + intensity + texture:file; M7-simple
//     ignores the env-map and treats it as uniform ambient.
//
// Per UsdLuxLight authoring: `inputs:intensity` × `inputs:exposure`
// is the canonical scale. exposure is in stops; final intensity =
// raw_intensity * 2^exposure. M7-simple skips the exposure
// multiplier (defaults to 0 → 2^0 = 1, harmless for the common case);
// M9 polish picks it up alongside the rest of the UsdLux input
// surface.
void EmitLight(const pxr::UsdPrim& prim, pxr::UsdGeomXformCache& xformCache,
               const StageContext& stageCtx, GpuScene& scene) noexcept {
  auto& log = Logging::Get();
  // Compose USD's local-to-world with the stage-to-world correction
  // (metersPerUnit + Z->Y if needed) so light positions land in
  // Pyxis-world (metres + Y-up). Direction extraction uses Pyxis
  // matrix math throughout — `mul(worldFromLocal, localDir)` with
  // localDir.w=0 drops the translation column automatically.
  const hlslpp::float4x4 worldFromLocal = ComposeWorldFromLocal(prim, xformCache, stageCtx);

  // Helpers for USD light input attrs — they live on the prim under
  // `inputs:<name>` namespace. UsdLuxLightAPI exposes typed accessors
  // but pulling the attr by name is simpler + avoids the API churn
  // between USD versions.
  auto readFloat = [&](const char* name, float fallback) {
    const pxr::UsdAttribute attr = prim.GetAttribute(pxr::TfToken(std::string("inputs:") + name));
    if (!attr)
      return fallback;
    float value = fallback;
    attr.Get(&value);
    return value;
  };
  auto readColor = [&](const char* name, hlslpp::float3 fallback) {
    const pxr::UsdAttribute attr = prim.GetAttribute(pxr::TfToken(std::string("inputs:") + name));
    if (!attr)
      return fallback;
    pxr::GfVec3f value(fallback.x, fallback.y, fallback.z);
    attr.Get(&value);
    return hlslpp::float3{value[0], value[1], value[2]};
  };

  LightDesc desc;
  desc.color = readColor("color", hlslpp::float3{1.0f, 1.0f, 1.0f});
  // UsdLuxLightAPI canonical scale: intensity * 2^exposure. Omniverse
  // + DCC content typically authors a high `intensity` (e.g. 12000 for
  // a sky dome) and zero exposure; some pipelines push the scale into
  // exposure stops instead. Both together cover the common cases.
  // Default exposure = 0 → 2^0 = 1 (no-op for fixtures that don't
  // author it).
  const float rawIntensity = readFloat("intensity", 1.0f);
  const float exposureStops = readFloat("exposure", 0.0f);
  desc.intensity = rawIntensity * std::exp2(exposureStops);

  if (prim.IsA<pxr::UsdLuxDistantLight>())
  {
    desc.kind = LightDesc::Kind::Distant;
    // Local -Z transformed by worldFromLocal's rotation block. Pyxis
    // column-vector convention: dir_world = M * dir_local. w=0 keeps
    // the translation column from contributing — pure rotation +
    // optional uniform scale (Pack normalises before upload).
    const hlslpp::float4 dirWorld =
        mul(worldFromLocal, hlslpp::float4{0.0f, 0.0f, -1.0f, 0.0f});
    desc.direction = hlslpp::float3{dirWorld.x, dirWorld.y, dirWorld.z};
  }
  else if (prim.IsA<pxr::UsdLuxRectLight>())
  {
    desc.kind = LightDesc::Kind::Rect;
    const hlslpp::float4 originWorld =
        mul(worldFromLocal, hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f});
    desc.position = hlslpp::float3{originWorld.x, originWorld.y, originWorld.z};
    const hlslpp::float4 uWorld =
        mul(worldFromLocal, hlslpp::float4{1.0f, 0.0f, 0.0f, 0.0f});
    desc.axisU = hlslpp::float3{uWorld.x, uWorld.y, uWorld.z};
    const hlslpp::float4 vWorld =
        mul(worldFromLocal, hlslpp::float4{0.0f, 1.0f, 0.0f, 0.0f});
    desc.axisV = hlslpp::float3{vWorld.x, vWorld.y, vWorld.z};
  }
  else if (prim.IsA<pxr::UsdLuxDomeLight>())
  {
    desc.kind = LightDesc::Kind::Dome;
    // Resolve the dome's `inputs:texture:file` SdfAssetPath through
    // USD's ArResolver (handles relative `@./default_sky.exr@`-style
    // refs against the .usd's parent directory) and AcquireTexture
    // it. The resulting TextureHandle is stored on LightDesc.envMap;
    // GpuScene::CommitResources resolves it to a bindless slot at
    // pack time, and PathTracePass reads the first live dome's
    // texture for the miss shader's lat-long sample.
    const pxr::UsdAttribute textureAttr =
        prim.GetAttribute(pxr::TfToken("inputs:texture:file"));
    if (textureAttr)
    {
      pxr::SdfAssetPath assetPath;
      if (textureAttr.Get(&assetPath))
      {
        // ResolvedPath() runs USD's ArResolver; falls back to the
        // raw asset string if the resolver couldn't find the file
        // (rare for relative siblings; the M7 path-trace then
        // logs an EXR-decode failure at CommitResources time).
        std::string resolvedPath = assetPath.GetResolvedPath();
        if (resolvedPath.empty())
          resolvedPath = assetPath.GetAssetPath();
        if (!resolvedPath.empty())
        {
          TextureKey key;
          key.resolvedPath = resolvedPath;
          key.role = TextureKey::Role::Emission;  // HDR env-map: linear, no sRGB EOTF
          desc.envMap = scene.AcquireTexture(key);
        }
      }
    }
  }
  else
  {
    return;  // unknown light kind
  }

  const LightHandle handle = scene.AddLight(desc);
  if (handle == LightHandle::Invalid)
  {
    log.Warn(log::APP, "StageWalker: AddLight failed for "
                           + prim.GetPath().GetString()
                           + " (light handle space exhausted?).");
  }
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
                        const StageContext& stageCtx,
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
    // Mark the prototype root AND all descendants as consumed —
    // pass 3's standalone-mesh walk will skip them so we don't
    // double-emit a non-instanced copy. Walking descendants matters
    // for the (M6-unsupported but defensively-handled) Xform-wrapped
    // prototype case: without descendant marking, the inner meshes
    // would leak through pass 3 and render as one extra standalone
    // copy at the Xform's world position.
    for (const pxr::UsdPrim& descendant : pxr::UsdPrimRange(protoPrim))
      consumedPrototypes.emplace(descendant.GetPath().GetString());

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
    // Bake stage-to-world (metersPerUnit + Z->Y) onto every instance
    // so the BLAS keeps its stage-unit local geometry but the per-
    // instance transform places it correctly in Pyxis-world.
    instanceDesc.worldFromLocal =
        mul(stageCtx.stageToWorld, ToPyxisMatrix(worldFromLocal));
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
// was emitted (false skips the matching instance increment).
//
// Triangulation: GpuScene::CreateMesh expects a triangle list, but USD
// authors arbitrary faceVertexCounts (most production / Omniverse
// scenes are quad-dominant). Each face with N vertices fan-
// triangulates to N-2 triangles `(v0, v_{i+1}, v_{i+2})`. Convex-
// polygon assumption — concave faces produce visually-wrong (but
// non-crashing) triangles. Subdivision is §42-deferred so authoring
// concave ngons that need ear-clipping is post-v1.
bool EmitMesh(const pxr::UsdPrim& prim, pxr::UsdGeomXformCache& xformCache,
              const MaterialHandleByPath& materialsByPath,
              const StageContext& stageCtx, GpuScene& scene) noexcept {
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

  // Convert positions into the §18.4 contiguous layout.
  std::vector<hlslpp::float3> positions;
  positions.reserve(usdPoints.size());
  for (const pxr::GfVec3f& point : usdPoints)
    positions.emplace_back(point[0], point[1], point[2]);

  // Fan-triangulate. Reserve assumes ~triangles+quads (3*indices is
  // an upper bound for all-triangle input; quad-dominant scenes
  // produce ~2x triangles per face, ngons more). One pass over the
  // counts/indices stream emits 3 indices per output triangle.
  std::vector<uint32_t> indices;
  indices.reserve(usdIndices.size() * 3u / 2u);
  std::size_t faceOffset = 0;
  for (const int faceCount : usdCounts)
  {
    if (faceCount < 3)
    {
      // Degenerate face (point or edge). Skip and advance.
      faceOffset += static_cast<std::size_t>(faceCount);
      continue;
    }
    if (faceOffset + static_cast<std::size_t>(faceCount) > usdIndices.size())
    {
      // Malformed — counts disagree with indices length. Drop the
      // entire mesh; partial fan would mismatch positions.
      log.Warn(log::APP, "StageWalker: " + prim.GetPath().GetString()
                             + " faceVertexCounts/Indices length mismatch; "
                             + "dropping mesh.");
      return false;
    }
    for (int triIdx = 0; triIdx < faceCount - 2; ++triIdx)
    {
      indices.push_back(static_cast<uint32_t>(usdIndices[faceOffset + 0]));
      indices.push_back(static_cast<uint32_t>(usdIndices[faceOffset + triIdx + 1]));
      indices.push_back(static_cast<uint32_t>(usdIndices[faceOffset + triIdx + 2]));
    }
    faceOffset += static_cast<std::size_t>(faceCount);
  }

  if (indices.empty())
  {
    log.Warn(log::APP, "StageWalker: " + prim.GetPath().GetString()
                           + " has no valid faces after triangulation; skipping.");
    return false;
  }

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
  // ComposeWorldFromLocal bakes metersPerUnit + Z->Y if the stage
  // metadata says so, so the BLAS keeps its stage-unit local-space
  // geometry but the per-instance transform places it correctly in
  // Pyxis-world (metres + Y-up).
  InstanceDesc instanceDesc;
  instanceDesc.mesh = *meshHandle;
  instanceDesc.worldFromLocal = ComposeWorldFromLocal(prim, xformCache, stageCtx);
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
// Build a Pyxis CameraDesc from a UsdGeomCamera prim. Returns nullopt
// for invalid prims. Does NOT push to GpuScene — caller decides
// whether this is the active camera (e.g. matches the stage's
// `customLayerData.cameraSettings.boundCamera` hint).
//
// Composes the stage-to-world correction onto the camera's world
// transform so viewFromWorld lands in Pyxis-world coords (metres +
// Y-up if the stage was Z-up). Projection matrix is intrinsic — no
// scale correction needed.
std::optional<CameraDesc> BuildCameraDesc(const pxr::UsdPrim& prim,
                                          pxr::UsdGeomXformCache& xformCache,
                                          const StageContext& stageCtx) noexcept
{
  const pxr::UsdGeomCamera cameraPrim(prim);
  if (!cameraPrim.GetPrim().IsValid())
    return std::nullopt;

  const pxr::GfCamera gfCamera = cameraPrim.GetCamera(pxr::UsdTimeCode::Default());
  const hlslpp::float4x4 worldFromCamera = ComposeWorldFromLocal(prim, xformCache, stageCtx);

  CameraDesc desc;
  desc.viewFromWorld = hlslpp::inverse(worldFromCamera);
  desc.projFromView  = ToPyxisMatrix(gfCamera.GetFrustum().ComputeProjectionMatrix());
  desc.focalLengthMm = gfCamera.GetFocalLength();
  desc.apertureFStop = gfCamera.GetFStop();
  desc.focusDistance = gfCamera.GetFocusDistance();
  const pxr::GfRange1f clipRange = gfCamera.GetClippingRange();
  desc.nearClip = clipRange.GetMin();
  desc.farClip  = clipRange.GetMax();
  // UsdGeomCamera::exposure (in stops). Defaults to 0 = no adjustment.
  // Omniverse + DCC content commonly authors a strongly negative
  // value (e.g. -10) to compensate for radiometric light intensities
  // that are calibrated for a physically-based exposure response;
  // without consuming this attribute the scene blows out to white.
  if (const pxr::UsdAttribute exposureAttr = cameraPrim.GetExposureAttr())
  {
    float exposure = 0.0f;
    if (exposureAttr.Get(&exposure))
      desc.exposure = exposure;
  }
  return desc;
}

}  // namespace

IngestStats StageWalker::WalkFile(std::string_view usdPath, GpuScene& scene) {
  auto& log = Logging::Get();
  const std::string pathString{usdPath};

  using Clock = std::chrono::steady_clock;
  const auto walkStart = Clock::now();
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(pathString);
  const auto stageOpenEnd = Clock::now();
  if (!stage)
  {
    log.Error(log::APP, std::string{"StageWalker: failed to open "} + pathString);
    IngestStats failed{};
    failed.stageOpenMs =
        std::chrono::duration<float, std::milli>(stageOpenEnd - walkStart).count();
    failed.totalMs = failed.stageOpenMs;
    return failed;
  }
  IngestStats stats = WalkStage(stage, scene);
  const auto walkEnd = Clock::now();
  // stageOpenMs is local to WalkFile (WalkStage didn't open the
  // stage); fold it in + recompute totalMs to include it.
  stats.stageOpenMs =
      std::chrono::duration<float, std::milli>(stageOpenEnd - walkStart).count();
  stats.totalMs = std::chrono::duration<float, std::milli>(walkEnd - walkStart).count();
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

  using Clock = std::chrono::steady_clock;
  const auto walkStart = Clock::now();

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
  const auto traverseEnd = Clock::now();
  stats.traverseSortMs =
      std::chrono::duration<float, std::milli>(traverseEnd - walkStart).count();

  pxr::UsdGeomXformCache xformCache;
  const StageContext stageCtx = BuildStageContext(stage);

  // Read the optional `boundCamera` hint from the root layer's
  // customLayerData (Omniverse + DCC convention). Empty string =
  // no hint; we'll fall back to first-camera-in-SdfPath-order.
  std::string boundCameraHintPath;
  if (stage->GetRootLayer())
  {
    const pxr::VtDictionary customData = stage->GetRootLayer()->GetCustomLayerData();
    auto camSettingsIt = customData.find("cameraSettings");
    if (camSettingsIt != customData.end()
        && camSettingsIt->second.IsHolding<pxr::VtDictionary>())
    {
      const pxr::VtDictionary& camSettings =
          camSettingsIt->second.Get<pxr::VtDictionary>();
      auto boundIt = camSettings.find("boundCamera");
      if (boundIt != camSettings.end() && boundIt->second.IsHolding<std::string>())
      {
        boundCameraHintPath = boundIt->second.Get<std::string>();
      }
    }
  }

  // Pass 1 — materials. Translate every UsdShadeMaterial to an
  // OpenPBRMaterialDesc and AcquireMaterial it; record the resulting
  // handle by SdfPath so the mesh pass can resolve bindings without
  // re-walking the stage. Both adapters share this code path
  // (HydraEngine routes through StageWalker at M5 — see HydraEngine.h)
  // so any difference in material translation breaks the §25.O.3
  // byte-equal invariant.
  MaterialHandleByPath materialsByPath;
  const auto materialPassStart = Clock::now();
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
  const auto materialPassEnd = Clock::now();
  stats.materialPassMs =
      std::chrono::duration<float, std::milli>(materialPassEnd - materialPassStart).count();

  // Pass 2 — UsdGeomPointInstancer (M6). Walked BEFORE the standalone
  // mesh pass so any prototype meshes referenced by an instancer get
  // registered + recorded in `consumedPrototypes`; the mesh pass then
  // skips them so we don't emit a non-instanced duplicate of each
  // prototype. SdfPath-sorted iteration order is preserved per
  // §25.O.3 so both adapters expand instancers identically.
  ConsumedPrototypePaths consumedPrototypes;
  const auto instancerPassStart = Clock::now();
  for (const pxr::UsdPrim& prim : prims)
  {
    if (!prim.IsA<pxr::UsdGeomPointInstancer>())
      continue;
    EmitPointInstancer(prim, xformCache, materialsByPath, stageCtx, scene, stats,
                       consumedPrototypes);
  }
  const auto instancerPassEnd = Clock::now();
  stats.instancerPassMs =
      std::chrono::duration<float, std::milli>(instancerPassEnd - instancerPassStart).count();

  // Pass 3 — meshes / camera / lights, with material handles resolved
  // from pass 1 and instancer prototypes skipped (pass 2 already
  // expanded them).
  const auto meshPassStart = Clock::now();
  for (const pxr::UsdPrim& prim : prims)
  {
    if (prim.IsA<pxr::UsdGeomMesh>())
    {
      if (consumedPrototypes.contains(prim.GetPath().GetString()))
        continue;
      if (EmitMesh(prim, xformCache, materialsByPath, stageCtx, scene))
      {
        ++stats.meshesEmitted;
        ++stats.instancesEmitted;
      }
    }
    else if (prim.IsA<pxr::UsdGeomCamera>())
    {
      // Build the desc + collect into the stats list — the editor
      // will populate a Scene-Camera combo from these. Active-camera
      // selection happens AFTER the loop so we can honour the
      // root-layer's `boundCamera` hint regardless of SdfPath order.
      if (auto descOpt = BuildCameraDesc(prim, xformCache, stageCtx); descOpt)
      {
        NamedCamera entry;
        entry.name = prim.GetPath().GetString();
        entry.desc = *descOpt;
        stats.cameras.push_back(std::move(entry));
        ++stats.camerasEmitted;
      }
    }
    else if (prim.IsA<pxr::UsdLuxDistantLight>() || prim.IsA<pxr::UsdLuxDomeLight>()
             || prim.IsA<pxr::UsdLuxRectLight>())
    {
      // M7-simple: translate + push into GpuScene. The closesthit's
      // simple shading model (NdotL Lambert for Distant + Rect,
      // uniform ambient for Dome) consumes them; user's M7-full
      // pass adds NEE + MIS + IBL importance sampling per §7.
      EmitLight(prim, xformCache, stageCtx, scene);
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
  // Active-camera selection. Honour the root-layer's `boundCamera`
  // hint if present (Omniverse + DCC convention: the camera the
  // scene's authoring tool was last looking through). Fall back to
  // first-in-SdfPath-order — preserves the M4 "first camera wins"
  // contract for fixtures that don't author a hint. activeCameraIndex
  // stays -1 only when the scene authored zero cameras.
  if (!stats.cameras.empty())
  {
    int activeIdx = 0;  // first-in-SdfPath-order fallback
    if (!boundCameraHintPath.empty())
    {
      for (std::size_t i = 0; i < stats.cameras.size(); ++i)
      {
        if (stats.cameras[i].name == boundCameraHintPath)
        {
          activeIdx = static_cast<int>(i);
          break;
        }
      }
    }
    stats.activeCameraIndex = activeIdx;
    scene.SetCamera(stats.cameras[static_cast<std::size_t>(activeIdx)].desc);
  }

  const auto meshPassEnd = Clock::now();
  stats.meshLightCameraMs =
      std::chrono::duration<float, std::milli>(meshPassEnd - meshPassStart).count();
  // totalMs covers WalkStage end-to-end (sum of stages + harness
  // overhead). WalkFile recomputes totalMs to also include the
  // pxr::UsdStage::Open above WalkStage.
  stats.totalMs =
      std::chrono::duration<float, std::milli>(meshPassEnd - walkStart).count();

  return stats;
}

}  // namespace pyxis::usd_ingest
