// Pyxis Hydra — HdRenderDelegate. Plan §7 / RFC 0006.
//
// The single Pyxis Hydra delegate (RFC 0006 collapsed the pyxis_hydra_omni fork
// onto this source once both shared Kit's nv-usd). It owns a PyxisEngine; mesh /
// camera / light prim Sync impls push to the engine's GpuScene via
// HdPyxisRenderParam (serialized on its SceneMutex against Hydra's parallel prim
// Sync); the render pass drives PyxisEngine::RenderFrame and composites the
// result into the host's bound color AOV.

#include "HdPyxisRenderDelegate.h"

#include "HdPyxisRenderParam.h"
#include "PyxisEngine.h"

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/InstanceDesc.h>
#include <Pyxis/Renderer/Descs/LightDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>
#include <Pyxis/Renderer/Descs/TextureKey.h>
#include <Pyxis/Renderer/GpuScene.h>

#include <Pyxis/Platform/FileSystem/AssetLocator.h>

// §25.O.3 — the SHARED material translator: the delegate runs bound material
// prims through the SAME FromUsdShade as pyxis_usd_ingest's StageWalker.
#include <Pyxis/MaterialTranslation/UsdShadeToOpenPBR.h>
// §25.O.3 — the SHARED stage ingest: the delegate populates the GpuScene by
// running the SAME StageWalker as the standalone, so the World Lobby (geometry,
// face-subsets, materials, lights, analytic geom, instancing) lands BYTE-
// IDENTICALLY across both adapters instead of being re-implemented in Sync.
#include <Pyxis/UsdIngest/StageWalker.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/geomSubset.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sprim.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdLux/blackbody.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdUtils/stageCache.h>

#include <hlsl++.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numbers>
#include <string>
#include <unordered_map>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// ---- Persistent engine across pxr renderer switches -----------------------
// The closed omni.hydra.pxr engine DESTROYS this delegate when the user
// switches the viewport renderer away from Pyxis and CONSTRUCTS A FRESH ONE on
// switch-back. To avoid rebuilding the whole engine (Vulkan device + GpuScene +
// re-uploading every texture/mesh/BLAS) on every switch-back, the PyxisEngine
// lives in a process-static holder that outlives any single delegate instance.
// On switch-back Hydra builds a fresh render index and re-syncs ALL prims as
// AllDirty, so we can't lean on dirty-bits to skip work — instead each prim's
// EmitToScene content-hashes its geometry+transform+material and reuses the
// already-resident mesh/instance when the hash is unchanged (the fast path).
//
// Render settings:
//   pyxis:persistEngine (bool, default true) — toggle the persistence.
//   pyxis:stageToken    (string)             — stamped by the Kit extension so a
//                                               stage change clears the holder.
// Env override PYXIS_OMNI_NO_PERSIST forces persistence OFF (for the
// VRAM-constrained / large-scene case, and for headless).
//
// Threading: Hydra runs prim Sync in parallel; the holder's `prims` map is
// mutated under HdPyxisRenderParam::SceneMutex exactly like the GpuScene
// mutations, so all access in EmitToScene happens inside that lock.
struct PersistentPyxisState {
  std::unique_ptr<pyxis::hydra::PyxisEngine> engine;
  struct MeshEntry {
    pyxis::MeshHandle mesh = pyxis::MeshHandle::Invalid;
    pyxis::InstanceHandle instance = pyxis::InstanceHandle::Invalid;
    uint64_t contentHash = 0;
  };
  // Keyed by prim SdfPath, shared across delegate instances via this static
  // holder — this is what makes switch-back reuse work. A prim emits a VECTOR of
  // entries: one per face-bound UsdGeomSubset (each subset is its own mesh +
  // instance with the subset's material, mirroring StageWalker §15), plus one for
  // the unassigned faces. A plain (subset-free) mesh has exactly one entry.
  std::unordered_map<SdfPath, std::vector<MeshEntry>, SdfPath::Hash> prims;
  // Lights keyed by SdfPath so a persisted-engine switch-back UPDATEs the
  // existing light instead of AddLight-ing a duplicate (which would make the
  // scene progressively over-lit on each switch).
  std::unordered_map<SdfPath, pyxis::LightHandle, SdfPath::Hash> lights;
  std::string stageToken;  // last seen stage identity (step 5 / pyxis:stageToken)
};

PersistentPyxisState& PersistentState() {
  static PersistentPyxisState state;
  return state;
}

// FNV-1a over a byte range — a local content-hash helper (no public API added).
// Seedable so multiple ranges fold into one digest.
[[nodiscard]] uint64_t HashBytes(const void* data, size_t size, uint64_t seed) noexcept {
  uint64_t hash = seed;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 0x00000100000001B3ull;  // FNV-1a 64-bit prime
  }
  return hash;
}

// USD row-major double matrix -> Pyxis column-vector row-major float4x4 (§10).
// Same transposition both Pyxis adapters use (§25.O.3 byte-equal invariant).
hlslpp::float4x4 ToPyxisMatrix(GfMatrix4d const& usd) noexcept {
  return hlslpp::float4x4(
      hlslpp::float4{static_cast<float>(usd[0][0]), static_cast<float>(usd[1][0]),
                     static_cast<float>(usd[2][0]), static_cast<float>(usd[3][0])},
      hlslpp::float4{static_cast<float>(usd[0][1]), static_cast<float>(usd[1][1]),
                     static_cast<float>(usd[2][1]), static_cast<float>(usd[3][1])},
      hlslpp::float4{static_cast<float>(usd[0][2]), static_cast<float>(usd[1][2]),
                     static_cast<float>(usd[2][2]), static_cast<float>(usd[3][2])},
      hlslpp::float4{static_cast<float>(usd[0][3]), static_cast<float>(usd[1][3]),
                     static_cast<float>(usd[2][3]), static_cast<float>(usd[3][3])});
}

[[nodiscard]] pyxis::GpuScene* SceneOf(HdRenderParam* renderParam) noexcept {
  auto* param = static_cast<HdPyxisRenderParam*>(renderParam);
  return param ? param->GetGpuScene() : nullptr;
}

// Mutex serializing GpuScene mutations across Hydra's PARALLEL prim Sync (see
// HdPyxisRenderParam::SceneMutex). nullptr only if renderParam is null.
[[nodiscard]] std::mutex* SceneMutexOf(HdRenderParam* renderParam) noexcept {
  auto* param = static_cast<HdPyxisRenderParam*>(renderParam);
  return param ? &param->SceneMutex() : nullptr;
}

// Whether the engine is persisted process-wide (drives the EmitToScene dedup
// path). false when renderParam is null.
[[nodiscard]] bool PersistOf(HdRenderParam* renderParam) noexcept {
  auto* param = static_cast<HdPyxisRenderParam*>(renderParam);
  return param ? param->PersistEngine() : false;
}

// Stage-to-world correction (metresPerUnit + Z->Y) the host derived from the
// stage metadata. Identity when renderParam is null or the host didn't set it
// — see HdPyxisRenderParam::StageToWorld. Applied to every prim/light/camera
// transform so the delegate's world matches StageWalker's metres + Y-up frame.
[[nodiscard]] hlslpp::float4x4 StageToWorldOf(HdRenderParam* renderParam) noexcept {
  auto* param = static_cast<HdPyxisRenderParam*>(renderParam);
  return param ? param->StageToWorld() : hlslpp::float4x4::identity();
}

// The open UsdStage the host supplied (null if none). §25.O.3: the render pass
// ingests it through StageWalker (same scene as the standalone); ResolveMaterial
// (the no-stage fallback path) also uses it for the shared FromUsdShade.
[[nodiscard]] UsdStageRefPtr StageOf(HdRenderParam* renderParam) noexcept {
  auto* param = static_cast<HdPyxisRenderParam*>(renderParam);
  return param ? param->Stage() : UsdStageRefPtr();
}

// Resolve a mesh's bound material to a Pyxis MaterialHandle by reading its
// UsdPreviewSurface network and translating the core params -> OpenPBR. Done in
// the mesh Sync (not a separate registry) so it is independent of prim sync
// order; GpuScene::AcquireMaterial dedups by hash. Returns Invalid (renderer's
// default material) when there is no binding / network.
// Texture-acquire callback handed to FromUsdShade — identical to StageWalker's
// ACQUIRE_TEXTURE (userData is the GpuScene; sRGB for BaseColor/Emission, linear
// otherwise is decided inside AcquireTexture by the role).
pyxis::TextureHandle AcquireTextureForTranslation(std::string_view resolvedPath,
                                                  pyxis::TextureKey::Role role,
                                                  void* userData) {
  pyxis::TextureKey key;
  key.resolvedPath = resolvedPath;
  key.role = role;
  return static_cast<pyxis::GpuScene*>(userData)->AcquireTexture(key);
}

pyxis::MaterialHandle ResolveMaterial(HdSceneDelegate* sceneDelegate, SdfPath const& materialId,
                                      pyxis::GpuScene& scene, HdRenderParam* renderParam) {
  if (materialId.IsEmpty())
    return pyxis::MaterialHandle::Invalid;

  // §25.O.3 PRIMARY PATH — when the host supplied the UsdStage, translate the
  // bound material prim through the SHARED pyxis_material_translation::FromUsdShade
  // (the SAME code pyxis_usd_ingest's StageWalker runs). This reads MDL / MaterialX
  // / UsdPreviewSurface inputs directly off the prim, so the World Lobby's 122 MDL
  // materials get their real diffuse tints — byte-identical to the USD-direct
  // adapter. Falls through to the Hydra-network path below if no stage is set
  // (a generic host that never called SetStage) or the prim is not a material.
  UsdStageRefPtr stage = StageOf(renderParam);
  // Kit's omni.hydra.pxr drives the delegate WITHOUT a PyxisHydraHost, so no one
  // calls SetStage there. Fall back to the process-wide UsdUtilsStageCache, where
  // Kit (and Omniverse's UsdContext) registers the open stage — find the cached
  // stage that actually holds this material prim. Lets the live Kit viewport get
  // the same FromUsdShade translation as the headless host path.
  if (!stage) {
    for (const UsdStageRefPtr& cached : UsdUtilsStageCache::Get().GetAllStages()) {
      if (cached && cached->GetPrimAtPath(materialId)) {
        stage = cached;
        break;
      }
    }
  }
  if (stage) {
    if (const UsdPrim prim = stage->GetPrimAtPath(materialId)) {
      if (const UsdShadeMaterial material{prim}) {
        const pyxis::OpenPBRMaterialDesc desc = pyxis::material_translation::FromUsdShade(
            material, &AcquireTextureForTranslation, &scene, UsdTimeCode::Default());
        if (std::getenv("PYXIS_OMNI_MATDUMP") != nullptr) {
          std::fprintf(stderr,
                       "MATDUMP %s source=%d base=(%.3f %.3f %.3f) metal=%.3f rough=%.3f "
                       "emis=(%.3f %.3f %.3f) emisLum=%.3f baseTex=%d normTex=%d roughTex=%d\n",
                       materialId.GetText(), int(desc.source), float(desc.baseColor.x),
                       float(desc.baseColor.y), float(desc.baseColor.z), desc.metalness,
                       desc.roughness, float(desc.emissionColor.x), float(desc.emissionColor.y),
                       float(desc.emissionColor.z), desc.emissionLuminance,
                       desc.baseColorMap != pyxis::TextureHandle::Invalid,
                       desc.normalMap != pyxis::TextureHandle::Invalid,
                       desc.roughnessMap != pyxis::TextureHandle::Invalid);
        }
        return scene.AcquireMaterial(desc);
      }
    }
  }

  const VtValue resource = sceneDelegate->GetMaterialResource(materialId);
  if (!resource.IsHolding<HdMaterialNetworkMap>())
    return pyxis::MaterialHandle::Invalid;

  static const TfToken DIFFUSE_NAME("diffuseColor");
  static const TfToken METALLIC_NAME("metallic");
  static const TfToken ROUGHNESS_NAME("roughness");
  static const TfToken EMISSIVE_NAME("emissiveColor");
  static const TfToken OPACITY_NAME("opacity");
  static const TfToken NORMAL_NAME("normal");
  static const TfToken USD_PREVIEW_SURFACE("UsdPreviewSurface");
  static const TfToken USD_UV_TEXTURE("UsdUVTexture");
  static const TfToken FILE_NAME("file");

  pyxis::OpenPBRMaterialDesc desc;  // sensible defaults from the POD.
  const auto& netMap = resource.UncheckedGet<HdMaterialNetworkMap>();
  const auto surfaceIt = netMap.map.find(HdMaterialTerminalTokens->surface);
  if (surfaceIt == netMap.map.end())
    return scene.AcquireMaterial(desc);
  const HdMaterialNetwork& net = surfaceIt->second;

  // Find the UsdPreviewSurface terminal node. UsdImaging flattens authored MDL /
  // MaterialX materials into a UsdPreviewSurface network (constants + connected
  // UsdUVTexture nodes), so reading this network covers MDL scenes like the World
  // Lobby — we never see the raw MDL here.
  const HdMaterialNode* surface = nullptr;
  for (const HdMaterialNode& node : net.nodes)
    if (node.identifier == USD_PREVIEW_SURFACE) { surface = &node; break; }
  if (surface == nullptr && !net.nodes.empty())
    surface = &net.nodes.back();  // fallback: terminal is conventionally last.
  if (surface == nullptr)
    return scene.AcquireMaterial(desc);

  // Constant (unconnected) surface inputs.
  for (const auto& param : surface->parameters) {
    const TfToken& name = param.first;
    const VtValue& value = param.second;
    if (name == DIFFUSE_NAME && value.IsHolding<GfVec3f>()) {
      const GfVec3f color = value.UncheckedGet<GfVec3f>();
      desc.baseColor = {color[0], color[1], color[2]};
    } else if (name == METALLIC_NAME && value.IsHolding<float>()) {
      desc.metalness = value.UncheckedGet<float>();
    } else if (name == ROUGHNESS_NAME && value.IsHolding<float>()) {
      desc.roughness = value.UncheckedGet<float>();
    } else if (name == EMISSIVE_NAME && value.IsHolding<GfVec3f>()) {
      const GfVec3f color = value.UncheckedGet<GfVec3f>();
      desc.emissionColor = {color[0], color[1], color[2]};
      if (color[0] + color[1] + color[2] > 0.0f)
        desc.emissionLuminance = 1.0f;
    } else if (name == OPACITY_NAME && value.IsHolding<float>()) {
      desc.opacity = value.UncheckedGet<float>();
    }
  }

  // Connected texture inputs. A relationship feeds the surface's input
  // (outputId == surface, outputName == the input slot) FROM an upstream node
  // (inputId). When that upstream node is a UsdUVTexture, acquire its `file`.
  auto resolveConnectedTexture = [&](const TfToken& inputSlot,
                                     pyxis::TextureKey::Role role) -> pyxis::TextureHandle {
    for (const HdMaterialRelationship& rel : net.relationships) {
      if (rel.outputId != surface->path || rel.outputName != inputSlot)
        continue;
      for (const HdMaterialNode& node : net.nodes) {
        if (node.path != rel.inputId || node.identifier != USD_UV_TEXTURE)
          continue;
        const auto fileIt = node.parameters.find(FILE_NAME);
        if (fileIt == node.parameters.end() || !fileIt->second.IsHolding<SdfAssetPath>())
          return pyxis::TextureHandle::Invalid;
        const SdfAssetPath& asset = fileIt->second.UncheckedGet<SdfAssetPath>();
        std::string path = asset.GetResolvedPath();
        if (path.empty())
          path = asset.GetAssetPath();
        if (path.empty())
          return pyxis::TextureHandle::Invalid;
        pyxis::TextureKey key;
        key.resolvedPath = path;
        key.role = role;
        return scene.AcquireTexture(key);
      }
    }
    return pyxis::TextureHandle::Invalid;
  };

  desc.baseColorMap = resolveConnectedTexture(DIFFUSE_NAME, pyxis::TextureKey::Role::BaseColor);
  if (desc.baseColorMap != pyxis::TextureHandle::Invalid)
    desc.baseColor = {1.0f, 1.0f, 1.0f};  // texture supplies the albedo.
  desc.normalMap = resolveConnectedTexture(NORMAL_NAME, pyxis::TextureKey::Role::NormalMap);
  desc.roughnessMap =
      resolveConnectedTexture(ROUGHNESS_NAME, pyxis::TextureKey::Role::RoughnessMetallic);
  desc.metallicMap =
      resolveConnectedTexture(METALLIC_NAME, pyxis::TextureKey::Role::RoughnessMetallic);

  // §25.O.3 parity diagnostic: dump what the delegate translated per material so
  // it can be diffed against StageWalker's MATDUMP. Gated off by default.
  if (std::getenv("PYXIS_OMNI_MATDUMP") != nullptr) {
    std::fprintf(stderr,
                 "MATDUMP %s surface='%s' nNodes=%zu base=(%.3f %.3f %.3f) "
                 "metal=%.3f rough=%.3f emis=(%.3f %.3f %.3f) emisLum=%.3f "
                 "baseTex=%d normTex=%d roughTex=%d\n",
                 materialId.GetText(), surface ? surface->identifier.GetText() : "<none>",
                 net.nodes.size(), float(desc.baseColor.x), float(desc.baseColor.y),
                 float(desc.baseColor.z), desc.metalness, desc.roughness,
                 float(desc.emissionColor.x), float(desc.emissionColor.y),
                 float(desc.emissionColor.z), desc.emissionLuminance,
                 desc.baseColorMap != pyxis::TextureHandle::Invalid,
                 desc.normalMap != pyxis::TextureHandle::Invalid,
                 desc.roughnessMap != pyxis::TextureHandle::Invalid);
  }

  return scene.AcquireMaterial(desc);
}

// ---- Mesh: Hydra mesh -> GpuScene::CreateMesh + AppendInstance ------------
class HdPyxisMesh final : public HdMesh {
 public:
  explicit HdPyxisMesh(SdfPath const& primId) : HdMesh(primId) {}

  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam, HdDirtyBits* dirtyBits,
            TfToken const& /*reprToken*/) override {
    pyxis::GpuScene* scene = SceneOf(renderParam);
    if (scene == nullptr || sceneDelegate == nullptr) {
      *dirtyBits = HdChangeTracker::Clean;
      return;
    }
    // §25.O.3 — when the host supplied the stage, the render pass populates the
    // GpuScene in ONE StageWalker pass (byte-identical to the standalone), so this
    // per-prim Hydra translation must NOT also emit (it would double-populate and
    // drift). No-op to clean. The Sync-based path stays the fallback for a generic
    // Hydra host that never set a stage (e.g. usdview without our SetStage hook).
    if (StageOf(renderParam) == nullptr &&
        (HdChangeTracker::IsTopologyDirty(*dirtyBits, GetId()) ||
         HdChangeTracker::IsPrimvarDirty(*dirtyBits, GetId(), HdTokens->points))) {
      // Hydra runs prim Sync in parallel; serialize the GpuScene mutations
      // (AcquireMaterial / CreateMesh / AppendInstance inside EmitToScene).
      std::unique_lock<std::mutex> lock;
      if (std::mutex* sceneMutex = SceneMutexOf(renderParam))
        lock = std::unique_lock<std::mutex>(*sceneMutex);
      EmitToScene(sceneDelegate, *scene, renderParam);
    }
    *dirtyBits = HdChangeTracker::Clean;
  }

  [[nodiscard]] HdDirtyBits GetInitialDirtyBitsMask() const override {
    return HdChangeTracker::AllSceneDirtyBits;
  }

 protected:
  HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
  void _InitRepr(TfToken const& /*reprToken*/, HdDirtyBits* /*dirtyBits*/) override {}

 private:
  void EmitToScene(HdSceneDelegate* sceneDelegate, pyxis::GpuScene& scene,
                   HdRenderParam* renderParam) {
    const SdfPath& primId = GetId();
    const bool persist = PersistOf(renderParam);

    // Stage-change reset (step 5). Read pyxis:stageToken from the delegate's
    // render settings (stamped by the Kit extension). If it names a DIFFERENT
    // stage than the one currently resident in the persisted engine, drop all
    // ghost prims from the previous stage before this stage's prims populate
    // fresh. Best-effort: if the token never arrives (stays empty) we degrade to
    // "no stage reset" — same-stage reuse still works; only switching scenes
    // while persisting would then need a manual toggle. Runs under the scene
    // mutex (the caller already holds it).
    if (persist) {
      MaybeResetForStageChange(sceneDelegate, renderParam);
    }

    const HdMeshTopology topology = sceneDelegate->GetMeshTopology(primId);
    const VtIntArray& faceCounts = topology.GetFaceVertexCounts();
    const VtIntArray& faceIndices = topology.GetFaceVertexIndices();

    const VtValue pointsVal = sceneDelegate->Get(primId, HdTokens->points);
    if (!pointsVal.IsHolding<VtVec3fArray>())
      return;
    const VtVec3fArray& pointsArray = pointsVal.UncheckedGet<VtVec3fArray>();
    if (pointsArray.empty())
      return;
    const uint32_t vertexCount = static_cast<uint32_t>(pointsArray.size());

    // UV primvar (`st`). Interpolation is inferred by array size: per-vertex (one
    // per point) or faceVarying (one per face-vertex). Without UVs the bound
    // textures (marble / oak / metal / foliage) sample at a constant coordinate
    // and every surface reads flat. The varname could differ from "st" (the
    // UsdPrimvarReader authors it), but "st" is the overwhelming convention.
    static const TfToken ST_TOKEN("st");
    VtVec2fArray stArray;
    if (const VtValue stVal = sceneDelegate->Get(primId, ST_TOKEN); stVal.IsHolding<VtVec2fArray>())
      stArray = stVal.UncheckedGet<VtVec2fArray>();
    size_t faceVertexTotal = 0;
    for (const int faceCount : faceCounts)
      if (faceCount > 0)
        faceVertexTotal += static_cast<size_t>(faceCount);
    const bool stPerVertex = stArray.size() == pointsArray.size() && !stArray.empty();
    const bool stFaceVarying = stArray.size() == faceVertexTotal && faceVertexTotal > 0;
    const bool hasUv = stPerVertex || stFaceVarying;

    // Fan-triangulate, UN-INDEXED (flat per-triangle-vertex arrays) so faceVarying
    // UVs are correct. BOUNDS-SAFE: real scenes (World Lobby) have faces whose
    // indices reference points beyond the points array or whose face-vertex sum
    // disagrees with faceVertexIndices.size(); feeding an out-of-range index into
    // GpuScene reads positions[idx] out of bounds during the BLAS build and
    // corrupts the heap. Guard both bounds and drop any offending triangle.
    // Normals are intentionally left empty — the renderer generates per-face
    // normals on upload (MeshDesc contract).
    const size_t indexCount = faceIndices.size();
    const size_t faceTotal = faceCounts.size();
    // Per-face start offset into faceVertexIndices (faces have variable vertex
    // counts) so a subset can triangulate just its own faces.
    std::vector<size_t> faceStart(faceTotal);
    {
      size_t running = 0;
      for (size_t face = 0; face < faceTotal; ++face) {
        faceStart[face] = running;
        running += faceCounts[face] > 0 ? static_cast<size_t>(faceCounts[face]) : 0;
      }
    }

    // Fan-triangulate the given faces, UN-INDEXED (flat per-triangle-vertex
    // arrays so faceVarying UVs are correct). BOUNDS-SAFE: real scenes have faces
    // referencing points beyond the array; feeding an out-of-range index into
    // GpuScene corrupts the heap during BLAS build — guard + drop offenders.
    auto triangulate = [&](const VtIntArray& faces, std::vector<hlslpp::float3>& positions,
                           std::vector<hlslpp::float2>& uvs, std::vector<uint32_t>& indices) {
      for (const int faceIdx : faces) {
        if (faceIdx < 0 || static_cast<size_t>(faceIdx) >= faceTotal)
          continue;
        const int faceCount = faceCounts[faceIdx];
        const size_t cursor = faceStart[faceIdx];
        if (faceCount < 3 || cursor + static_cast<size_t>(faceCount) > indexCount)
          continue;
        for (int vert = 1; vert + 1 < faceCount; ++vert) {
          const std::array<int, 3> corners = {0, vert, vert + 1};
          std::array<hlslpp::float3, 3> triPos;
          std::array<hlslpp::float2, 3> triUv;
          bool valid = true;
          for (int slot = 0; slot < 3; ++slot) {
            const size_t faceVertex = cursor + static_cast<size_t>(corners[slot]);
            const uint32_t pointIdx = static_cast<uint32_t>(faceIndices[faceVertex]);
            if (pointIdx >= vertexCount) {
              valid = false;
              break;
            }
            const GfVec3f& pos = pointsArray[pointIdx];
            triPos[slot] = {pos[0], pos[1], pos[2]};
            if (stPerVertex && pointIdx < stArray.size()) {
              const GfVec2f& texCoord = stArray[pointIdx];
              triUv[slot] = {texCoord[0], texCoord[1]};
            } else if (stFaceVarying && faceVertex < stArray.size()) {
              const GfVec2f& texCoord = stArray[faceVertex];
              triUv[slot] = {texCoord[0], texCoord[1]};
            } else {
              triUv[slot] = {0.0f, 0.0f};
            }
          }
          if (!valid)
            continue;
          for (int slot = 0; slot < 3; ++slot) {
            indices.push_back(static_cast<uint32_t>(positions.size()));
            positions.push_back(triPos[slot]);
            uvs.push_back(triUv[slot]);
          }
        }
      }
    };

    // Decompose into submeshes: one per face-bound UsdGeomSubset (bound to the
    // subset's material), plus one for faces NOT covered by any subset (prim-level
    // material). A subset-free mesh is a single submesh over all faces. This
    // mirrors pyxis_usd_ingest's StageWalker §15 so the two adapters emit the SAME
    // mesh/instance set + per-face materials (§25.O.3 parity).
    struct SubMesh {
      VtIntArray faces;
      SdfPath materialId;
      bool tagged;  // false = whole-mesh / unassigned (debugName = primId)
    };
    std::vector<SubMesh> subMeshes;
    const SdfPath primMaterialId = sceneDelegate->GetMaterialId(primId);
    const HdGeomSubsets& geomSubsets = topology.GetGeomSubsets();
    if (geomSubsets.empty()) {
      VtIntArray allFaces(faceTotal);
      for (size_t face = 0; face < faceTotal; ++face)
        allFaces[face] = static_cast<int>(face);
      subMeshes.push_back({std::move(allFaces), primMaterialId, false});
    } else {
      std::vector<bool> covered(faceTotal, false);
      for (const HdGeomSubset& subset : geomSubsets) {
        if (subset.type != HdGeomSubset::TypeFaceSet)
          continue;
        for (const int face : subset.indices)
          if (face >= 0 && static_cast<size_t>(face) < faceTotal)
            covered[static_cast<size_t>(face)] = true;
        subMeshes.push_back(
            {subset.indices, subset.materialId.IsEmpty() ? primMaterialId : subset.materialId, true});
      }
      VtIntArray unassigned;
      for (size_t face = 0; face < faceTotal; ++face)
        if (!covered[face])
          unassigned.push_back(static_cast<int>(face));
      if (!unassigned.empty())
        subMeshes.push_back({std::move(unassigned), primMaterialId, false});
    }

    // GetTransform returns the prim's transform in the stage's authored units
    // and up-axis. Left-multiply the host-supplied stage-to-world correction
    // (metresPerUnit + Z->Y) so geometry lands in the renderer's metres + Y-up
    // frame — same bake StageWalker::ComposeWorldFromLocal applies (§25.O.3).
    // Shared by every submesh of this prim.
    const GfMatrix4d worldFromLocal = sceneDelegate->GetTransform(primId);
    const hlslpp::float4x4 pyxisXform =
        mul(StageToWorldOf(renderParam), ToPyxisMatrix(worldFromLocal));

    // Persist path keeps a VECTOR of entries (one per submesh) keyed by primId.
    // On re-sync (switch-back), a changed submesh count drops the stale set so the
    // per-index mapping below stays consistent.
    std::vector<PersistentPyxisState::MeshEntry>* primEntries = nullptr;
    if (persist) {
      primEntries = &PersistentState().prims[primId];
      if (primEntries->size() != subMeshes.size()) {
        for (auto& staleEntry : *primEntries)
          if (staleEntry.mesh != pyxis::MeshHandle::Invalid)
            scene.DestroyMesh(staleEntry.mesh);
        primEntries->assign(subMeshes.size(), PersistentPyxisState::MeshEntry{});
      }
    }

    // Emit each submesh as its own mesh + instance (StageWalker §15 parity).
    for (size_t sub = 0; sub < subMeshes.size(); ++sub) {
      std::vector<hlslpp::float3> positions;
      std::vector<hlslpp::float2> uvs;
      std::vector<uint32_t> indices;
      triangulate(subMeshes[sub].faces, positions, uvs, indices);
      if (indices.empty())
        continue;
      const std::string debugName =
          subMeshes[sub].tagged ? (primId.GetString() + "/subset" + std::to_string(sub))
                                 : primId.GetString();
      pyxis::MeshDesc meshDesc;
      meshDesc.positions = positions;
      meshDesc.indices = indices;
      if (hasUv)
        meshDesc.uv0 = uvs;
      meshDesc.debugName = debugName;
      const pyxis::MaterialHandle material =
          ResolveMaterial(sceneDelegate, subMeshes[sub].materialId, scene, renderParam);

      // Non-persist: always CreateMesh + AppendInstance, discard handles (the owned
      // engine tears down on delegate destruction — nothing to dedup against).
      if (!persist) {
        const auto meshHandle = scene.CreateMesh(meshDesc);
        if (!meshHandle.has_value())
          continue;
        pyxis::InstanceDesc instanceDesc;
        instanceDesc.mesh = *meshHandle;
        instanceDesc.material = material;
        instanceDesc.worldFromLocal = pyxisXform;
        instanceDesc.debugName = debugName;
        const auto instanceHandle = scene.AppendInstance(instanceDesc);
        (void)instanceHandle;  // cap exhaustion surfaces via FrameStats.
        continue;
      }

      // Persist: content-hash dedup per submesh index so a switch-back (every prim
      // re-synced AllDirty into a surviving engine) reuses the resident submesh
      // instead of duplicating geometry. Hash folds geometry + UVs + transform +
      // resolved material. Caller holds the scene mutex (serializes vs parallel
      // Hydra Sync).
      uint64_t contentHash = 0xCBF29CE484222325ull;  // FNV-1a 64-bit offset basis
      contentHash = HashBytes(positions.data(), positions.size() * sizeof(positions[0]), contentHash);
      contentHash = HashBytes(indices.data(), indices.size() * sizeof(indices[0]), contentHash);
      if (hasUv && !uvs.empty())
        contentHash = HashBytes(uvs.data(), uvs.size() * sizeof(uvs[0]), contentHash);
      contentHash = HashBytes(&pyxisXform, sizeof(pyxisXform), contentHash);
      const uint32_t materialValue = static_cast<uint32_t>(material);
      contentHash = HashBytes(&materialValue, sizeof(materialValue), contentHash);

      PersistentPyxisState::MeshEntry& entry = (*primEntries)[sub];
      if (entry.mesh == pyxis::MeshHandle::Invalid) {
        const auto meshHandle = scene.CreateMesh(meshDesc);
        if (!meshHandle.has_value())
          continue;
        pyxis::InstanceDesc instanceDesc;
        instanceDesc.mesh = *meshHandle;
        instanceDesc.material = material;
        instanceDesc.worldFromLocal = pyxisXform;
        instanceDesc.debugName = debugName;
        const auto instanceHandle = scene.AppendInstance(instanceDesc);
        if (!instanceHandle.has_value())
          continue;  // mesh stays resident; cap surfaces via FrameStats.
        entry = {*meshHandle, *instanceHandle, contentHash};
        continue;
      }
      if (entry.contentHash == contentHash)
        continue;  // FAST PATH (switch-back): nothing changed for this submesh.

      // Submesh changed — update in place; fall back to recreate if UpdateMesh fails.
      const auto updated = scene.UpdateMesh(entry.mesh, meshDesc);
      if (!updated.has_value()) {
        scene.DestroyMesh(entry.mesh);
        const auto meshHandle = scene.CreateMesh(meshDesc);
        if (!meshHandle.has_value()) {
          entry = {};
          continue;
        }
        pyxis::InstanceDesc instanceDesc;
        instanceDesc.mesh = *meshHandle;
        instanceDesc.material = material;
        instanceDesc.worldFromLocal = pyxisXform;
        instanceDesc.debugName = debugName;
        const auto instanceHandle = scene.AppendInstance(instanceDesc);
        if (!instanceHandle.has_value()) {
          entry = {};
          continue;
        }
        entry = {*meshHandle, *instanceHandle, contentHash};
        continue;
      }
      scene.UpdateInstanceTransform(entry.instance, pyxisXform);
      scene.UpdateInstanceMaterial(entry.instance, material);
      entry.contentHash = contentHash;
    }
  }

  // Read pyxis:stageToken from the delegate render settings; if it differs from
  // the resident stage in the persisted holder, Clear the engine's scene + the
  // prim map so a newly-opened stage doesn't render the previous stage's ghost
  // prims. Caller holds the scene mutex.
  static void MaybeResetForStageChange(HdSceneDelegate* sceneDelegate,
                                       HdRenderParam* renderParam) {
    const HdRenderIndex& index = sceneDelegate->GetRenderIndex();
    const HdRenderDelegate* delegate = index.GetRenderDelegate();
    if (delegate == nullptr)
      return;
    static const TfToken STAGE_TOKEN_NAME("pyxis:stageToken");
    const VtValue tokenVal = delegate->GetRenderSetting(STAGE_TOKEN_NAME);
    std::string newToken;
    if (tokenVal.IsHolding<std::string>())
      newToken = tokenVal.UncheckedGet<std::string>();
    else if (tokenVal.IsHolding<TfToken>())
      newToken = tokenVal.UncheckedGet<TfToken>().GetString();
    if (newToken.empty())
      return;  // token never arrived -> no reset (graceful degrade; see step 5).
    PersistentPyxisState& state = PersistentState();
    if (newToken == state.stageToken)
      return;
    // FIRST time we learn the stage identity (the engine just came up on this
    // stage): record it but DO NOT Clear. The engine is already fresh, and
    // Clearing here would race Hydra's PARALLEL prim Sync and wipe lights/prims
    // already synced THIS pass — they won't re-sync (Hydra marks them clean), so
    // the scene renders UNLIT (looks like flat base-color). Only an actual stage
    // SWITCH (a non-empty token changing to a DIFFERENT one) clears the previous
    // stage's resident prims.
    const bool firstStamp = state.stageToken.empty();
    state.stageToken = newToken;
    if (firstStamp)
      return;
    auto* param = static_cast<HdPyxisRenderParam*>(renderParam);
    pyxis::hydra::PyxisEngine* engine = param ? param->GetEngine() : nullptr;
    if (engine != nullptr && engine->Scene() != nullptr) {
      // GpuScene::Clear drops the TLAS + every buffer; the contract requires the
      // device be idle first or in-flight work (the previous stage's last frame)
      // referencing those resources crashes.
      engine->WaitIdle();
      engine->Scene()->Clear();
    }
    state.prims.clear();
    state.lights.clear();
  }
};

// ---- Camera: Hydra camera -> GpuScene::SetCamera --------------------------
class HdPyxisCamera final : public HdCamera {
 public:
  explicit HdPyxisCamera(SdfPath const& primId) : HdCamera(primId) {}

  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
            HdDirtyBits* dirtyBits) override {
    HdCamera::Sync(sceneDelegate, renderParam, dirtyBits);
    pyxis::GpuScene* scene = SceneOf(renderParam);
    // §25.O.3 — the render pass selects the active camera (USD GfCamera or the
    // free/navigation camera) each frame; skip the per-prim SetCamera when a stage
    // is present so there's a single, consistent camera source.
    if (scene != nullptr && sceneDelegate != nullptr && StageOf(renderParam) == nullptr) {
      const SdfPath& primId = GetId();
      // Bake the same stage-to-world correction onto the camera so it lives in
      // the renderer's metres + Y-up frame alongside the (corrected) geometry.
      // worldFromCamera_corrected = stageToWorld * worldFromCamera_stage, hence
      // viewFromWorld = inverse(worldFromCamera_corrected). Keeping camera and
      // geometry in one frame leaves framing unchanged; only the absolute world
      // scale (which the closesthit's metre constants depend on) is fixed.
      const hlslpp::float4x4 worldFromCamera =
          mul(StageToWorldOf(renderParam), ToPyxisMatrix(sceneDelegate->GetTransform(primId)));
      pyxis::CameraDesc cameraDesc;
      cameraDesc.viewFromWorld = hlslpp::inverse(worldFromCamera);
      cameraDesc.projFromView = ToPyxisMatrix(ComputeProjectionMatrix());
      const GfRange1f clipRange = GetClippingRange();
      cameraDesc.nearClip = clipRange.GetMin();
      cameraDesc.farClip = clipRange.GetMax();
      // UsdGeomCamera::exposure in stops (see the render-pass camera above for
      // the rationale) — StageWalker reads the same attribute, so consuming it
      // here keeps the two adapters' tonemap byte-comparable (§25.O.3).
      cameraDesc.exposure = GetExposure();
      std::unique_lock<std::mutex> lock;  // serialize vs parallel prim Sync
      if (std::mutex* sceneMutex = SceneMutexOf(renderParam))
        lock = std::unique_lock<std::mutex>(*sceneMutex);
      scene->SetCamera(cameraDesc);
    }
    *dirtyBits = HdChangeTracker::Clean;
  }
};

// ---- Functional stubs: material / light / render buffer -------------------
class StubMaterial final : public HdMaterial {
 public:
  explicit StubMaterial(SdfPath const& primId) : HdMaterial(primId) {}
  void Sync(HdSceneDelegate*, HdRenderParam*, HdDirtyBits* dirtyBits) override {
    *dirtyBits = HdChangeTracker::Clean;
  }
  [[nodiscard]] HdDirtyBits GetInitialDirtyBitsMask() const override { return HdMaterial::AllDirty; }
};

// Light: Hydra light -> GpuScene::AddLight (distant / dome / rect). Color +
// intensity from light params; direction/position from the transform.
class HdPyxisLight final : public HdLight {
 public:
  // StageWalker folds three USD area-light shapes (Rect / Disk / Sphere) into
  // LightDesc::Kind::Rect but computes a different `areaForNormalize` per shape
  // (w·h / π·r² / 4π·r²). The originating Hydra Sprim type is lost once we map
  // to LightDesc::Kind, so we stash it here at CreateSprim time to replay the
  // exact per-shape area math in Sync. NotRect for the non-Rect kinds.
  enum class RectShape : uint8_t { NotRect, Rect, Disk, Sphere };

  HdPyxisLight(SdfPath const& primId, pyxis::LightDesc::Kind kind,
               RectShape rectShape = RectShape::NotRect)
      : HdLight(primId), _kind(kind), _rectShape(rectShape) {}

  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
            HdDirtyBits* dirtyBits) override {
    pyxis::GpuScene* scene = SceneOf(renderParam);
    // §25.O.3 — when a stage is present the render pass emits all lights via
    // StageWalker (byte-identical to the standalone); skip the per-prim path.
    if (scene != nullptr && sceneDelegate != nullptr && StageOf(renderParam) == nullptr) {
      const SdfPath& primId = GetId();

      // Read light params the Hydra way: GetLightParamValue keyed on
      // HdLightTokens (or a TfToken("inputs:<name>") fallback for params
      // Hydra doesn't enumerate). These mirror StageWalker::EmitLight's
      // readFloat / readColor / readBool / readToken helpers, but sourced
      // from the scene delegate instead of raw USD attrs — the values are
      // identical for the World Lobby light set (§25.O.3 parity).
      auto readFloat = [&](const TfToken& token, float fallback) -> float {
        const VtValue value = sceneDelegate->GetLightParamValue(primId, token);
        if (value.IsHolding<float>())
          return value.UncheckedGet<float>();
        if (value.IsHolding<double>())
          return static_cast<float>(value.UncheckedGet<double>());
        return fallback;
      };
      auto readColor = [&](const TfToken& token, hlslpp::float3 fallback) -> hlslpp::float3 {
        const VtValue value = sceneDelegate->GetLightParamValue(primId, token);
        if (value.IsHolding<GfVec3f>()) {
          const GfVec3f rgb = value.UncheckedGet<GfVec3f>();
          return hlslpp::float3{rgb[0], rgb[1], rgb[2]};
        }
        return fallback;
      };
      auto readBool = [&](const TfToken& token, bool fallback) -> bool {
        const VtValue value = sceneDelegate->GetLightParamValue(primId, token);
        if (value.IsHolding<bool>())
          return value.UncheckedGet<bool>();
        return fallback;
      };
      auto readToken = [&](const TfToken& token, const TfToken& fallback) -> TfToken {
        const VtValue value = sceneDelegate->GetLightParamValue(primId, token);
        if (value.IsHolding<TfToken>())
          return value.UncheckedGet<TfToken>();
        return fallback;
      };

      pyxis::LightDesc desc;
      // The Sprim type captured at CreateSprim time IS the kind StageWalker
      // derives from prim.IsA<UsdLux*>() (distant / dome / rect|disk|sphere ->
      // Rect / cylinder). Assign it up front so the closesthit dispatches the
      // light correctly — without this every light defaults to Kind::Distant,
      // which drops the dome's env-map fill (interior goes ~4x too dark) and
      // mis-shades the area fixtures. §25.O.3 parity with StageWalker.
      desc.kind = _kind;

      // ---- Color (+ colorTemperature collapse) — StageWalker parity -------
      desc.color = readColor(HdLightTokens->color, hlslpp::float3{1.0f, 1.0f, 1.0f});
      const bool  enableColorTemp  = readBool(HdLightTokens->enableColorTemperature, false);
      const float colorTemperature = readFloat(HdLightTokens->colorTemperature, 6500.0f);
      if (enableColorTemp) {
        const GfVec3f blackbodyRgb = UsdLuxBlackbodyTemperatureAsRgb(colorTemperature);
        desc.color = hlslpp::float3{desc.color.x * blackbodyRgb[0],
                                    desc.color.y * blackbodyRgb[1],
                                    desc.color.z * blackbodyRgb[2]};
      }

      // ---- intensity * 2^exposure -----------------------------------------
      const float rawIntensity  = readFloat(HdLightTokens->intensity, 1.0f);
      const float exposureStops = readFloat(HdLightTokens->exposure, 0.0f);
      desc.intensity = rawIntensity * std::exp2(exposureStops);

      // ---- M8a modifiers stashed on LightDesc -----------------------------
      desc.diffuse   = readFloat(HdLightTokens->diffuse, 1.0f);
      desc.specular  = readFloat(HdLightTokens->specular, 1.0f);
      desc.normalize = readBool(HdLightTokens->normalize, false);
      desc.shapingConeAngle    = readFloat(HdLightTokens->shapingConeAngle, 90.0f);
      desc.shapingConeSoftness = readFloat(HdLightTokens->shapingConeSoftness, 0.0f);
      desc.shapingFocus        = readFloat(HdLightTokens->shapingFocus, 0.0f);
      desc.shapingFocusTint    = readColor(HdLightTokens->shapingFocusTint,
                                           hlslpp::float3{1.0f, 1.0f, 1.0f});

      // ---- Transform: SAME path as the meshes (GetTransform -> ToPyxisMatrix)
      // so lights land in the geometry's world space. StageWalker composes
      // stageToWorld * localToWorld; in the delegate Hydra has already baked
      // the stage->world correction into GetTransform, so this single matrix
      // is the equivalent worldFromLocal. We extract direction / position /
      // axes with Pyxis column-vector math (mul(M, v)), NOT GfMatrix helpers,
      // and we DO NOT normalize direction (PackLightGpu normalises) — byte
      // parity with StageWalker.
      const hlslpp::float4x4 worldFromLocal =
          mul(StageToWorldOf(renderParam), ToPyxisMatrix(sceneDelegate->GetTransform(primId)));

      constexpr float PI_F = std::numbers::pi_v<float>;
      auto worldLength3 = [](const hlslpp::float3& vec) noexcept {
        return std::sqrt(static_cast<float>(vec.x) * static_cast<float>(vec.x)
                         + static_cast<float>(vec.y) * static_cast<float>(vec.y)
                         + static_cast<float>(vec.z) * static_cast<float>(vec.z));
      };
      float areaForNormalize = 1.0f;

      std::unique_lock<std::mutex> lock;  // serialize vs parallel prim Sync
      if (std::mutex* sceneMutex = SceneMutexOf(renderParam))
        lock = std::unique_lock<std::mutex>(*sceneMutex);

      // ---- Per-kind dispatch — mirrors StageWalker::EmitLight -------------
      // _kind was decided at CreateSprim time from the Hydra Sprim type token
      // (distant / dome / rect / sphere / disk -> Rect / cylinder), exactly
      // the mapping StageWalker derives from prim.IsA<UsdLux*>().
      if (_kind == pyxis::LightDesc::Kind::Distant) {
        const hlslpp::float4 dirWorld =
            mul(worldFromLocal, hlslpp::float4{0.0f, 0.0f, -1.0f, 0.0f});
        desc.direction    = hlslpp::float3{dirWorld.x, dirWorld.y, dirWorld.z};
        desc.distantAngle = readFloat(HdLightTokens->angle, 0.53f);
        areaForNormalize  = 1.0f;
      } else if (_kind == pyxis::LightDesc::Kind::Rect) {
        // Rect / Disk / Sphere all fold into Kind::Rect (StageWalker). The
        // halfU/halfV source differs by the originating Sprim type, captured
        // in _rectShape at CreateSprim time.
        const hlslpp::float4 originWorld =
            mul(worldFromLocal, hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f});
        desc.position = hlslpp::float3{originWorld.x, originWorld.y, originWorld.z};
        float halfU = 1.0f;
        float halfV = 1.0f;
        if (_rectShape == RectShape::Rect) {
          halfU = readFloat(HdLightTokens->width, 2.0f) * 0.5f;
          halfV = readFloat(HdLightTokens->height, 2.0f) * 0.5f;
        } else {  // Disk or Sphere — both author `radius`
          const float radius = readFloat(HdLightTokens->radius, 0.5f);
          halfU = radius;
          halfV = radius;
        }
        const hlslpp::float4 uWorld =
            mul(worldFromLocal, hlslpp::float4{halfU, 0.0f, 0.0f, 0.0f});
        desc.axisU = hlslpp::float3{uWorld.x, uWorld.y, uWorld.z};
        const hlslpp::float4 vWorld =
            mul(worldFromLocal, hlslpp::float4{0.0f, halfV, 0.0f, 0.0f});
        desc.axisV = hlslpp::float3{vWorld.x, vWorld.y, vWorld.z};
        const float worldHalfU = worldLength3(desc.axisU);
        const float worldHalfV = worldLength3(desc.axisV);
        if (_rectShape == RectShape::Rect)
          areaForNormalize = 4.0f * worldHalfU * worldHalfV;
        else if (_rectShape == RectShape::Disk)
          areaForNormalize = PI_F * worldHalfU * worldHalfV;
        else  // Sphere
          areaForNormalize = 4.0f * PI_F * worldHalfU * worldHalfU;
      } else if (_kind == pyxis::LightDesc::Kind::Cylinder) {
        const float length = readFloat(HdLightTokens->length, 1.0f);
        const float radius = readFloat(HdLightTokens->radius, 0.5f);
        const hlslpp::float4 originWorld =
            mul(worldFromLocal, hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f});
        desc.position = hlslpp::float3{originWorld.x, originWorld.y, originWorld.z};
        const hlslpp::float4 uWorld =
            mul(worldFromLocal, hlslpp::float4{length * 0.5f, 0.0f, 0.0f, 0.0f});
        const hlslpp::float4 vWorld =
            mul(worldFromLocal, hlslpp::float4{0.0f, radius, 0.0f, 0.0f});
        desc.axisU = hlslpp::float3{uWorld.x, uWorld.y, uWorld.z};
        desc.axisV = hlslpp::float3{vWorld.x, vWorld.y, vWorld.z};
        const float worldHalfLen = worldLength3(desc.axisU);
        const float worldRadius  = worldLength3(desc.axisV);
        areaForNormalize = 2.0f * PI_F * worldRadius * (2.0f * worldHalfLen);
      } else if (_kind == pyxis::LightDesc::Kind::Dome) {
        // Dome texture format — latlong (default) / mirroredBall / angular.
        static const TfToken mirroredBallTok("mirroredBall");  // NOLINT(readability-identifier-naming)
        static const TfToken angularTok("angular");            // NOLINT(readability-identifier-naming)
        static const TfToken automaticTok("automatic");        // NOLINT(readability-identifier-naming)
        const TfToken format = readToken(HdLightTokens->textureFormat, automaticTok);
        if (format == mirroredBallTok)
          desc.domeFormat = pyxis::LightDesc::DomeFormat::MirroredBall;
        else if (format == angularTok)
          desc.domeFormat = pyxis::LightDesc::DomeFormat::Angular;
        else
          desc.domeFormat = pyxis::LightDesc::DomeFormat::LatLong;

        // Resolve texture:file -> bindless TextureHandle, mirroring
        // StageWalker (incl. the bundled default_sky.exr fallback so a
        // textureless dome still has a lat-long map to sample).
        const VtValue texVal =
            sceneDelegate->GetLightParamValue(primId, HdLightTokens->textureFile);
        std::string resolvedPath;
        if (texVal.IsHolding<SdfAssetPath>()) {
          const SdfAssetPath& asset = texVal.UncheckedGet<SdfAssetPath>();
          resolvedPath = asset.GetResolvedPath();
          if (resolvedPath.empty())
            resolvedPath = asset.GetAssetPath();
        }
        if (resolvedPath.empty()) {
          const pyxis::Path bundled =
              pyxis::AssetLocator{}.LocateResource("scenes/default_sky.exr");
          if (!bundled.View().empty())
            resolvedPath.assign(bundled.View());
        }
        if (!resolvedPath.empty()) {
          pyxis::TextureKey key;
          key.resolvedPath = resolvedPath;
          key.role = pyxis::TextureKey::Role::Emission;  // HDR env-map: linear
          desc.envMap = scene->AcquireTexture(key);
        }
        // domeRotationY: StageWalker reads the raw xformOp:rotateY off the
        // prim because its stageToWorld correction would otherwise pollute
        // the angle. Hydra bakes all xformOps into GetTransform and exposes
        // no separate rotateY param, so we cannot recover the isolated
        // horizontal spin here without re-decomposing the matrix. Left at 0
        // (the LatLong miss-shader default). FLAGGED for live verification —
        // see report.
        areaForNormalize = 1.0f;
      }

      // ---- M8a area-to-power conversion (StageWalker parity) --------------
      if (!desc.normalize && areaForNormalize > 1e-6f)
        desc.intensity *= areaForNormalize;

      // §25.O.3 parity diagnostic — diff against StageWalker's LIGHTDUMP[usd].
      if (std::getenv("PYXIS_OMNI_LIGHTDUMP") != nullptr) {
        std::fprintf(stderr,
                     "LIGHTDUMP[hyd] %s kind=%d color=%.4f,%.4f,%.4f intensity=%.4f "
                     "pos=%.3f,%.3f,%.3f dir=%.3f,%.3f,%.3f area=%.4f norm=%d envMap=%d\n",
                     primId.GetText(), int(desc.kind), float(desc.color.x), float(desc.color.y),
                     float(desc.color.z), desc.intensity, float(desc.position.x),
                     float(desc.position.y), float(desc.position.z), float(desc.direction.x),
                     float(desc.direction.y), float(desc.direction.z), areaForNormalize,
                     desc.normalize ? 1 : 0, desc.envMap != pyxis::TextureHandle::Invalid);
      }

      // With a persisted engine, a renderer switch-back re-syncs every light
      // AllDirty into the SAME (surviving) GpuScene. Dedup by SdfPath so we
      // UPDATE the resident light instead of AddLight-ing a duplicate (which
      // would make the scene progressively over-lit per switch). Non-persist:
      // always AddLight (the owned engine tears down, nothing to dedup against).
      if (PersistOf(renderParam)) {
        auto& lights = PersistentState().lights;
        if (const auto found = lights.find(primId); found != lights.end()) {
          scene->UpdateLight(found->second, desc);
        } else {
          lights.emplace(primId, scene->AddLight(desc));
        }
      } else {
        const auto lightHandle = scene->AddLight(desc);
        (void)lightHandle;
      }
    }
    *dirtyBits = HdChangeTracker::Clean;
  }

  [[nodiscard]] HdDirtyBits GetInitialDirtyBitsMask() const override { return HdLight::AllDirty; }

 private:
  pyxis::LightDesc::Kind _kind;
  RectShape _rectShape;
};

// Minimal CPU-backed render buffer so HdEngine's AOV machinery doesn't crash.
// Pyxis renders into its own exportable image (PyxisEngine), so this buffer is
// the host-facing presentation target the render pass composites into.
class StubRenderBuffer final : public HdRenderBuffer {
 public:
  explicit StubRenderBuffer(SdfPath const& primId) : HdRenderBuffer(primId) {}

  bool Allocate(GfVec3i const& dimensions, HdFormat format, bool /*multiSampled*/) override {
    _width = static_cast<uint32_t>(dimensions[0]);
    _height = static_cast<uint32_t>(dimensions[1]);
    _format = format;
    _data.assign(static_cast<size_t>(_width) * _height * HdDataSizeOfFormat(format), 0);
    if (std::getenv("PYXIS_OMNI_DBG") != nullptr)
      std::fprintf(stderr, "PYXISDBG RenderBuffer::Allocate id='%s' %ux%u fmt=%d\n",
                   GetId().GetText(), _width, _height, static_cast<int>(format));
    return true;
  }
  unsigned int GetWidth() const override { return _width; }
  unsigned int GetHeight() const override { return _height; }
  unsigned int GetDepth() const override { return 1; }
  HdFormat GetFormat() const override { return _format; }
  bool IsMultiSampled() const override { return false; }
  void* Map() override {
    _mapped = true;
    return _data.data();
  }
  void Unmap() override { _mapped = false; }
  bool IsMapped() const override { return _mapped; }
  void Resolve() override {}
  bool IsConverged() const override { return true; }

 protected:
  void _Deallocate() override {
    _data.clear();
    _width = _height = 0;
  }

 private:
  uint32_t _width = 0;
  uint32_t _height = 0;
  HdFormat _format = HdFormatUNorm8Vec4;
  bool _mapped = false;
  std::vector<uint8_t> _data;
};

float HalfToFloat(uint16_t half) {
  const uint32_t sign = (half >> 15) & 0x1u, exp = (half >> 10) & 0x1Fu, mant = half & 0x3FFu;
  uint32_t bits;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign << 31;
    } else {
      int shift = -1;
      uint32_t norm = mant;
      do {
        ++shift;
        norm <<= 1;
      } while ((norm & 0x400u) == 0);
      bits = (sign << 31) | (static_cast<uint32_t>(127 - 15 - shift) << 23) | ((norm & 0x3FFu) << 13);
    }
  } else if (exp == 0x1F) {
    bits = (sign << 31) | (0xFFu << 23) | (mant << 13);
  } else {
    bits = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
  }
  float out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

// Standard sRGB OETF (linear -> sRGB display encoding).
float LinearToSrgb(float linear) {
  linear = linear < 0.0f ? 0.0f : (linear > 1.0f ? 1.0f : linear);
  return linear <= 0.0031308f ? linear * 12.92f
                              : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

// Minimal float -> IEEE half (round-to-nearest-even not required for display).
uint16_t FloatToHalf(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 16) & 0x8000u;
  const int32_t expo = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
  const uint32_t mant = bits & 0x7FFFFFu;
  if (expo <= 0)
    return static_cast<uint16_t>(sign);  // flush subnormals/underflow to ±0
  if (expo >= 0x1F)
    return static_cast<uint16_t>(sign | (0x1Fu << 10));  // overflow -> inf
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(expo) << 10) | (mant >> 13));
}

// Composite Pyxis's rendered color (RGBA16F, from PyxisEngine readback) into the
// host's bound color HdRenderBuffer. This is the host-facing presentation step —
// every display path (custom panel, usdview, UsdImagingGL, omni.hydra.pxr) reads
// this buffer. Supports the common AOV formats; converts from RGBA16F as needed.
//
// §25.O.3 / RTX-match: PyxisEngine exports the ACES-tonemapped color in LINEAR
// display space (raygen writes acesFilmic() with no OETF; the exported texture is
// a storage/UAV target so the GPU applies no sRGB on write). The Kit viewport
// (and usdview's GL present) show this color AOV VERBATIM — they do NOT apply a
// display transform to a delegate's color buffer — so to match RTX (which hands
// the viewport display-ready values) the delegate must sRGB-ENCODE here. The
// standalone pyxis.exe path is unaffected: it reads the LINEAR buffer via
// ReadbackColorHdr and applies sRGB in its own PNG writer / sRGB swapchain. RGB
// is encoded; alpha stays linear. Set PYXIS_OMNI_EXPORT_LINEAR_HDR to skip the
// encode (a host that does its own display transform).
void WritePyxisColorToAov(pyxis::hydra::PyxisEngine& engine, HdRenderBuffer* buffer) {
  // Per-frame trace (composite / render-pass): gated on PYXIS_OMNI_TRACE, NOT
  // PYXIS_OMNI_DBG, so the default-on DBG keeps only occasional lifecycle prints
  // and doesn't flood every frame.
  const bool dbg = std::getenv("PYXIS_OMNI_TRACE") != nullptr;
  if (buffer == nullptr || buffer->GetWidth() == 0 || buffer->GetHeight() == 0) {
    if (dbg)
      std::fprintf(stderr, "PYXISDBG WriteAov: buffer null or 0-size\n");
    return;
  }
  std::vector<uint8_t> src;
  uint32_t srcWidth = 0, srcHeight = 0;
  if (!engine.ReadbackColorHdr(src, srcWidth, srcHeight) || srcWidth == 0 || srcHeight == 0) {
    if (dbg)
      std::fprintf(stderr, "PYXISDBG WriteAov: readback failed (src=%ux%u)\n", srcWidth, srcHeight);
    return;
  }
  if (dbg)
    std::fprintf(stderr, "PYXISDBG WriteAov: src=%ux%u buf=%ux%u fmt=%d\n", srcWidth, srcHeight,
                 buffer->GetWidth(), buffer->GetHeight(), static_cast<int>(buffer->GetFormat()));
  const uint32_t bufWidth = buffer->GetWidth(), bufHeight = buffer->GetHeight();
  const uint32_t copyWidth = std::min(srcWidth, bufWidth);
  const uint32_t copyHeight = std::min(srcHeight, bufHeight);
  const HdFormat fmt = buffer->GetFormat();
  auto* dst = static_cast<uint8_t*>(buffer->Map());
  if (dst == nullptr)
    return;
  // VERTICAL FLIP (viewport only): Pyxis renders top-row-first (Vulkan), but the
  // pxr engine presents this color HdRenderBuffer bottom-row-first (GL / usdview
  // convention), so a straight row copy shows the image upside-down in the
  // viewport. Flip the destination row here. This is the HOST PRESENTATION path
  // only — ReadbackColorHdr (the headless EXR + §25.O.3 byte-identical
  // determinism path) is left untouched, so EXR orientation/determinism is
  // unaffected.
  // sRGB-encode RGB (channels 0..2), pass alpha (channel 3) through linearly.
  // Skipped only when a host opts into doing its own display transform.
  const bool encodeSrgb = std::getenv("PYXIS_OMNI_EXPORT_LINEAR_HDR") == nullptr;
  auto encode = [encodeSrgb](float linear, int channel) -> float {
    if (channel == 3 || !encodeSrgb)
      return linear < 0.f ? 0.f : (linear > 1.f ? 1.f : linear);
    return LinearToSrgb(linear);
  };
  const auto* srcRowBase = reinterpret_cast<const uint16_t*>(src.data());
  for (uint32_t row = 0; row < copyHeight; ++row) {
    const uint16_t* srcRow = srcRowBase + static_cast<size_t>(row) * srcWidth * 4;
    const uint32_t dstRowIdx = copyHeight - 1 - row;  // bottom-up for the viewport
    if (fmt == HdFormatFloat16Vec4) {
      auto* dstRow = reinterpret_cast<uint16_t*>(dst + static_cast<size_t>(dstRowIdx) * bufWidth * 8);
      for (uint32_t col = 0; col < copyWidth; ++col)
        for (int channel = 0; channel < 4; ++channel)
          dstRow[col * 4 + channel] = FloatToHalf(encode(HalfToFloat(srcRow[col * 4 + channel]), channel));
    } else if (fmt == HdFormatUNorm8Vec4) {
      uint8_t* dstRow = dst + static_cast<size_t>(dstRowIdx) * bufWidth * 4;
      for (uint32_t col = 0; col < copyWidth; ++col)
        for (int channel = 0; channel < 4; ++channel)
          dstRow[col * 4 + channel] =
              static_cast<uint8_t>(std::lround(encode(HalfToFloat(srcRow[col * 4 + channel]), channel) * 255.f));
    } else if (fmt == HdFormatFloat32Vec4) {
      auto* dstRow = reinterpret_cast<float*>(dst + static_cast<size_t>(dstRowIdx) * bufWidth * 16);
      for (uint32_t col = 0; col < copyWidth; ++col)
        for (int channel = 0; channel < 4; ++channel)
          dstRow[col * 4 + channel] = encode(HalfToFloat(srcRow[col * 4 + channel]), channel);
    }
  }
  buffer->Unmap();
}

// ---- Render pass (file-local) ---------------------------------------------
// Drives PyxisEngine::RenderFrame (commits the synced GpuScene + renders into
// the exportable image + signals the timeline), then composites the result into
// the host's bound color render buffer.
class HdPyxisRenderPass final : public HdRenderPass {
 public:
  HdPyxisRenderPass(HdRenderIndex* index, HdRprimCollection const& collection,
                    pyxis::hydra::PyxisEngine* engine)
      : HdRenderPass(index, collection), _engine(engine) {}
  ~HdPyxisRenderPass() override = default;

 protected:
  void _Execute(HdRenderPassStateSharedPtr const& renderPassState,
                TfTokenVector const& /*renderTags*/) override {
    // The prims have already Sync'd into the engine's GpuScene; RenderFrame
    // commits it (builds BLAS/TLAS) and renders into the exportable image.
    if (_engine == nullptr || !_engine->IsValid())
      return;

    // Resize the engine to the host's color AOV dimensions so we render at the
    // viewport's resolution + aspect (otherwise a fixed-size render gets cropped
    // to the top-left of the viewport buffer — wrong framing, sky-biased).
    if (renderPassState) {
      for (const HdRenderPassAovBinding& binding : renderPassState->GetAovBindings()) {
        if (binding.aovName == HdAovTokens->color && binding.renderBuffer != nullptr) {
          const uint32_t aovW = binding.renderBuffer->GetWidth();
          const uint32_t aovH = binding.renderBuffer->GetHeight();
          if (aovW > 0 && aovH > 0 && (aovW != _engine->Width() || aovH != _engine->Height()))
            _engine->Resize(aovW, aovH);
          break;
        }
      }
    }

    // §25.O.3 — INGEST VIA STAGEWALKER. When the host has supplied the stage (the
    // common case: PyxisHydraHost, or Kit via the UsdUtilsStageCache fallback), we
    // populate the GpuScene by running the SAME StageWalker as the standalone
    // pyxis_usd_ingest, NOT the per-prim Hydra Sync translation. This makes the
    // World Lobby (geometry + face-subsets + materials + lights + analytic geom +
    // instancing) land BYTE-IDENTICALLY across both adapters — the M4 "wrap
    // StageWalker for byte-equal parity" design — instead of re-implementing (and
    // drifting from) StageWalker in HdPyxis{Mesh,Light}::Sync. Those Sync impls
    // no-op when a stage is present (see SceneOf-stage guard) so the scene is
    // populated exactly once, here. Walked once per stage; a persisted switch-back
    // (new pass, already-populated engine) adopts the resident scene without
    // re-walking; a genuine stage change rebuilds.
    HdRenderParam* renderParam = nullptr;
    if (const HdRenderIndex* renderIndex = GetRenderIndex()) {
      if (const HdRenderDelegate* delegate = renderIndex->GetRenderDelegate())
        renderParam = delegate->GetRenderParam();
    }
    if (const UsdStageRefPtr stage = StageOf(renderParam); stage && _engine->Scene() != nullptr) {
      if (_walkedStage != stage) {
        const bool sceneHasContent = _engine->LastMeshCount() > 0;
        if (_walkedStage == nullptr && sceneHasContent) {
          // Persisted switch-back: a prior delegate already walked this engine's
          // scene. Adopt it (no re-walk, no Clear) — the fast warm path.
          _walkedStage = stage;
        } else {
          if (sceneHasContent)
            _engine->Scene()->Clear();  // stage changed -> rebuild from scratch.
          pyxis::usd_ingest::StageWalker walker;
          const pyxis::usd_ingest::IngestResult result =
              walker.WalkStage(stage, *_engine->Scene());
          (void)result;  // counts surface via Engine()->Last*Count() after commit.
          _walkedStage = stage;
        }
      }
    }

    // Set the camera from the render-pass-state — this is the viewport's ACTIVE
    // camera (incl. the free/navigation camera, which is NOT a USD camera prim and
    // so never reaches HdPyxisCamera::Sync). Without this the scene is rendered
    // with no/stale camera -> black viewport. GetWorldToViewMatrix/GetProjectionMatrix
    // give the active camera's matrices regardless of its source.
    if (renderPassState && _engine->Scene() != nullptr) {
      // The active camera's world-to-view is in stage units; geometry/lights
      // were corrected by stageToWorld (metres + Y-up). Compose the inverse
      // correction so this camera sees the corrected world identically framed:
      //   viewFromWorld = viewFromWorld_stage * inverse(stageToWorld).
      hlslpp::float4x4 stageToWorld = hlslpp::float4x4::identity();
      UsdStageRefPtr stage;
      if (const HdRenderIndex* renderIndex = GetRenderIndex()) {
        if (const HdRenderDelegate* delegate = renderIndex->GetRenderDelegate()) {
          HdRenderParam* renderParam = delegate->GetRenderParam();
          stageToWorld = StageToWorldOf(renderParam);
          stage = StageOf(renderParam);
        }
      }
      pyxis::CameraDesc cam;
      const HdCamera* hdCam = renderPassState->GetCamera();

      // §25.O.3 camera parity: when the active camera IS a USD camera prim and the
      // stage is available, build the projection + view straight from the USD
      // GfCamera — EXACTLY as pyxis_usd_ingest's StageWalker does
      // (gfCamera.GetFrustum().ComputeProjectionMatrix() + inverse(stageToWorld *
      // localToWorld)). Hydra's renderPassState->GetProjectionMatrix() CONFORMS the
      // camera aperture to the viewport aspect, which diverges from StageWalker's
      // raw-aperture frustum and shifts every edge by a sub-pixel/FOV amount. The
      // free/navigation camera (no USD prim) falls back to renderPassState.
      bool usdCameraUsed = false;
      if (stage && hdCam != nullptr && !hdCam->GetId().IsEmpty()) {
        if (const UsdGeomCamera usdCamera{stage->GetPrimAtPath(hdCam->GetId())}) {
          // Mirror StageWalker::EmitCamera field-for-field so the CameraDesc is
          // identical to the standalone's (incl. orthographic mode + intrinsics).
          const GfCamera gfCamera = usdCamera.GetCamera(UsdTimeCode::Default());
          const hlslpp::float4x4 worldFromCamera =
              mul(stageToWorld, ToPyxisMatrix(gfCamera.GetTransform()));
          cam.viewFromWorld = hlslpp::inverse(worldFromCamera);
          cam.projFromView = ToPyxisMatrix(gfCamera.GetFrustum().ComputeProjectionMatrix());
          cam.focalLengthMm = gfCamera.GetFocalLength();
          cam.apertureFStop = gfCamera.GetFStop();
          cam.focusDistance = gfCamera.GetFocusDistance();
          const GfRange1f clip = gfCamera.GetClippingRange();
          cam.nearClip = clip.GetMin();
          cam.farClip = clip.GetMax();
          cam.projectionMode =
              (gfCamera.GetProjection() == GfCamera::Orthographic) ? 1u : 0u;
          usdCameraUsed = true;
        }
      }
      if (!usdCameraUsed) {
        cam.viewFromWorld = mul(ToPyxisMatrix(renderPassState->GetWorldToViewMatrix()),
                                hlslpp::inverse(stageToWorld));
        cam.projFromView = ToPyxisMatrix(renderPassState->GetProjectionMatrix());
        if (hdCam != nullptr) {
          const GfRange1f clip = hdCam->GetClippingRange();
          cam.nearClip = clip.GetMin();
          cam.farClip = clip.GetMax();
        }
      }
      // Exposure (stops): CameraDesc multiplies radiance by 2^exposure before the
      // ACES tonemap. hdCam->GetExposure() returns the same raw stops StageWalker
      // reads from UsdGeomCamera::exposure, so both adapters tonemap identically.
      if (hdCam != nullptr)
        cam.exposure = hdCam->GetExposure();
      _engine->Scene()->SetCamera(cam);
    }

    _engine->RenderFrame();

    // Present: composite Pyxis's color into the host's bound color render buffer
    // (the AOV every Hydra host reads — custom panel / usdview / UsdImagingGL).
    // Per-frame trace (composite / render-pass): gated on PYXIS_OMNI_TRACE, NOT
  // PYXIS_OMNI_DBG, so the default-on DBG keeps only occasional lifecycle prints
  // and doesn't flood every frame.
  const bool dbg = std::getenv("PYXIS_OMNI_TRACE") != nullptr;
    if (renderPassState) {
      const HdRenderPassAovBindingVector& bindings = renderPassState->GetAovBindings();
      if (dbg) {
        std::fprintf(stderr, "PYXISDBG _Execute: engineValid=%d aovBindings=%zu\n",
                     _engine->IsValid() ? 1 : 0, bindings.size());
        for (const HdRenderPassAovBinding& binding : bindings) {
          const HdRenderBuffer* buf = binding.renderBuffer;
          std::fprintf(stderr, "PYXISDBG   aov='%s' buf=%p%s\n", binding.aovName.GetText(),
                       static_cast<const void*>(buf),
                       buf ? "" : " (null; renderBufferId set instead?)");
          if (buf)
            std::fprintf(stderr, "PYXISDBG     bufWxH=%ux%u fmt=%d\n", buf->GetWidth(),
                         buf->GetHeight(), static_cast<int>(buf->GetFormat()));
        }
      }
      bool composited = false;
      for (const HdRenderPassAovBinding& binding : bindings) {
        if (binding.aovName == HdAovTokens->color && binding.renderBuffer != nullptr) {
          WritePyxisColorToAov(*_engine, binding.renderBuffer);
          composited = true;
          break;
        }
      }
      if (dbg)
        std::fprintf(stderr, "PYXISDBG _Execute: composited-to-color=%d\n", composited ? 1 : 0);
    } else if (dbg) {
      std::fprintf(stderr, "PYXISDBG _Execute: renderPassState is NULL\n");
    }
  }

 private:
  pyxis::hydra::PyxisEngine* _engine = nullptr;  // borrowed; owned by the delegate.
  // §25.O.3 — the stage this pass has already ingested via StageWalker. Guards
  // against re-walking each frame; null until the first walk.
  UsdStageRefPtr _walkedStage;
};

}  // namespace

// ---- Render delegate ------------------------------------------------------
HdPyxisRenderDelegate::HdPyxisRenderDelegate() { Init(); }

HdPyxisRenderDelegate::HdPyxisRenderDelegate(HdRenderSettingsMap const& settingsMap)
    : HdRenderDelegate(settingsMap) {
  Init();
}

HdPyxisRenderDelegate::~HdPyxisRenderDelegate() {
  // This runs on every viewport renderer switch (the pxr engine destroys the
  // delegate). Re-read the persist toggle HERE (not just at Init) so the Render
  // Settings checkbox is effective: if the user turned persistence OFF, free the
  // resident engine now so switching away releases its VRAM. If still ON, leave
  // it resident so switch-back reuses warm textures/geometry/BLAS. (The owned /
  // non-persist case lets the unique_ptr drain + tear down as before.)
  const bool persistNow = ReadPersistEngineSetting();
  if (std::getenv("PYXIS_OMNI_DBG") != nullptr)
    std::fprintf(stderr,
                 "PYXISDBG ~HdPyxisRenderDelegate: owns=%d persistNow=%d (engine=%p)\n",
                 _ownsEngine ? 1 : 0, persistNow ? 1 : 0, static_cast<void*>(_engine));
  if (!_ownsEngine && !persistNow) {
    // We were borrowing the process-static engine, but persistence is now OFF
    // (toggle / env): release it so the VRAM is freed on this switch-away.
    PersistentState().engine.reset();
    PersistentState().prims.clear();
    PersistentState().lights.clear();
    PersistentState().stageToken.clear();
  }
  // _ownedEngine (if any) destructs here; a borrowed _engine pointer that we
  // left resident is owned by the holder (intentionally outlives this delegate).
}

void HdPyxisRenderDelegate::SetRenderSetting(TfToken const& key, VtValue const& value) {
  // The pxr engine calls this when the Render Settings panel writes a value.
  // Print it so the carb -> delegate propagation is verifiable live (the panel
  // toggle is otherwise a no-visual control — it only affects switch-back speed
  // + idle VRAM, applied at the next teardown/Init via ReadPersistEngineSetting).
  if (std::getenv("PYXIS_OMNI_DBG") != nullptr) {
    const std::string shown = value.IsHolding<bool>()
                                  ? (value.UncheckedGet<bool>() ? "true" : "false")
                                  : value.GetTypeName();
    std::fprintf(stderr, "PYXISDBG SetRenderSetting '%s' = %s\n", key.GetText(), shown.c_str());
  }
  HdRenderDelegate::SetRenderSetting(key, value);
}

bool HdPyxisRenderDelegate::ReadPersistEngineSetting() const {
  // NOTE: a fresh delegate's GetRenderSetting at Init() returns the descriptor
  // default (true) unless the host passed the persisted carb value via the
  // HdRenderSettingsMap ctor. That's acceptable — the PYXIS_OMNI_NO_PERSIST env
  // override below remains the hard guarantee for disabling persistence
  // (headless / VRAM-constrained), independent of carb propagation timing.
  // Env override wins (and means OFF) so persistence can be disabled without UI.
  if (std::getenv("PYXIS_OMNI_NO_PERSIST") != nullptr)
    return false;
  static const TfToken PERSIST_NAME("pyxis:persistEngine");
  const VtValue value = GetRenderSetting(PERSIST_NAME);  // 1-arg overload -> VtValue
  bool persist = true;  // default ON (setting absent / unknown type)
  if (value.IsHolding<bool>())
    persist = value.UncheckedGet<bool>();
  else if (value.IsHolding<int>())
    persist = value.UncheckedGet<int>() != 0;
  else if (value.IsHolding<std::string>()) {
    const std::string& str = value.UncheckedGet<std::string>();
    persist = !(str == "0" || str == "false" || str == "False");
  }
  if (std::getenv("PYXIS_OMNI_DBG") != nullptr)
    std::fprintf(stderr, "PYXISDBG persistEngine read: empty=%d -> persist=%d\n",
                 value.IsEmpty() ? 1 : 0, persist ? 1 : 0);
  return persist;
}

void HdPyxisRenderDelegate::Init() {
  _resourceRegistry = std::make_shared<HdResourceRegistry>();
  _supportedRprimTypes = {HdPrimTypeTokens->mesh};
  // Every UsdLux light type StageWalker::EmitLight handles must appear here, or
  // Hydra never creates the Sprim and the light silently vanishes (§25.O.3
  // parity bug: the World Lobby chandeliers are sphere / disk / cylinder
  // lights, so listing only distant/dome/rect dropped 29 of 30 lights).
  // sphereLight + diskLight + cylinderLight added. Note: this nv-usd build has
  // no geometryLight / portalLight Hydra Sprim tokens (UsdLuxGeometryLight is
  // surfaced as `meshLight`, no portal Sprim) — StageWalker's Geometry/Portal
  // kinds have no Hydra delegate entry point; see report.
  _supportedSprimTypes = {HdPrimTypeTokens->camera, HdPrimTypeTokens->material,
                          HdPrimTypeTokens->distantLight, HdPrimTypeTokens->domeLight,
                          HdPrimTypeTokens->rectLight, HdPrimTypeTokens->sphereLight,
                          HdPrimTypeTokens->diskLight, HdPrimTypeTokens->cylinderLight};
  _supportedBprimTypes = {HdPrimTypeTokens->renderBuffer};

  // Advertise our render settings (always, regardless of the persist value) so
  // the pxr engine bridges carb <-> Get/SetRenderSetting via these descriptors.
  // { display name, token key, default VtValue }.
  _settingDescriptors.clear();
  _settingDescriptors.push_back(
      {"Persist engine across renderer switches", TfToken("pyxis:persistEngine"), VtValue(true)});
  _settingDescriptors.push_back(
      {"Stage token (internal)", TfToken("pyxis:stageToken"), VtValue(std::string())});

  // Resolve the persist toggle: pyxis:persistEngine render setting (default
  // true), overridden OFF by PYXIS_OMNI_NO_PERSIST (for VRAM-constrained / large
  // scenes + headless). The render setting is USD-native (no carb dependency)
  // so the standalone/usdview build still compiles.
  _persistEngine = ReadPersistEngineSetting();

  if (_persistEngine) {
    PersistentPyxisState& state = PersistentState();
    if (state.engine != nullptr && state.engine->IsValid()) {
      // BORROW the resident engine — the fast switch-back path. No device init,
      // no texture/geometry re-upload; the prims re-sync as content-hash cache
      // hits.
      _engine = state.engine.get();
      _ownsEngine = false;
      if (std::getenv("PYXIS_OMNI_DBG") != nullptr)
        std::fprintf(stderr, "PYXISDBG Init: borrowing resident PyxisEngine (engine=%p)\n",
                     static_cast<void*>(_engine));
    } else {
      // No resident engine yet — create one and MOVE it into the holder, then
      // borrow from there.
      auto engine = std::make_unique<pyxis::hydra::PyxisEngine>();
      if (!engine->Initialize(1280, 720)) {
        _engine = nullptr;
        return;
      }
      state.engine = std::move(engine);
      _engine = state.engine.get();
      _ownsEngine = false;
    }
  } else {
    // Non-persist: own the engine in the delegate, tearing down on destruction
    // (today's behaviour). Default size; a real viewport resizes via the AOV.
    //
    // If a previously-persisted engine is still resident in the holder (the user
    // turned persistence OFF), release it now so we don't keep ~10 GB pinned
    // alongside the new owned engine (the whole point of the toggle is to free
    // that VRAM). Safe here: the pxr engine destroys the prior (borrowing)
    // delegate before constructing this one, so nothing is borrowing it.
    if (PersistentPyxisState& state = PersistentState(); state.engine != nullptr) {
      state.engine.reset();
      state.prims.clear();
      state.stageToken.clear();
    }
    _ownedEngine = std::make_unique<pyxis::hydra::PyxisEngine>();
    if (!_ownedEngine->Initialize(1280, 720)) {
      _ownedEngine.reset();
      _engine = nullptr;
      return;
    }
    _engine = _ownedEngine.get();
    _ownsEngine = true;
  }

  _renderParam = std::make_unique<HdPyxisRenderParam>(_engine->Scene(), _engine->ProfilerPtr(),
                                                      _engine, _persistEngine);
}

const TfTokenVector& HdPyxisRenderDelegate::GetSupportedRprimTypes() const {
  return _supportedRprimTypes;
}
const TfTokenVector& HdPyxisRenderDelegate::GetSupportedSprimTypes() const {
  return _supportedSprimTypes;
}
const TfTokenVector& HdPyxisRenderDelegate::GetSupportedBprimTypes() const {
  return _supportedBprimTypes;
}

HdRenderSettingDescriptorList HdPyxisRenderDelegate::GetRenderSettingDescriptors() const {
  return _settingDescriptors;
}

HdAovDescriptor HdPyxisRenderDelegate::GetDefaultAovDescriptor(TfToken const& name) const {
  // color: half-float RGBA (matches PyxisEngine's exported tonemapped readback).
  if (name == HdAovTokens->color)
    return HdAovDescriptor(HdFormatFloat16Vec4, false, VtValue(GfVec4f(0.0f, 0.0f, 0.0f, 1.0f)));
  // depth: needed by HdxTaskController for the depth AOV it always sets up.
  if (name == HdAovTokens->depth)
    return HdAovDescriptor(HdFormatFloat32, false, VtValue(1.0f));
  // id AOVs (picking) — int32, cleared to -1.
  if (name == HdAovTokens->primId || name == HdAovTokens->instanceId ||
      name == HdAovTokens->elementId)
    return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
  // Everything else: a generic float16x4 buffer so the host can still bind it.
  return HdAovDescriptor(HdFormatFloat16Vec4, false, VtValue(GfVec4f(0.0f)));
}

HdRenderParam* HdPyxisRenderDelegate::GetRenderParam() const { return _renderParam.get(); }

void HdPyxisRenderDelegate::SetStageToWorld(double metersPerUnit, bool stageIsZUp) noexcept {
  if (_renderParam == nullptr)
    return;
  // Mirror StageWalker::BuildStageContext: uniform metresPerUnit scale, then a
  // Z-up->Y-up rotation if the stage is Z-up. Column-vector convention (§10):
  // basis vectors land in the matrix columns. metresPerUnit <= 0 is corrupt
  // metadata — fall back to 1 (no scale) defensively, as StageWalker does.
  const float scaleFactor =
      (metersPerUnit > 0.0) ? static_cast<float>(metersPerUnit) : 1.0f;
  const hlslpp::float4x4 scale(
      hlslpp::float4{scaleFactor, 0.0f, 0.0f, 0.0f}, hlslpp::float4{0.0f, scaleFactor, 0.0f, 0.0f},
      hlslpp::float4{0.0f, 0.0f, scaleFactor, 0.0f}, hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f});
  hlslpp::float4x4 stageToWorld = scale;
  if (stageIsZUp) {
    const hlslpp::float4x4 rot(
        hlslpp::float4{1.0f, 0.0f, 0.0f, 0.0f}, hlslpp::float4{0.0f, 0.0f, 1.0f, 0.0f},
        hlslpp::float4{0.0f, -1.0f, 0.0f, 0.0f}, hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f});
    stageToWorld = mul(rot, scale);
  }
  _renderParam->SetStageToWorld(stageToWorld);
}

void HdPyxisRenderDelegate::SetStage(const UsdStageRefPtr& stage) noexcept {
  if (_renderParam != nullptr)
    _renderParam->SetStage(stage);
}

HdResourceRegistrySharedPtr HdPyxisRenderDelegate::GetResourceRegistry() const {
  return _resourceRegistry;
}

HdRenderPassSharedPtr HdPyxisRenderDelegate::CreateRenderPass(
    HdRenderIndex* index, HdRprimCollection const& collection) {
  return HdRenderPassSharedPtr(new HdPyxisRenderPass(index, collection, _engine));
}

HdInstancer* HdPyxisRenderDelegate::CreateInstancer(HdSceneDelegate* /*delegate*/,
                                                    SdfPath const& /*id*/) {
  return nullptr;
}
void HdPyxisRenderDelegate::DestroyInstancer(HdInstancer* instancer) { delete instancer; }

HdRprim* HdPyxisRenderDelegate::CreateRprim(TfToken const& typeId, SdfPath const& rprimId) {
  if (typeId == HdPrimTypeTokens->mesh)
    return new HdPyxisMesh(rprimId);
  return nullptr;
}
void HdPyxisRenderDelegate::DestroyRprim(HdRprim* rprim) { delete rprim; }

HdSprim* HdPyxisRenderDelegate::CreateSprim(TfToken const& typeId, SdfPath const& sprimId) {
  if (typeId == HdPrimTypeTokens->camera)
    return new HdPyxisCamera(sprimId);
  if (typeId == HdPrimTypeTokens->material)
    return new StubMaterial(sprimId);
  if (typeId == HdPrimTypeTokens->distantLight)
    return new HdPyxisLight(sprimId, pyxis::LightDesc::Kind::Distant);
  if (typeId == HdPrimTypeTokens->domeLight)
    return new HdPyxisLight(sprimId, pyxis::LightDesc::Kind::Dome);
  // RectLight / DiskLight / SphereLight all map to LightDesc::Kind::Rect (the
  // StageWalker fold); the RectShape tag preserves the per-shape area-normalize
  // formula (w·h / π·r² / 4π·r²).
  if (typeId == HdPrimTypeTokens->rectLight)
    return new HdPyxisLight(sprimId, pyxis::LightDesc::Kind::Rect,
                            HdPyxisLight::RectShape::Rect);
  if (typeId == HdPrimTypeTokens->diskLight)
    return new HdPyxisLight(sprimId, pyxis::LightDesc::Kind::Rect,
                            HdPyxisLight::RectShape::Disk);
  if (typeId == HdPrimTypeTokens->sphereLight)
    return new HdPyxisLight(sprimId, pyxis::LightDesc::Kind::Rect,
                            HdPyxisLight::RectShape::Sphere);
  if (typeId == HdPrimTypeTokens->cylinderLight)
    return new HdPyxisLight(sprimId, pyxis::LightDesc::Kind::Cylinder);
  return nullptr;
}
HdSprim* HdPyxisRenderDelegate::CreateFallbackSprim(TfToken const& typeId) {
  if (typeId == HdPrimTypeTokens->camera)
    return new HdPyxisCamera(SdfPath::EmptyPath());
  if (typeId == HdPrimTypeTokens->material)
    return new StubMaterial(SdfPath::EmptyPath());
  return new HdPyxisLight(SdfPath::EmptyPath(), pyxis::LightDesc::Kind::Distant);
}
void HdPyxisRenderDelegate::DestroySprim(HdSprim* sprim) { delete sprim; }

HdBprim* HdPyxisRenderDelegate::CreateBprim(TfToken const& typeId, SdfPath const& bprimId) {
  if (std::getenv("PYXIS_OMNI_DBG") != nullptr)
    std::fprintf(stderr, "PYXISDBG CreateBprim type='%s' id='%s'\n", typeId.GetText(),
                 bprimId.GetText());
  if (typeId == HdPrimTypeTokens->renderBuffer)
    return new StubRenderBuffer(bprimId);
  return nullptr;
}
HdBprim* HdPyxisRenderDelegate::CreateFallbackBprim(TfToken const& typeId) {
  if (typeId == HdPrimTypeTokens->renderBuffer)
    return new StubRenderBuffer(SdfPath::EmptyPath());
  return nullptr;
}
void HdPyxisRenderDelegate::DestroyBprim(HdBprim* bprim) { delete bprim; }

void HdPyxisRenderDelegate::CommitResources(HdChangeTracker* /*tracker*/) {
  // GpuScene commit happens inside PyxisEngine::RenderFrame (on the render
  // pass) so it lands on the same command list as the render.
}

PXR_NAMESPACE_CLOSE_SCOPE
