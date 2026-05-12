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

#include <mikktspace.h>

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <Pyxis/Platform/FileSystem/AssetLocator.h>

#include <pxr/usd/usdLux/blackbody.h>
#include <pxr/usd/usdLux/cylinderLight.h>
#include <pxr/usd/usdLux/diskLight.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/geometryLight.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/portalLight.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/shapingAPI.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/base/gf/matrix4d.h>

#include <cmath>
#include <numbers>
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
#include <execution>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pyxis::usd_ingest {

// ---- IngestResult PIMPL ---------------------------------------------------
// STL containers + std::strings live behind this opaque body so the
// public IngestResult class stays §18.9-compliant (no STL across the
// SHARED-DLL boundary).
struct IngestResult::Impl {
  IngestStats stats{};
  // Internal-only camera record — the std::string lives in the DLL's
  // allocator space and never crosses the boundary; GetCameraAt
  // memcpy-truncates it into the public NamedCameraView::name char
  // buffer.
  struct Entry {
    std::string name;       // SdfPath of the camera prim
    CameraDesc  desc;       // intrinsics + viewFromWorld
  };
  std::vector<Entry> cameras;
};

IngestResult::IngestResult() : _impl(std::make_unique<Impl>()) {}
IngestResult::~IngestResult() = default;
IngestResult::IngestResult(IngestResult&&) noexcept            = default;
IngestResult& IngestResult::operator=(IngestResult&&) noexcept = default;

const IngestStats& IngestResult::Stats() const noexcept { return _impl->stats; }

uint32_t IngestResult::GetCameraCount() const noexcept {
  return static_cast<uint32_t>(_impl->cameras.size());
}

bool IngestResult::GetCameraAt(uint32_t index, NamedCameraView* out) const noexcept {
  if (out == nullptr || index >= _impl->cameras.size())
    return false;
  const auto& entry = _impl->cameras[index];
  out->desc = entry.desc;
  // Truncate to NamedCameraView::name's 256-char inline buffer; null-
  // terminate. The truncation only matters for diagnostics-display
  // since the active-camera selection lookup uses the truncated form
  // too (matches against the same truncated `boundCamera` hint).
  const std::size_t copyLen = std::min<std::size_t>(entry.name.size(), sizeof(out->name) - 1u);
  std::memcpy(out->name, entry.name.data(), copyLen);
  out->name[copyLen] = '\0';
  return true;
}

IngestResult::Impl& IngestResult::GetImpl() noexcept { return *_impl; }

namespace {

// M9-fidelity hard-edge dedup. UsdGeomMesh authors normals + UVs
// with one of three interpolation modes:
//   - vertex:      one value per usdPoints entry (already shared)
//   - faceVarying: one value per face-vertex (independent per face)
//   - constant/uniform: one value for the whole mesh / face
// Only faceVarying introduces hard edges — when two faces sharing a
// vertex want different normals (hard mesh edge) or different UVs
// (UV seam at an island boundary). The pre-M9-fidelity pipeline
// collapsed faceVarying to per-vertex by taking the first value
// per shared vertex slot, dropping the second. With dedup we emit
// a NEW vertex slot whenever (positionIdx, normal, uv) differs —
// duplicating shared positions only as needed. UV seams + hard
// crease edges then render correctly.
//
// **Chained per-position dedup** (not std::unordered_map). The
// initial implementation used unordered_map<VertexKey, uint32_t>
// keyed on (positionIdx + bit-cast normal + bit-cast uv); it was
// correct but catastrophically slow in MSVC Debug builds —
// std::unordered_map node allocations + iterator-debugging
// overhead dragged lobby ingest from ~15s to ~160s.
//
// Replacement: per parent-mesh-position, a singly-linked chain
// of (normal, uv, outIdx) entries pooled in a flat vector.
// Lookup walks the chain comparing bit-patterns; in production
// scenes most positions have one entry (chain length 1, one
// memcmp per face-vertex), UV-seam corners + hard-edge vertices
// have 2–4 entries. Zero per-FV allocations once the entry pool
// is reserved. Same dedup semantics as the unordered_map — bit-
// equal (n, uv) tuples collapse to the same slot, distinct
// tuples split — but ~50× faster in Debug.
struct VertexEntry {
  std::uint32_t normalBits[3];   // bit-cast hlslpp::float3
  std::uint32_t uvBits[2];       // bit-cast hlslpp::float2
  std::uint32_t outIdx;          // slot in outPositions
  std::int32_t  next;            // chain link, -1 = end
};

// MikkTSpace user-data + callbacks for per-mesh tangent generation.
// MikkTSpace consumes (positions, normals, UVs) per-face-vertex and
// produces per-(face, vertex) tangents — strictly the documented
// "do NOT collapse via the existing index list" pattern. We feed it
// the M9-fidelity hard-edge-deduplicated arrays (one tangent per
// emitted vertex slot, where face-varying boundaries already split
// into distinct slots), so each tangent write naturally lands on a
// unique slot and the first-tangent-wins guard becomes a no-op.
//
// Pointers (not std::vector*) are intentional: MikkTSpace fires its
// callbacks ~12 times per face, and MSVC Debug's std::vector<T>::
// operator[] does iterator-debugging bounds checks each access. On
// the lobby that overhead alone added tens of seconds to ingest;
// caching raw const pointers + sizes once per mesh skirts the
// per-callback std::vector machinery entirely.
struct MikkContext {
  const std::uint32_t*    triangleIndices   = nullptr;
  std::size_t             triangleVertCount = 0;  // = triangleIndices length
  const hlslpp::float3*   positions         = nullptr;
  std::size_t             positionsCount    = 0;
  const hlslpp::float3*   vertexNormals     = nullptr;
  const hlslpp::float2*   vertexUvs         = nullptr;
  hlslpp::float4*         outVertexTangents = nullptr;  // x,y,z,sign
  bool*                   outTangentAssigned = nullptr;
  std::size_t             outVertexCount    = 0;
};

int MikkGetNumFaces(const SMikkTSpaceContext* ctx) {
  const auto* user = static_cast<const MikkContext*>(ctx->m_pUserData);
  return static_cast<int>(user->triangleVertCount / 3u);
}

int MikkGetNumVerticesOfFace(const SMikkTSpaceContext* /*ctx*/, int /*face*/) {
  return 3;  // Always triangulated upstream.
}

void MikkGetPosition(const SMikkTSpaceContext* ctx, float* outPos,
                     int face, int vert) {
  const auto* user = static_cast<const MikkContext*>(ctx->m_pUserData);
  const std::uint32_t vertexIdx =
      user->triangleIndices[static_cast<std::size_t>(face) * 3u
                            + static_cast<std::size_t>(vert)];
  const hlslpp::float3& pos = user->positions[vertexIdx];
  outPos[0] = static_cast<float>(pos.x);
  outPos[1] = static_cast<float>(pos.y);
  outPos[2] = static_cast<float>(pos.z);
}

void MikkGetNormal(const SMikkTSpaceContext* ctx, float* outNormal,
                   int face, int vert) {
  const auto* user = static_cast<const MikkContext*>(ctx->m_pUserData);
  const std::uint32_t vertexIdx =
      user->triangleIndices[static_cast<std::size_t>(face) * 3u
                            + static_cast<std::size_t>(vert)];
  const hlslpp::float3& nrm = user->vertexNormals[vertexIdx];
  outNormal[0] = static_cast<float>(nrm.x);
  outNormal[1] = static_cast<float>(nrm.y);
  outNormal[2] = static_cast<float>(nrm.z);
}

void MikkGetTexCoord(const SMikkTSpaceContext* ctx, float* outUv,
                     int face, int vert) {
  const auto* user = static_cast<const MikkContext*>(ctx->m_pUserData);
  const std::uint32_t vertexIdx =
      user->triangleIndices[static_cast<std::size_t>(face) * 3u
                            + static_cast<std::size_t>(vert)];
  const hlslpp::float2& uvCoord = user->vertexUvs[vertexIdx];
  outUv[0] = static_cast<float>(uvCoord.x);
  outUv[1] = static_cast<float>(uvCoord.y);
}

void MikkSetTSpaceBasic(const SMikkTSpaceContext* ctx, const float* tangent,
                        float sign, int face, int vert) {
  auto* user = static_cast<MikkContext*>(ctx->m_pUserData);
  const std::uint32_t vertexIdx =
      user->triangleIndices[static_cast<std::size_t>(face) * 3u
                            + static_cast<std::size_t>(vert)];
  if (vertexIdx >= user->outVertexCount)
    return;
  if (!user->outTangentAssigned[vertexIdx])
  {
    user->outVertexTangents[vertexIdx] =
        hlslpp::float4{tangent[0], tangent[1], tangent[2], sign};
    user->outTangentAssigned[vertexIdx] = true;
  }
}

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
// MikkTSpace tangent generation is the most expensive step in mesh
// ingest (the lobby's 955 sub-meshes × ~140ms each = ~130s in Debug
// before this gate). Tangents are only sampled by the closesthit's
// normal-mapping branch, so we can skip MikkTSpace entirely on
// meshes whose bound materials don't carry a normal map. Pass 1
// fills this set with material handles whose OpenPBRMaterialDesc
// resolved a non-Invalid normalMap; the per-subset emit checks
// before invoking genTangSpaceDefault.
using MaterialsNeedingTangents = std::unordered_set<MaterialHandle>;

// Result of mesh-data extraction — the heavy CPU work (USD attr
// reads, hard-edge dedup, MikkTSpace) lifted out of the GpuScene
// mutation calls so it can run on worker threads in parallel. The
// main / render thread later walks PreparedMesh.subMeshes serially
// and feeds them to scene.CreateMesh / scene.AppendInstance — those
// stay single-writer per §30.11.
struct PreparedSubMesh {
  std::vector<hlslpp::float3>  positions;
  std::vector<std::uint32_t>   indices;
  std::vector<hlslpp::float3>  normals;
  std::vector<hlslpp::float4>  tangents;
  std::vector<hlslpp::float2>  uv0;
  MaterialHandle               material  = MaterialHandle::Invalid;
  std::string                  debugName;
};

struct PreparedMesh {
  std::vector<PreparedSubMesh> subMeshes;       // size 0 when prep failed / mesh dropped
  hlslpp::float4x4             worldFromLocal;  // populated when subMeshes non-empty
};

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

// M7 / M8a: translate a UsdLux* prim into a LightDesc + push via
// GpuScene::AddLight. The simple closesthit consumes color × intensity
// per kind (NdotL Lambert for Distant + Rect, uniform ambient for
// Dome). The user's M7-full pass replaces the closesthit body
// alongside NEE + MIS + IBL importance sampling per §7.
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
// M8a UsdLux coverage expansion — every commonly-authored UsdLux
// input is now READ at ingest and forwarded into LightDesc, even
// when the M7-simple closesthit ignores the field. The data flows
// through GpuScene → LightEntry so the M9 polish pass + the user's
// M7-full closesthit can pick them up without re-walking the stage.
//
// Inputs we COLLAPSE at load (so the closesthit reads the
// already-modulated value):
//   * `inputs:exposure`  → multiplies intensity by 2^stops
//   * `inputs:colorTemperature` (when `enableColorTemperature` true)
//                        → multiplies color by Planckian blackbody
//                          (USD's UsdLuxBlackbodyTemperatureAsRgb)
//   * `inputs:normalize` → divides intensity by the light's surface
//                          area (Rect = w·h, Disk = π·r², Sphere =
//                          4π·r², Cylinder = 2π·r·length, Dome = 4π
//                          steradians treated as area=1, Distant
//                          ignored — direct lights don't normalise)
//
// Inputs we STASH on LightDesc for the future shading pass:
//   * `inputs:diffuse` / `inputs:specular` — per-contribution scalars
//   * `inputs:angle` (DistantLight) — sun angular diameter
//   * UsdLuxShapingAPI (`shaping:cone:angle / softness / focus /
//     focusTint`) — spotlight cone falloff
//   * UsdLuxDomeLight `inputs:texture:format` — latlong / mirroredBall /
//     angular — closesthit picks the matching UV mapping.
//
// Light kinds we now LOAD but defer rendering of:
//   * UsdLuxCylinderLight  → Kind::Cylinder
//   * UsdLuxGeometryLight  → Kind::Geometry
//   * UsdLuxPortalLight    → Kind::Portal
//
// IES profiles (`shaping:ies:file`) and the Geometry / Portal
// per-prim refs are LOGGED + ignored — the LightDesc surface doesn't
// have a slot for them yet (they'd want a separate handle pool, M9
// follow-up).
//
// Visibility (`UsdGeomImageable::ComputeVisibility()`) gates the
// whole pipeline at the top — `invisible` lights skip the AddLight
// entirely so a world authoring a "vis = invisible" backup light
// doesn't double-shade with the active one.
void EmitLight(const pxr::UsdPrim& prim, pxr::UsdGeomXformCache& xformCache,
               const StageContext& stageCtx, GpuScene& scene) noexcept {
  auto& log = Logging::Get();

  // ---- Visibility gate ---------------------------------------------------
  // UsdGeomImageable::ComputeVisibility walks up the hierarchy honouring
  // inherited `visibility`. Skipping invisible lights here means a USD
  // scene with a default-on light authored alongside a `visibility =
  // invisible` backup doesn't double-up — matches usdview / Storm
  // semantics.
  const pxr::UsdGeomImageable imageable(prim);
  if (imageable
      && imageable.ComputeVisibility(pxr::UsdTimeCode::Default())
             == pxr::UsdGeomTokens->invisible)
  {
    return;
  }

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
  auto readBool = [&](const char* name, bool fallback) {
    const pxr::UsdAttribute attr = prim.GetAttribute(pxr::TfToken(std::string("inputs:") + name));
    if (!attr)
      return fallback;
    bool value = fallback;
    attr.Get(&value);
    return value;
  };
  auto readToken = [&](const char* name, const pxr::TfToken& fallback) {
    const pxr::UsdAttribute attr = prim.GetAttribute(pxr::TfToken(std::string("inputs:") + name));
    if (!attr)
      return fallback;
    pxr::TfToken value = fallback;
    attr.Get(&value);
    return value;
  };

  LightDesc desc;
  desc.color = readColor("color", hlslpp::float3{1.0f, 1.0f, 1.0f});
  // ---- Color temperature collapse ---------------------------------------
  // USD: when enableColorTemperature is true, the light's tint is
  // `color * blackbody(colorTemperature_K)`. Default Kelvin = 6500 (D65).
  // We bake the multiplier into desc.color so the runtime stays
  // colorTemperature-unaware — matches usdview / Hydra behaviour.
  const bool  enableColorTemp  = readBool("enableColorTemperature", false);
  const float colorTemperature = readFloat("colorTemperature", 6500.0f);
  if (enableColorTemp)
  {
    const pxr::GfVec3f blackbodyRgb = pxr::UsdLuxBlackbodyTemperatureAsRgb(colorTemperature);
    desc.color = hlslpp::float3{
        desc.color.x * blackbodyRgb[0],
        desc.color.y * blackbodyRgb[1],
        desc.color.z * blackbodyRgb[2]};
  }

  // UsdLuxLightAPI canonical scale: intensity * 2^exposure. Omniverse
  // + DCC content typically authors a high `intensity` (e.g. 12000 for
  // a sky dome) and zero exposure; some pipelines push the scale into
  // exposure stops instead. Both together cover the common cases.
  // Default exposure = 0 → 2^0 = 1 (no-op for fixtures that don't
  // author it).
  const float rawIntensity  = readFloat("intensity", 1.0f);
  const float exposureStops = readFloat("exposure",  0.0f);
  desc.intensity = rawIntensity * std::exp2(exposureStops);

  // ---- M8a UsdLux modifiers stashed on LightDesc -----------------------
  // diffuse / specular are per-lobe multipliers the M9 BSDF split
  // applies; default 1.0 = no-op.
  desc.diffuse   = readFloat("diffuse",  1.0f);
  desc.specular  = readFloat("specular", 1.0f);
  desc.normalize = readBool("normalize", false);

  // UsdLuxShapingAPI — read whether or not the prim opts in (the API
  // is metadata-only on the prim's customData; reading the attrs
  // directly + falling back to defaults is ABI-stable across USD
  // versions). Default conn:angle=90 means "no spotlight cone";
  // closesthit's M9 spot pass picks this up.
  desc.shapingConeAngle    = readFloat("shaping:cone:angle",    90.0f);
  desc.shapingConeSoftness = readFloat("shaping:cone:softness", 0.0f);
  desc.shapingFocus        = readFloat("shaping:focus",         0.0f);
  desc.shapingFocusTint    = readColor("shaping:focusTint",
                                       hlslpp::float3{1.0f, 1.0f, 1.0f});

  // IES profile — common for archviz scenes (real luminaire
  // distributions). LightDesc has no slot for it yet (would need a
  // dedicated photometric-profile pool, M9 follow-up). Log so the
  // user knows the data was seen + ignored, not silently dropped.
  const pxr::UsdAttribute iesFileAttr =
      prim.GetAttribute(pxr::TfToken("inputs:shaping:ies:file"));
  if (iesFileAttr)
  {
    pxr::SdfAssetPath iesPath;
    if (iesFileAttr.Get(&iesPath) && !iesPath.GetAssetPath().empty())
    {
      log.Info(log::APP, "StageWalker: " + prim.GetPath().GetString()
                             + " authors IES profile " + iesPath.GetAssetPath()
                             + " — read but not yet rendered (M9 follow-up).");
    }
  }

  // ---- Per-kind dispatch ------------------------------------------------
  // areaForNormalize is the WORLD-space surface area of the light,
  // used by the `normalize` divisor at the bottom of this function.
  // Area lights compute it AFTER the local axisU/V are transformed
  // through worldFromLocal — so an xform scale on the light prim (or
  // on a parent Xform group) rolls into the divisor naturally. A
  // 2×-scaled RectLight integrates over 4× area; a uniformly scaled
  // SphereLight integrates over (scale²)× area; etc. Without this,
  // a normalised scaled light would render {scale²}× too bright.
  // Helper for world-space axis length — std::hypot for the
  // 3-component case avoids overflow on very large scales.
  auto worldLength3 = [](const hlslpp::float3& vec) noexcept {
    return std::sqrt(static_cast<float>(vec.x) * static_cast<float>(vec.x)
                     + static_cast<float>(vec.y) * static_cast<float>(vec.y)
                     + static_cast<float>(vec.z) * static_cast<float>(vec.z));
  };
  constexpr float PI_F = std::numbers::pi_v<float>;
  float areaForNormalize = 1.0f;

  if (prim.IsA<pxr::UsdLuxDistantLight>())
  {
    desc.kind = LightDesc::Kind::Distant;
    // Local -Z transformed by worldFromLocal's rotation block. Pyxis
    // column-vector convention: dir_world = M * dir_local. w=0 keeps
    // the translation column from contributing — pure rotation +
    // optional uniform scale (Pack normalises before upload).
    const hlslpp::float4 dirWorld =
        mul(worldFromLocal, hlslpp::float4{0.0f, 0.0f, -1.0f, 0.0f});
    desc.direction    = hlslpp::float3{dirWorld.x, dirWorld.y, dirWorld.z};
    desc.distantAngle = readFloat("angle", 0.53f);
    // DistantLight intensity is per-square-metre at the surface; no
    // area normalisation regardless of the `normalize` flag.
    areaForNormalize = 1.0f;
  }
  else if (prim.IsA<pxr::UsdLuxRectLight>()
        || prim.IsA<pxr::UsdLuxDiskLight>()
        || prim.IsA<pxr::UsdLuxSphereLight>())
  {
    // Three area-light shapes folded into Kind::Rect for the M7-simple
    // closesthit: it treats Rect as a point-at-center with axisU/V
    // packed for the M7-full pass to do real area sampling.
    desc.kind = LightDesc::Kind::Rect;
    const hlslpp::float4 originWorld =
        mul(worldFromLocal, hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f});
    desc.position = hlslpp::float3{originWorld.x, originWorld.y, originWorld.z};
    float halfU = 1.0f;
    float halfV = 1.0f;
    if (prim.IsA<pxr::UsdLuxRectLight>())
    {
      halfU = readFloat("width",  2.0f) * 0.5f;
      halfV = readFloat("height", 2.0f) * 0.5f;
    }
    else  // DiskLight or SphereLight — both author `radius`
    {
      const float radius = readFloat("radius", 0.5f);
      halfU = radius;
      halfV = radius;
    }
    const hlslpp::float4 uWorld =
        mul(worldFromLocal, hlslpp::float4{halfU, 0.0f, 0.0f, 0.0f});
    desc.axisU = hlslpp::float3{uWorld.x, uWorld.y, uWorld.z};
    const hlslpp::float4 vWorld =
        mul(worldFromLocal, hlslpp::float4{0.0f, halfV, 0.0f, 0.0f});
    desc.axisV = hlslpp::float3{vWorld.x, vWorld.y, vWorld.z};
    // World-space area: derive from the post-transform axisU/V so any
    // xform scale on the light prim (or a parent Xform) rolls into
    // the normalize divisor automatically. For Rect / Disk this also
    // correctly handles non-uniform scale (the disk becomes an
    // ellipse with area π·rU·rV). Sphere keeps the canonical 4π·r²
    // shape — non-uniform scale on a Sphere is rare + ill-defined
    // (the volume is no longer a sphere); we use the U axis as the
    // representative radius to keep behaviour stable, with a code-
    // level note pointing at the M9 ellipsoid-area refinement.
    const float worldHalfU = worldLength3(desc.axisU);
    const float worldHalfV = worldLength3(desc.axisV);
    if (prim.IsA<pxr::UsdLuxRectLight>())
      areaForNormalize = 4.0f * worldHalfU * worldHalfV;
    else if (prim.IsA<pxr::UsdLuxDiskLight>())
      areaForNormalize = PI_F * worldHalfU * worldHalfV;
    else  // SphereLight
      areaForNormalize = 4.0f * PI_F * worldHalfU * worldHalfU;
  }
  else if (prim.IsA<pxr::UsdLuxCylinderLight>())
  {
    // M8a stub: load the geometry into LightDesc but the M7-simple
    // closesthit doesn't render Cylinder yet.
    desc.kind = LightDesc::Kind::Cylinder;
    const float length = readFloat("length", 1.0f);
    const float radius = readFloat("radius", 0.5f);
    const hlslpp::float4 originWorld =
        mul(worldFromLocal, hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f});
    desc.position = hlslpp::float3{originWorld.x, originWorld.y, originWorld.z};
    // axisU = tube direction (USD convention: cylinder length along
    // local X), scaled to half-length. axisV = radial (along local Y),
    // scaled to radius. Closesthit's M9 cylinder pass reads both.
    const hlslpp::float4 uWorld =
        mul(worldFromLocal, hlslpp::float4{length * 0.5f, 0.0f, 0.0f, 0.0f});
    const hlslpp::float4 vWorld =
        mul(worldFromLocal, hlslpp::float4{0.0f, radius, 0.0f, 0.0f});
    desc.axisU = hlslpp::float3{uWorld.x, uWorld.y, uWorld.z};
    desc.axisV = hlslpp::float3{vWorld.x, vWorld.y, vWorld.z};
    // World-space cylinder lateral area = 2π · r · L. axisU is the
    // half-length, axisV is the radius — both already in world space
    // so xform scale flows through.
    const float worldHalfLen = worldLength3(desc.axisU);
    const float worldRadius  = worldLength3(desc.axisV);
    areaForNormalize = 2.0f * PI_F * worldRadius * (2.0f * worldHalfLen);
    log.Info(log::APP, "StageWalker: " + prim.GetPath().GetString()
                           + " UsdLuxCylinderLight loaded — closesthit "
                             "rendering deferred to M9.");
  }
  else if (prim.IsA<pxr::UsdLuxGeometryLight>())
  {
    // M8a stub: the geometry rel target → emitting mesh is the
    // structural piece we'd need a dedicated handle for. For now
    // carry the prim through as Kind::Geometry with position-at-origin;
    // M9 wires the `inputs:geometry` rel resolution + per-tri sampling.
    desc.kind = LightDesc::Kind::Geometry;
    const hlslpp::float4 originWorld =
        mul(worldFromLocal, hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f});
    desc.position = hlslpp::float3{originWorld.x, originWorld.y, originWorld.z};
    areaForNormalize = 1.0f;  // unknown until the geometry is resolved
    log.Info(log::APP, "StageWalker: " + prim.GetPath().GetString()
                           + " UsdLuxGeometryLight loaded — geometry rel "
                             "resolution + emission rendering deferred to M9.");
  }
  else if (prim.IsA<pxr::UsdLuxPortalLight>())
  {
    // M8a stub: portal lights are a sampling-variance hint for the
    // dome importance sampler. Carry as Kind::Portal so an M9 NEE
    // pass with importance sampling can link them to the active
    // dome; M7-simple just drops them on the floor (no contribution
    // to the simple Lambert sum).
    desc.kind = LightDesc::Kind::Portal;
    const hlslpp::float4 originWorld =
        mul(worldFromLocal, hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f});
    desc.position = hlslpp::float3{originWorld.x, originWorld.y, originWorld.z};
    const float width  = readFloat("width",  1.0f);
    const float height = readFloat("height", 1.0f);
    const hlslpp::float4 uWorld =
        mul(worldFromLocal, hlslpp::float4{width * 0.5f, 0.0f, 0.0f, 0.0f});
    const hlslpp::float4 vWorld =
        mul(worldFromLocal, hlslpp::float4{0.0f, height * 0.5f, 0.0f, 0.0f});
    desc.axisU = hlslpp::float3{uWorld.x, uWorld.y, uWorld.z};
    desc.axisV = hlslpp::float3{vWorld.x, vWorld.y, vWorld.z};
    // World portal area — same shape as a Rect (4 · halfU_world · halfV_world).
    const float worldHalfW = worldLength3(desc.axisU);
    const float worldHalfH = worldLength3(desc.axisV);
    areaForNormalize = 4.0f * worldHalfW * worldHalfH;
    log.Info(log::APP, "StageWalker: " + prim.GetPath().GetString()
                           + " UsdLuxPortalLight loaded — dome-importance "
                             "linking deferred to M9.");
  }
  else if (prim.IsA<pxr::UsdLuxDomeLight>())
  {
    desc.kind = LightDesc::Kind::Dome;
    // Dome texture format — USD allows latlong (default), mirroredBall,
    // angular. The miss shader currently assumes latlong; an M9 pass
    // adds the other UV mappings. Stash the format so the data
    // survives the ingest boundary.
    static const pxr::TfToken latLongTok("latlong");          // NOLINT(readability-identifier-naming)
    static const pxr::TfToken mirroredBallTok("mirroredBall");// NOLINT(readability-identifier-naming)
    static const pxr::TfToken angularTok("angular");          // NOLINT(readability-identifier-naming)
    static const pxr::TfToken automaticTok("automatic");      // NOLINT(readability-identifier-naming)
    const pxr::TfToken format = readToken("texture:format", automaticTok);
    if (format == mirroredBallTok)
      desc.domeFormat = LightDesc::DomeFormat::MirroredBall;
    else if (format == angularTok)
      desc.domeFormat = LightDesc::DomeFormat::Angular;
    else
      desc.domeFormat = LightDesc::DomeFormat::LatLong;
    if (desc.domeFormat != LightDesc::DomeFormat::LatLong)
    {
      log.Info(log::APP, "StageWalker: " + prim.GetPath().GetString()
                             + " dome texture:format = " + format.GetString()
                             + " — read but miss shader still samples as "
                               "lat-long (M9 follow-up).");
    }

    // Resolve the dome's `inputs:texture:file` SdfAssetPath through
    // USD's ArResolver (handles relative `@./default_sky.exr@`-style
    // refs against the .usd's parent directory) and AcquireTexture
    // it. The resulting TextureHandle is stored on LightDesc.envMap;
    // GpuScene::CommitResources resolves it to a bindless slot at
    // pack time, and PathTracePass reads the first live dome's
    // texture for the miss shader's lat-long sample.
    std::string resolvedPath;
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
        resolvedPath = assetPath.GetResolvedPath();
        if (resolvedPath.empty())
          resolvedPath = assetPath.GetAssetPath();
      }
    }
    // Fallback: when the scene authors a DomeLight without an
    // env-map (Omniverse Lobby, default cube fixtures, etc.), bind
    // the bundled `Resources/scenes/default_sky.exr` so the miss
    // shader has a real lat-long texture to sample. Without this
    // the dome falls back to flat color × intensity which gives
    // either a pitch-black or fully-saturated background.
    if (resolvedPath.empty())
    {
      const Path bundled =
          AssetLocator{}.LocateResource("scenes/default_sky.exr");
      if (!bundled.View().empty())
        resolvedPath.assign(bundled.View());
    }
    if (!resolvedPath.empty())
    {
      TextureKey key;
      key.resolvedPath = resolvedPath;
      key.role = TextureKey::Role::Emission;  // HDR env-map: linear, no sRGB EOTF
      desc.envMap = scene.AcquireTexture(key);
    }
    // M9-fidelity per-prim dome rotation. Read xformOp:rotateY (or
    // the Y component of xformOp:rotateXYZ) directly from the prim
    // — NOT from the composed worldFromLocal matrix, which already
    // bakes in the stage Z→Y correction. UsdLuxDomeLight's typical
    // authoring is a horizontal HDRI spin around world-Y; X / Z
    // axes on a dome are uncommon and deferred. Convert degrees →
    // radians for the miss-shader trig.
    {
      double rotateY = 0.0;
      if (const pxr::UsdAttribute rotYAttr =
              prim.GetAttribute(pxr::TfToken("xformOp:rotateY")))
      {
        rotYAttr.Get(&rotateY);
      }
      else if (const pxr::UsdAttribute rotXYZAttr =
                   prim.GetAttribute(pxr::TfToken("xformOp:rotateXYZ")))
      {
        pxr::GfVec3d rotXYZ(0.0, 0.0, 0.0);
        rotXYZAttr.Get(&rotXYZ);
        rotateY = rotXYZ[1];
      }
      desc.domeRotationY = static_cast<float>(rotateY * (PI_F / 180.0f));
    }
    // Dome's "area" is a full sphere of solid angle; the closesthit
    // already integrates over the sphere when sampling the env-map,
    // so leave areaForNormalize at 1 and let the dome's intensity
    // carry the radiance unit straight through.
    areaForNormalize = 1.0f;
  }
  else
  {
    return;  // unknown light kind
  }

  // ---- M8a area-to-power conversion (OVRTX / RenderMan / Karma) -------
  // USD's `inputs:intensity` semantics:
  //   normalize=false (default): intensity is per-area surface radiance
  //                              in nits (cd/m²) — what each square
  //                              metre of the light emits.
  //   normalize=true:  intensity is the TOTAL emitted radiant power
  //                    (candela-equivalent for the whole light).
  // The closesthit's M7-simple area-light path treats every light as a
  // point at centre and applies inverse-square distance falloff:
  //   contribution = (color × intensity) × NdotL / r²
  // For that to be dimensionally correct, `intensity` must be in
  // RADIANT INTENSITY units (candela). Convert at load time:
  //   normalize=true:  intensity already total power, pass through.
  //   normalize=false: multiply by world-space area to get total power
  //                    (= per-sqm radiance × m² of emitting surface).
  // Without this conversion, a 60000-nit DiskLight (radius 2cm, area
  // ≈ 0.00126 m²) would render as if it were 60000 cd — ~800× too
  // bright. With the conversion, it becomes the correct ~75 cd that a
  // small bright LED downlight actually emits, and the camera's heavy
  // negative exposure stops bring the result into display range.
  // Distant + Dome leave areaForNormalize at 1.0 (no area concept) so
  // the multiply collapses to a no-op for them.
  if (!desc.normalize && areaForNormalize > 1e-6f)
  {
    desc.intensity *= areaForNormalize;
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

// Emit a single UsdGeomMesh into `scene`. Returns the number of
// (mesh, instance) pairs successfully emitted — typically 1 for a
// plain mesh; N for a mesh with N face-bound UsdGeomSubsets (each
// subset becomes its own MeshHandle + InstanceHandle so the §15
// instance→material side-table can route closesthits per subset).
//
// Triangulation: GpuScene::CreateMesh expects a triangle list, but USD
// authors arbitrary faceVertexCounts (most production / Omniverse
// scenes are quad-dominant). Each face with N vertices fan-
// triangulates to N-2 triangles `(v0, v_{i+1}, v_{i+2})`. Convex-
// polygon assumption — concave faces produce visually-wrong (but
// non-crashing) triangles. Subdivision is §42-deferred so authoring
// concave ngons that need ear-clipping is post-v1.
//
// M8a face-subset support: when the prim has children of type
// `UsdGeomSubset` with `elementType = "face"`, each subset's face
// list is sliced out into its own sub-mesh + sub-instance, bound to
// the subset's `material:binding`. Faces NOT covered by any subset
// fall back to the prim-level material in a "_unassigned" sub-mesh.
// Positions are duplicated across subsets (each sub-BLAS holds the
// full vertex array but only references its subset's faces) — the
// canonical position-reindexing optimisation is M9 polish.
//
// Side-effect: `stats.meshesEmitted` and `stats.instancesEmitted`
// are bumped per sub-mesh inside this function so the caller doesn't
// need to know about subsets. Returns the per-prim count for the
// caller's local diagnostics.
// Prepare mesh data from a USD prim — pure CPU work, NO GpuScene
// mutation. Safe to call concurrently from multiple worker threads
// PROVIDED each thread passes its own xformCache (UsdGeomXformCache
// is not thread-safe per pxr docs) and materialsByPath /
// materialsNeedingTangents stay const-shared.
PreparedMesh PrepareMesh(const pxr::UsdPrim& prim, pxr::UsdGeomXformCache& xformCache,
                         const MaterialHandleByPath& materialsByPath,
                         const MaterialsNeedingTangents& materialsNeedingTangents,
                         const StageContext& stageCtx) noexcept {
  auto& log = Logging::Get();
  PreparedMesh prepared;
  const pxr::UsdGeomMesh meshPrim(prim);
  if (!meshPrim.GetPrim().IsValid())
    return prepared;

  pxr::VtArray<pxr::GfVec3f> usdPoints;
  pxr::VtArray<int> usdCounts;
  pxr::VtArray<int> usdIndices;
  meshPrim.GetPointsAttr().Get(&usdPoints);
  meshPrim.GetFaceVertexCountsAttr().Get(&usdCounts);
  meshPrim.GetFaceVertexIndicesAttr().Get(&usdIndices);

  if (usdPoints.empty() || usdCounts.empty() || usdIndices.empty())
    return prepared;

  // Convert positions into the §18.4 contiguous layout. Shared
  // across every sub-mesh emitted from this prim.
  std::vector<hlslpp::float3> positions;
  positions.reserve(usdPoints.size());
  for (const pxr::GfVec3f& point : usdPoints)
    positions.emplace_back(point[0], point[1], point[2]);

  // Per-face start offset into usdIndices — used by both the UV
  // extractor (faceVarying needs face-local fv-index lookup) and the
  // subset path (slice the fv-stream by face id).
  std::vector<std::size_t> faceStartOffsets(usdCounts.size() + 1u, 0u);
  {
    std::size_t running = 0;
    for (std::size_t faceIdx = 0; faceIdx < usdCounts.size(); ++faceIdx)
    {
      faceStartOffsets[faceIdx] = running;
      running += static_cast<std::size_t>(usdCounts[faceIdx] > 0 ? usdCounts[faceIdx] : 0);
    }
    faceStartOffsets.back() = running;
    if (running != usdIndices.size())
    {
      log.Warn(log::APP, "StageWalker: " + prim.GetPath().GetString()
                             + " faceVertexCounts/Indices length mismatch (sum "
                             + std::to_string(running) + " vs indices "
                             + std::to_string(usdIndices.size())
                             + "); dropping mesh.");
      return prepared;
    }
  }

  // ---- §M8a face-subset collection ---------------------------------
  // GeomSubsets with elementType=face partition the mesh into named
  // face groups, each carrying its own material:binding. Walk them
  // here, build a per-subset emit plan, and fall any unassigned faces
  // back to the prim-level material binding.
  struct SubsetEmitInfo {
    pxr::VtIntArray faceIndices;  // empty = "use every face" (no-subset path)
    MaterialHandle  material = MaterialHandle::Invalid;
    std::string     debugSuffix;  // appended to prim path; empty for whole-mesh
  };
  std::vector<SubsetEmitInfo> subsetInfos;

  const std::vector<pxr::UsdGeomSubset> faceSubsets =
      pxr::UsdGeomSubset::GetGeomSubsets(meshPrim, pxr::UsdGeomTokens->face);
  if (faceSubsets.empty())
  {
    SubsetEmitInfo info;
    info.material = ResolveBoundMaterial(prim, materialsByPath);
    subsetInfos.push_back(std::move(info));
  }
  else
  {
    std::vector<bool> faceAssigned(usdCounts.size(), false);
    for (const pxr::UsdGeomSubset& subset : faceSubsets)
    {
      pxr::VtIntArray subsetIndices;
      subset.GetIndicesAttr().Get(&subsetIndices);
      if (subsetIndices.empty())
        continue;
      SubsetEmitInfo info;
      info.material = ResolveBoundMaterial(subset.GetPrim(), materialsByPath);
      info.debugSuffix = "/" + subset.GetPrim().GetName().GetString();
      info.faceIndices.reserve(subsetIndices.size());
      for (const int faceIdx : subsetIndices)
      {
        if (faceIdx >= 0 && static_cast<std::size_t>(faceIdx) < usdCounts.size())
        {
          info.faceIndices.push_back(faceIdx);
          faceAssigned[static_cast<std::size_t>(faceIdx)] = true;
        }
      }
      if (!info.faceIndices.empty())
        subsetInfos.push_back(std::move(info));
    }
    // Faces not claimed by any subset fall back to the prim-level
    // material — UsdGeomSubset::familyName=partition would forbid this
    // configuration but the more permissive nonOverlapping family
    // (USD default) allows uncovered faces. Emit them as one extra
    // sub-mesh so the surface is fully drawn.
    SubsetEmitInfo unassigned;
    for (std::size_t faceIdx = 0; faceIdx < usdCounts.size(); ++faceIdx)
    {
      if (!faceAssigned[faceIdx])
        unassigned.faceIndices.push_back(static_cast<int>(faceIdx));
    }
    if (!unassigned.faceIndices.empty())
    {
      unassigned.material = ResolveBoundMaterial(prim, materialsByPath);
      unassigned.debugSuffix = "/_unassigned";
      subsetInfos.push_back(std::move(unassigned));
    }
  }

  // ---- UV source data (read once; sliced per subset below) ----------
  // primvars:st for the WHOLE mesh. The per-subset UV emit below
  // walks either the full face stream (no-subset case) or the
  // subset's face indices (subset case) into the size-matched
  // vertexUvs buffer per sub-mesh.
  pxr::VtArray<pxr::GfVec2f> stUvs;
  pxr::TfToken               stInterpolation;
  bool                       stHasValue = false;
  {
    const pxr::UsdGeomPrimvarsAPI primvarsApi(prim);
    const pxr::UsdGeomPrimvar     stPrimvar = primvarsApi.GetPrimvar(pxr::TfToken("st"));
    if (stPrimvar.HasValue())
    {
      // ComputeFlattened (NOT plain Get) so indexed primvars expand
      // through their `primvars:st:indices` side-table. Production
      // USD (Omniverse, Animal Logic, Disney) authors UV islands as
      // `values=[N unique uvs] + indices=[per-faceVertex int]` to
      // dedupe, and Get() would return just the N unique values —
      // tripping our `size == fvCount` length check below and
      // silently dropping every mesh's UVs.
      stPrimvar.ComputeFlattened(&stUvs);
      stInterpolation = stPrimvar.GetInterpolation();
      stHasValue = true;
    }
  }

  // ---- Normals source data (M9 smooth-shading) ----------------------
  // UsdGeomMesh authors normals as either the schema-level
  // `normals` attribute (the most common case — what the lobby uses)
  // or as a `primvars:normals` primvar (modern style). Try the
  // schema attr first; if absent, try the primvar. Closesthit reads
  // these via per-vertex barycentric interpolation; in the face-
  // varying case we collapse to per-vertex (taking the first normal
  // per shared vertex), losing hard-edge fidelity. Vertex
  // duplication for accurate hard edges is M11+ polish — for now
  // smooth shading on shared edges is a major visual upgrade vs the
  // M7-simple per-face fallback.
  pxr::VtArray<pxr::GfVec3f> normalsArr;
  pxr::TfToken               normalsInterpolation;
  bool                       normalsHasValue = false;
  {
    const pxr::UsdAttribute schemaNormalsAttr = meshPrim.GetNormalsAttr();
    if (schemaNormalsAttr && schemaNormalsAttr.HasAuthoredValue()
        && schemaNormalsAttr.Get(&normalsArr) && !normalsArr.empty())
    {
      normalsInterpolation = meshPrim.GetNormalsInterpolation();
      normalsHasValue = true;
    }
    else
    {
      const pxr::UsdGeomPrimvarsAPI primvarsApi(prim);
      const pxr::UsdGeomPrimvar     normalsPrimvar =
          primvarsApi.GetPrimvar(pxr::TfToken("normals"));
      if (normalsPrimvar.HasValue())
      {
        normalsPrimvar.ComputeFlattened(&normalsArr);
        if (!normalsArr.empty())
        {
          normalsInterpolation = normalsPrimvar.GetInterpolation();
          normalsHasValue = true;
        }
      }
    }
  }

  // ---- Per-subset accumulation loop --------------------------------
  // Builds prepared.subMeshes; the GpuScene mutation calls live in
  // EmitPreparedMesh below.
  prepared.worldFromLocal = ComposeWorldFromLocal(prim, xformCache, stageCtx);
  const std::string primPath = prim.GetPath().GetString();

  // Cached interpolation-mode flags — read once, branched per face-
  // vertex below. The `faceVarying` length check (.size() == fvCount)
  // is the tripwire that catches mis-authored primvars; when it
  // fails we treat the channel as absent rather than indexing past
  // the array's end.
  const bool hasNormals       = normalsHasValue && !normalsArr.empty();
  const bool normalsFv        = hasNormals
                                && normalsInterpolation == pxr::UsdGeomTokens->faceVarying
                                && normalsArr.size() == usdIndices.size();
  const bool normalsVertex    = hasNormals
                                && normalsInterpolation == pxr::UsdGeomTokens->vertex
                                && normalsArr.size() == usdPoints.size();
  const bool normalsConstant  = hasNormals
                                && (normalsInterpolation == pxr::UsdGeomTokens->constant
                                    || normalsInterpolation == pxr::UsdGeomTokens->uniform);
  const bool emitNormals      = normalsFv || normalsVertex || normalsConstant;

  const bool hasUvs           = stHasValue && !stUvs.empty();
  const bool uvsFv            = hasUvs
                                && stInterpolation == pxr::UsdGeomTokens->faceVarying
                                && stUvs.size() == usdIndices.size();
  const bool uvsVertex        = hasUvs
                                && stInterpolation == pxr::UsdGeomTokens->vertex
                                && stUvs.size() == usdPoints.size();
  const bool uvsConstant      = hasUvs
                                && (stInterpolation == pxr::UsdGeomTokens->constant
                                    || stInterpolation == pxr::UsdGeomTokens->uniform);

  for (const SubsetEmitInfo& subset : subsetInfos)
  {
    // M9-fidelity hard-edge dedup. Walks the face-vertex stream once
    // per subset. Each face-vertex contributes a (position, normal,
    // uv) triple; identical triples sharing a position collapse into
    // the same output vertex slot, distinct triples (faceVarying
    // boundaries — UV seams + crease edges) split into separate
    // slots. Per-subset arrays are minimal — only positions actually
    // touched by the subset's faces appear in the output.
    auto getNormal = [&](std::size_t fvIdx, std::uint32_t positionIdx) -> hlslpp::float3 {
      if (normalsFv)
      {
        const auto& src = normalsArr[fvIdx];
        return hlslpp::float3{src[0], src[1], src[2]};
      }
      if (normalsVertex)
      {
        const auto& src = normalsArr[positionIdx];
        return hlslpp::float3{src[0], src[1], src[2]};
      }
      if (normalsConstant)
      {
        const auto& src = normalsArr[0];
        return hlslpp::float3{src[0], src[1], src[2]};
      }
      return hlslpp::float3{0.0f, 0.0f, 0.0f};
    };
    auto getUv = [&](std::size_t fvIdx, std::uint32_t positionIdx) -> hlslpp::float2 {
      if (uvsFv)
      {
        const auto& uvSrc = stUvs[fvIdx];
        return hlslpp::float2{uvSrc[0], uvSrc[1]};
      }
      if (uvsVertex)
      {
        const auto& uvSrc = stUvs[positionIdx];
        return hlslpp::float2{uvSrc[0], uvSrc[1]};
      }
      if (uvsConstant)
      {
        const auto& uvSrc = stUvs[0];
        return hlslpp::float2{uvSrc[0], uvSrc[1]};
      }
      return hlslpp::float2{0.0f, 0.0f};
    };

    // Per-position chain head — `headByPosition[pi]` indexes into
    // `entries` for the first VertexEntry attached to USD-position
    // `pi`, or -1 if none yet. Walked linearly per face-vertex
    // lookup; chain length is 1 for interior shared positions, 2–4
    // for UV-seam / crease corners.
    //
    // Reused across submeshes via thread_local storage. Each OpenMP
    // worker thread keeps its own scratch with capacity preserved
    // between meshes — first iteration pays for the alloc, subsequent
    // iterations only pay for fill (assign / clear). This eliminates
    // the per-submesh allocator-mutex contention that was the
    // dominant Debug pass3b cost.
    //
    // (PrepareMesh lives in an anonymous namespace so it's not
    // dllexport — `thread_local` is legal here. The lambda restriction
    // we hit earlier only applies to `thread_local` declared INSIDE
    // a lambda body that captures into an exported DLL.)
    thread_local std::vector<std::int32_t>      headByPosition;
    thread_local std::vector<VertexEntry>       entries;
    thread_local std::vector<hlslpp::float3>    outPositions;
    thread_local std::vector<hlslpp::float3>    outNormals;
    thread_local std::vector<hlslpp::float2>    outUvs;
    thread_local std::vector<std::uint32_t>     indices;
    headByPosition.assign(usdPoints.size(), -1);  // resize + fill, capacity preserved
    entries.clear();
    outPositions.clear();
    outNormals.clear();
    outUvs.clear();
    indices.clear();
    {
      const std::size_t fvUpperBound =
          subset.faceIndices.empty() ? usdIndices.size()
                                     : subset.faceIndices.size() * 4u;
      entries.reserve(fvUpperBound);
      outPositions.reserve(fvUpperBound);
      if (emitNormals)
        outNormals.reserve(fvUpperBound);
      outUvs.reserve(fvUpperBound);
      indices.reserve(fvUpperBound * 3u / 2u);
    }

    auto pushVertex = [&](std::size_t fvIdx) -> std::uint32_t {
      const auto positionIdx = static_cast<std::uint32_t>(usdIndices[fvIdx]);
      if (positionIdx >= usdPoints.size())
        return 0u;  // bounds-check defence; broken meshes drop bad fv refs
      const hlslpp::float3 normal = getNormal(fvIdx, positionIdx);
      const hlslpp::float2 uvOut  = getUv(fvIdx, positionIdx);

      // Bit-cast key components — operator== over bit-patterns
      // catches mathematically-distinct floats (e.g. NaNs) that
      // memcmp also distinguishes, and keeps -0 / +0 as separate
      // keys (deterministic, matches authoring intent).
      const float normalArr[3] = {static_cast<float>(normal.x),
                                  static_cast<float>(normal.y),
                                  static_cast<float>(normal.z)};
      const float uvArr[2]     = {static_cast<float>(uvOut.x),
                                  static_cast<float>(uvOut.y)};
      std::uint32_t normalBits[3];
      std::uint32_t uvBits[2];
      std::memcpy(normalBits, normalArr, sizeof(normalArr));
      std::memcpy(uvBits,     uvArr,     sizeof(uvArr));

      // Walk the chain attached to this position; reuse on match.
      for (std::int32_t cur = headByPosition[positionIdx]; cur >= 0; )
      {
        const VertexEntry& cand = entries[static_cast<std::size_t>(cur)];
        if (cand.normalBits[0] == normalBits[0]
            && cand.normalBits[1] == normalBits[1]
            && cand.normalBits[2] == normalBits[2]
            && cand.uvBits[0]     == uvBits[0]
            && cand.uvBits[1]     == uvBits[1])
          return cand.outIdx;
        cur = cand.next;
      }

      // No match — emit a new vertex slot + link into the chain.
      const auto newIdx = static_cast<std::uint32_t>(outPositions.size());
      outPositions.push_back(positions[positionIdx]);
      if (emitNormals)
        outNormals.push_back(normal);
      outUvs.push_back(uvOut);
      VertexEntry entry{};
      entry.normalBits[0] = normalBits[0];
      entry.normalBits[1] = normalBits[1];
      entry.normalBits[2] = normalBits[2];
      entry.uvBits[0]     = uvBits[0];
      entry.uvBits[1]     = uvBits[1];
      entry.outIdx        = newIdx;
      entry.next          = headByPosition[positionIdx];
      entries.push_back(entry);
      headByPosition[positionIdx] = static_cast<std::int32_t>(entries.size() - 1u);
      return newIdx;
    };

    auto emitTrianglesForFace = [&](std::size_t faceIdx) {
      const int faceCount = usdCounts[faceIdx];
      if (faceCount < 3)
        return;
      const std::size_t fvBase = faceStartOffsets[faceIdx];
      for (int triIdx = 0; triIdx < faceCount - 2; ++triIdx)
      {
        indices.push_back(pushVertex(fvBase + 0u));
        indices.push_back(pushVertex(fvBase + static_cast<std::size_t>(triIdx + 1)));
        indices.push_back(pushVertex(fvBase + static_cast<std::size_t>(triIdx + 2)));
      }
    };
    if (subset.faceIndices.empty())
    {
      for (std::size_t faceIdx = 0; faceIdx < usdCounts.size(); ++faceIdx)
        emitTrianglesForFace(faceIdx);
    }
    else
    {
      for (const int faceIdx : subset.faceIndices)
        emitTrianglesForFace(static_cast<std::size_t>(faceIdx));
    }
    if (indices.empty())
    {
      log.Warn(log::APP, "StageWalker: " + primPath + subset.debugSuffix
                             + " has no valid faces after triangulation; skipping.");
      continue;
    }

    // Per-vertex tangents via MikkTSpace on the deduplicated arrays.
    // Hard-edge dup means each (face, vert) maps to a vertex slot
    // unique to its UV/normal combination, so MikkTSpace's per-(face,
    // vert) tangent writes naturally land on the right slot — the
    // first-tangent-wins guard becomes a no-op. Requires non-empty
    // UVs + vertex normals + at least one triangle, AND the bound
    // material to actually carry a normal map (the closesthit's
    // MATERIAL_FLAG_HAS_NORMAL_MAP gate is the only consumer). The
    // material check is the dominant perf win — 40% of lobby
    // sub-meshes have no normal map and now skip MikkTSpace entirely.
    const bool subsetNeedsTangents =
        subset.material != MaterialHandle::Invalid
        && materialsNeedingTangents.contains(subset.material);
    thread_local std::vector<hlslpp::float4> vertexTangents;
    thread_local std::vector<std::uint8_t>   tangentAssigned;
    vertexTangents.clear();
    tangentAssigned.clear();
    if (subsetNeedsTangents && !indices.empty() && emitNormals
        && (uvsFv || uvsVertex || uvsConstant))
    {
      vertexTangents.assign(outPositions.size(),
                            hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f});
      // std::vector<bool> is bit-packed; MikkTSpace's per-FV write
      // path needs a plain bool* for the cached-pointer path. Use
      // a uint8_t-typed vector so &v[i] is a sane T* pointer.
      tangentAssigned.assign(outPositions.size(), 0u);
      MikkContext userData{};
      userData.triangleIndices    = indices.data();
      userData.triangleVertCount  = indices.size();
      userData.positions          = outPositions.data();
      userData.positionsCount     = outPositions.size();
      userData.vertexNormals      = outNormals.data();
      userData.vertexUvs          = outUvs.data();
      userData.outVertexTangents  = vertexTangents.data();
      userData.outTangentAssigned = reinterpret_cast<bool*>(tangentAssigned.data());
      userData.outVertexCount     = vertexTangents.size();
      SMikkTSpaceInterface mikkInterface{};
      mikkInterface.m_getNumFaces           = MikkGetNumFaces;
      mikkInterface.m_getNumVerticesOfFace  = MikkGetNumVerticesOfFace;
      mikkInterface.m_getPosition           = MikkGetPosition;
      mikkInterface.m_getNormal             = MikkGetNormal;
      mikkInterface.m_getTexCoord           = MikkGetTexCoord;
      mikkInterface.m_setTSpaceBasic        = MikkSetTSpaceBasic;
      SMikkTSpaceContext mikkCtx{};
      mikkCtx.m_pInterface = &mikkInterface;
      mikkCtx.m_pUserData  = &userData;
      if (!genTangSpaceDefault(&mikkCtx))
      {
        // MikkTSpace failed (degenerate UVs across the whole mesh,
        // most likely) — drop the tangent buffer so the closesthit's
        // normal-mapping branch falls back to vertex-normal-only.
        vertexTangents.clear();
      }
    }

    // Copy (not move) into the per-thread output: move would steal
    // the thread_local scratch buffer, dropping its capacity to 0
    // and forcing the next submesh's first push_back to reallocate
    // — defeating the whole point of the thread_local pool. assign()
    // copies into a fresh PreparedSubMesh allocation (single alloc
    // per channel per submesh, paid by the worker thread on the
    // contended Debug heap) but leaves the scratch capacity intact.
    PreparedSubMesh subMesh;
    subMesh.positions.assign(outPositions.begin(), outPositions.end());
    subMesh.indices  .assign(indices.begin(),      indices.end());
    subMesh.normals  .assign(outNormals.begin(),   outNormals.end());
    subMesh.tangents .assign(vertexTangents.begin(), vertexTangents.end());
    subMesh.uv0      .assign(outUvs.begin(),       outUvs.end());
    subMesh.material  = subset.material;
    subMesh.debugName = primPath + subset.debugSuffix;
    prepared.subMeshes.push_back(std::move(subMesh));
  }
  return prepared;
}

// EmitPreparedMesh pushes a PreparedMesh into GpuScene — the
// single-writer half of the split. Must run on the render / main
// thread per §30.11. Bumps stats.meshesEmitted /
// stats.instancesEmitted per sub-mesh; returns the count actually
// emitted (0 when prep failed or every CreateMesh / AppendInstance
// failed).
std::size_t EmitPreparedMesh(const PreparedMesh& prepared, GpuScene& scene,
                             IngestStats& stats) noexcept {
  auto& log = Logging::Get();
  std::size_t emittedCount = 0;
  for (const PreparedSubMesh& subMesh : prepared.subMeshes)
  {
    MeshDesc meshDesc;
    meshDesc.positions = subMesh.positions;
    meshDesc.indices   = subMesh.indices;
    meshDesc.uv0       = subMesh.uv0;
    meshDesc.normals   = subMesh.normals;
    meshDesc.tangents  = subMesh.tangents;
    meshDesc.debugName = subMesh.debugName;
    const auto meshHandle = scene.CreateMesh(meshDesc);
    if (!meshHandle.has_value())
    {
      log.Error(log::APP, "StageWalker: CreateMesh failed for "
                              + subMesh.debugName + ": "
                              + std::string{meshHandle.error().message.View()});
      continue;
    }
    ++stats.meshesEmitted;

    // World transform → instance. Material binding came from the
    // subset (or the prim-level fallback) at PrepareMesh time.
    // ComposeWorldFromLocal bakes metersPerUnit + Z->Y if the stage
    // metadata says so, so the BLAS keeps stage-unit local-space
    // geometry while the per-instance transform places it in
    // Pyxis-world (metres + Y-up).
    InstanceDesc instanceDesc;
    instanceDesc.mesh           = *meshHandle;
    instanceDesc.worldFromLocal = prepared.worldFromLocal;
    instanceDesc.material       = subMesh.material;
    instanceDesc.debugName      = subMesh.debugName;
    const auto instanceHandle = scene.AppendInstance(instanceDesc);
    if (!instanceHandle.has_value())
    {
      log.Error(log::APP, "StageWalker: AppendInstance failed for "
                              + subMesh.debugName + ": "
                              + std::string{instanceHandle.error().message.View()});
      continue;
    }
    ++stats.instancesEmitted;
    ++emittedCount;
  }
  return emittedCount;
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

IngestResult StageWalker::WalkFile(std::string_view usdPath, GpuScene& scene) {
  auto& log = Logging::Get();
  const std::string pathString{usdPath};

  using Clock = std::chrono::steady_clock;
  const auto walkStart = Clock::now();
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(pathString);
  const auto stageOpenEnd = Clock::now();
  if (!stage)
  {
    log.Error(log::APP, std::string{"StageWalker: failed to open "} + pathString);
    IngestResult failed;
    failed.GetImpl().stats.timings.stageOpenMs =
        std::chrono::duration<float, std::milli>(stageOpenEnd - walkStart).count();
    failed.GetImpl().stats.timings.totalMs = failed.GetImpl().stats.timings.stageOpenMs;
    return failed;
  }
  IngestResult result = WalkStage(stage, scene);
  const auto walkEnd = Clock::now();
  // stageOpenMs is local to WalkFile (WalkStage didn't open the
  // stage); fold it in + recompute totalMs to include it.
  IngestStats& stats = result.GetImpl().stats;
  stats.timings.stageOpenMs =
      std::chrono::duration<float, std::milli>(stageOpenEnd - walkStart).count();
  stats.timings.totalMs =
      std::chrono::duration<float, std::milli>(walkEnd - walkStart).count();
  auto fmtMs = [](float milliseconds) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0fms", milliseconds);
    return std::string{buf};
  };
  log.Info(log::APP,
           "StageWalker: " + pathString + " walked — "
               + std::to_string(stats.meshesEmitted) + " meshes, "
               + std::to_string(stats.instancesEmitted) + " instances ("
               + std::to_string(stats.instancersEmitted) + " instancers), "
               + std::to_string(stats.materialsEmitted) + " materials, "
               + std::to_string(stats.lightsEmitted) + " lights, "
               + std::to_string(stats.camerasEmitted) + " cameras, "
               + std::to_string(stats.skipped) + " skipped.");
  log.Info(log::APP,
           "StageWalker timing: stageOpen=" + fmtMs(stats.timings.stageOpenMs)
               + " traverseSort=" + fmtMs(stats.timings.traverseSortMs)
               + " prewarm=" + fmtMs(stats.timings.prewarmPassMs)
               + " materialPass=" + fmtMs(stats.timings.materialPassMs)
               + " instancerPass=" + fmtMs(stats.timings.instancerPassMs)
               + " meshLightCamera=" + fmtMs(stats.timings.meshLightCameraMs)
               + " total=" + fmtMs(stats.timings.totalMs));
  return result;
}

IngestResult StageWalker::WalkStage(const pxr::UsdStageRefPtr& stage,
                                    GpuScene& scene) {
  IngestResult result;
  if (!stage)
    return result;
  IngestStats& stats = result.GetImpl().stats;
  auto& cameras = result.GetImpl().cameras;

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
  stats.timings.traverseSortMs =
      std::chrono::duration<float, std::milli>(traverseEnd - walkStart).count();

  // Parallel attribute pre-warm was tried + dropped — the temporary
  // VtArrays go out of scope before pass 3 re-reads the same attrs,
  // and USD's per-layer crate cache evicts decoded values without an
  // owned reference holding them. Both mesh- and material-targeted
  // variants ended up net-negative (1.4–2.2s overhead, < 0.15s
  // saved). The `prewarmPassMs` timing field remains in IngestStats
  // as a hook for a future "actual parallel-into-intermediates"
  // pipeline that captures the decoded values into long-lived PODs.
  stats.timings.prewarmPassMs = 0.0f;

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
  MaterialHandleByPath       materialsByPath;
  MaterialsNeedingTangents   materialsNeedingTangents;
  // Texture-acquisition callback for the translator — stateless
  // function pointer + opaque scene-pointer userData. The translator
  // stays renderer-agnostic; the role passes through unchanged so
  // the renderer's §13 texture cache picks the colorspace EOTF on
  // decode (sRGB for BaseColor + Emission; linear for everything
  // else).
  static constexpr auto ACQUIRE_TEXTURE =
      [](std::string_view resolvedPath, TextureKey::Role role,
         void* userData) -> TextureHandle {
        TextureKey key;
        key.resolvedPath = resolvedPath;
        key.role = role;
        return static_cast<GpuScene*>(userData)->AcquireTexture(key);
      };
  const auto materialPassStart = Clock::now();
  for (const pxr::UsdPrim& prim : prims)
  {
    if (!prim.IsA<pxr::UsdShadeMaterial>())
      continue;
    const pxr::UsdShadeMaterial materialPrim(prim);
    const std::string primPath = prim.GetPath().GetString();
    OpenPBRMaterialDesc materialDesc =
        material_translation::FromUsdShade(materialPrim, ACQUIRE_TEXTURE, &scene);
    materialDesc.sourcePrim = primPath;
    const MaterialHandle handle = scene.AcquireMaterial(materialDesc);
    materialsByPath.emplace(primPath, handle);
    // Track which materials carry a normal map so the mesh pass can
    // skip MikkTSpace tangent generation on meshes that don't need
    // them. ~40% of lobby materials are normal-map-free; skipping
    // their meshes' MikkTSpace was the dominant ingest-perf win.
    if (materialDesc.normalMap != TextureHandle::Invalid)
      materialsNeedingTangents.insert(handle);
    ++stats.materialsEmitted;
  }
  const auto materialPassEnd = Clock::now();
  stats.timings.materialPassMs =
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
  stats.timings.instancerPassMs =
      std::chrono::duration<float, std::milli>(instancerPassEnd - instancerPassStart).count();

  // Pass 3 — meshes / camera / lights, with material handles resolved
  // from pass 1 and instancer prototypes skipped (pass 2 already
  // expanded them).
  //
  // Mesh prep runs in parallel across the asset I/O pool (§31). The
  // heavy work (USD attr reads, hard-edge dedup, MikkTSpace tangents)
  // is pure CPU and doesn't touch GpuScene, so it parallelises well.
  // The actual GpuScene mutations (CreateMesh + AppendInstance) stay
  // on this thread to honour §30.11's single-writer-Flecs constraint.
  // SdfPath-sorted ordering is preserved by indexing preparedMeshes
  // in iteration order.
  const auto meshPassStart = Clock::now();

  // Pass 3a — collect mesh prim indices (skipping consumed prototypes)
  // into a parallel-prep work list.
  std::vector<std::size_t> meshPrimIndices;
  meshPrimIndices.reserve(prims.size());
  for (std::size_t i = 0; i < prims.size(); ++i)
  {
    if (!prims[i].IsA<pxr::UsdGeomMesh>())
      continue;
    if (consumedPrototypes.contains(prims[i].GetPath().GetString()))
      continue;
    meshPrimIndices.push_back(i);
  }

  // Pass 3b — parallel mesh prep. Each worker gets its own
  // UsdGeomXformCache (pxr's cache isn't thread-safe). The
  // materialsByPath / materialsNeedingTangents maps are const-shared.
  // PrepareMesh writes only to its assigned slot in preparedMeshes
  // and to the worker's local xformCache; no cross-worker state.
  std::vector<PreparedMesh> preparedMeshes(meshPrimIndices.size());
  const auto pass3bStart = Clock::now();
  // OpenMP parallel-for. Lower per-task overhead than MSVC's PPL/
  // ConcRT (used by std::execution::par) — measured benefit on the
  // lobby in Debug (where PPL's task-tracking machinery + Debug
  // heap contention made std::execution::par 25% SLOWER than seq).
  // Release: ~3-4x speedup with 8 cores.
  const std::int64_t meshCount = static_cast<std::int64_t>(meshPrimIndices.size());
#pragma omp parallel for schedule(dynamic, 4)
  for (std::int64_t outIdx = 0; outIdx < meshCount; ++outIdx)
  {
    // Per-task xformCache — pxr::UsdGeomXformCache isn't thread-
    // safe. We lose cross-sibling-mesh xform caching but each
    // per-prim xform-chain walk is small.
    pxr::UsdGeomXformCache localXformCache;
    const std::size_t primIdx = meshPrimIndices[static_cast<std::size_t>(outIdx)];
    preparedMeshes[static_cast<std::size_t>(outIdx)] =
        PrepareMesh(prims[primIdx], localXformCache, materialsByPath,
                    materialsNeedingTangents, stageCtx);
  }
  const auto pass3bEnd = Clock::now();
  const auto pass3bMs =
      std::chrono::duration<float, std::milli>(pass3bEnd - pass3bStart).count();
  Logging::Get().Info(log::APP,
      "StageWalker pass3b (parallel mesh prep): " + std::to_string(static_cast<int>(pass3bMs)) + "ms");

  // Pass 3c — single-writer drain: walk prims in SdfPath order and
  // emit prepared meshes / cameras / lights in lockstep.
  const auto pass3cStart = Clock::now();
  std::size_t nextPreparedIdx = 0;
  for (const pxr::UsdPrim& prim : prims)
  {
    if (prim.IsA<pxr::UsdGeomMesh>())
    {
      if (consumedPrototypes.contains(prim.GetPath().GetString()))
        continue;
      EmitPreparedMesh(preparedMeshes[nextPreparedIdx], scene, stats);
      ++nextPreparedIdx;
    }
    else if (prim.IsA<pxr::UsdGeomCamera>())
    {
      // Build the desc + collect into the stats list — the editor
      // will populate a Scene-Camera combo from these. Active-camera
      // selection happens AFTER the loop so we can honour the
      // root-layer's `boundCamera` hint regardless of SdfPath order.
      if (auto descOpt = BuildCameraDesc(prim, xformCache, stageCtx); descOpt)
      {
        IngestResult::Impl::Entry entry;
        entry.name = prim.GetPath().GetString();
        entry.desc = *descOpt;
        cameras.push_back(std::move(entry));
        ++stats.camerasEmitted;
      }
    }
    else if (prim.IsA<pxr::UsdLuxDistantLight>()  || prim.IsA<pxr::UsdLuxDomeLight>()
             || prim.IsA<pxr::UsdLuxRectLight>()    || prim.IsA<pxr::UsdLuxDiskLight>()
             || prim.IsA<pxr::UsdLuxSphereLight>()  || prim.IsA<pxr::UsdLuxCylinderLight>()
             || prim.IsA<pxr::UsdLuxGeometryLight>()|| prim.IsA<pxr::UsdLuxPortalLight>())
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
  const auto pass3cEnd = Clock::now();
  const auto pass3cMs =
      std::chrono::duration<float, std::milli>(pass3cEnd - pass3cStart).count();
  Logging::Get().Info(log::APP,
      "StageWalker pass3c (serial drain + cameras + lights): "
          + std::to_string(static_cast<int>(pass3cMs)) + "ms");

  // Active-camera selection. Honour the root-layer's `boundCamera`
  // hint if present (Omniverse + DCC convention: the camera the
  // scene's authoring tool was last looking through). Fall back to
  // first-in-SdfPath-order — preserves the M4 "first camera wins"
  // contract for fixtures that don't author a hint. activeCameraIndex
  // stays -1 only when the scene authored zero cameras.
  if (!cameras.empty())
  {
    int activeIdx = 0;  // first-in-SdfPath-order fallback
    if (!boundCameraHintPath.empty())
    {
      for (std::size_t i = 0; i < cameras.size(); ++i)
      {
        if (cameras[i].name == boundCameraHintPath)
        {
          activeIdx = static_cast<int>(i);
          break;
        }
      }
    }
    stats.activeCameraIndex = activeIdx;
    scene.SetCamera(cameras[static_cast<std::size_t>(activeIdx)].desc);
  }

  const auto meshPassEnd = Clock::now();
  stats.timings.meshLightCameraMs =
      std::chrono::duration<float, std::milli>(meshPassEnd - meshPassStart).count();
  // totalMs covers WalkStage end-to-end. WalkFile recomputes it to
  // also include the pxr::UsdStage::Open above WalkStage.
  stats.timings.totalMs =
      std::chrono::duration<float, std::milli>(meshPassEnd - walkStart).count();

  return result;
}

}  // namespace pyxis::usd_ingest
