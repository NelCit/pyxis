// Pyxis Omniverse Hydra delegate — RFC 0004 Stage 3 (C4).
//
// Ported from sources/pyxis_hydra (the vcpkg-USD desktop delegate), adapted to
// build out-of-tree against Kit's nv-usd 25.11. Mesh + camera prim Sync impls
// push to the delegate-owned PyxisEngine's GpuScene; the render pass drives
// PyxisEngine::RenderFrame.

#include "HdPyxisOmniRenderDelegate.h"

#include "HdPyxisOmniRenderParam.h"
#include "PyxisEngine.h"

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/InstanceDesc.h>
#include <Pyxis/Renderer/Descs/LightDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>
#include <Pyxis/Renderer/GpuScene.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sprim.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/path.h>

#include <hlsl++.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

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
  auto* param = static_cast<HdPyxisOmniRenderParam*>(renderParam);
  return param ? param->GetGpuScene() : nullptr;
}

// Resolve a mesh's bound material to a Pyxis MaterialHandle by reading its
// UsdPreviewSurface network and translating the core params -> OpenPBR. Done in
// the mesh Sync (not a separate registry) so it is independent of prim sync
// order; GpuScene::AcquireMaterial dedups by hash. Returns Invalid (renderer's
// default material) when there is no binding / network.
pyxis::MaterialHandle ResolveMaterial(HdSceneDelegate* sceneDelegate, SdfPath const& materialId,
                                      pyxis::GpuScene& scene) {
  if (materialId.IsEmpty())
    return pyxis::MaterialHandle::Invalid;
  const VtValue resource = sceneDelegate->GetMaterialResource(materialId);
  if (!resource.IsHolding<HdMaterialNetworkMap>())
    return pyxis::MaterialHandle::Invalid;

  static const TfToken kDiffuse("diffuseColor");
  static const TfToken kMetallic("metallic");
  static const TfToken kRoughness("roughness");
  static const TfToken kEmissive("emissiveColor");
  static const TfToken kOpacity("opacity");

  pyxis::OpenPBRMaterialDesc desc;  // sensible defaults from the POD.
  const auto& netMap = resource.UncheckedGet<HdMaterialNetworkMap>();
  const auto it = netMap.map.find(HdMaterialTerminalTokens->surface);
  if (it != netMap.map.end()) {
    for (const HdMaterialNode& node : it->second.nodes) {
      for (const auto& param : node.parameters) {
        const TfToken& name = param.first;
        const VtValue& value = param.second;
        if (name == kDiffuse && value.IsHolding<GfVec3f>()) {
          const GfVec3f color = value.UncheckedGet<GfVec3f>();
          desc.baseColor = {color[0], color[1], color[2]};
        } else if (name == kMetallic && value.IsHolding<float>()) {
          desc.metalness = value.UncheckedGet<float>();
        } else if (name == kRoughness && value.IsHolding<float>()) {
          desc.roughness = value.UncheckedGet<float>();
        } else if (name == kEmissive && value.IsHolding<GfVec3f>()) {
          const GfVec3f color = value.UncheckedGet<GfVec3f>();
          desc.emissionColor = {color[0], color[1], color[2]};
          if (color[0] + color[1] + color[2] > 0.0f)
            desc.emissionLuminance = 1.0f;
        } else if (name == kOpacity && value.IsHolding<float>()) {
          desc.opacity = value.UncheckedGet<float>();
        }
      }
    }
  }
  return scene.AcquireMaterial(desc);
}

// ---- Mesh: Hydra mesh -> GpuScene::CreateMesh + AppendInstance ------------
class HdPyxisOmniMesh final : public HdMesh {
 public:
  explicit HdPyxisOmniMesh(SdfPath const& primId) : HdMesh(primId) {}

  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam, HdDirtyBits* dirtyBits,
            TfToken const& /*reprToken*/) override {
    pyxis::GpuScene* scene = SceneOf(renderParam);
    if (scene == nullptr || sceneDelegate == nullptr) {
      *dirtyBits = HdChangeTracker::Clean;
      return;
    }
    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, GetId()) ||
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, GetId(), HdTokens->points)) {
      EmitToScene(sceneDelegate, *scene);
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
  void EmitToScene(HdSceneDelegate* sceneDelegate, pyxis::GpuScene& scene) {
    const SdfPath& primId = GetId();
    const HdMeshTopology topology = sceneDelegate->GetMeshTopology(primId);
    const VtIntArray& faceCounts = topology.GetFaceVertexCounts();
    const VtIntArray& faceIndices = topology.GetFaceVertexIndices();

    // Triangulate fan-style so quads/ngons render too (the desktop adapter's
    // M4 stub only took triangles; here we fan-triangulate any convex face).
    std::vector<uint32_t> indices;
    {
      size_t cursor = 0;
      for (const int faceCount : faceCounts) {
        if (faceCount >= 3) {
          for (int v = 1; v + 1 < faceCount; ++v) {
            indices.push_back(static_cast<uint32_t>(faceIndices[cursor]));
            indices.push_back(static_cast<uint32_t>(faceIndices[cursor + v]));
            indices.push_back(static_cast<uint32_t>(faceIndices[cursor + v + 1]));
          }
        }
        cursor += static_cast<size_t>(faceCount);
      }
    }

    const VtValue pointsVal = sceneDelegate->Get(primId, HdTokens->points);
    if (!pointsVal.IsHolding<VtVec3fArray>())
      return;
    const VtVec3fArray& pointsArray = pointsVal.UncheckedGet<VtVec3fArray>();
    if (pointsArray.empty() || indices.empty())
      return;

    std::vector<hlslpp::float3> positions;
    positions.reserve(pointsArray.size());
    for (const GfVec3f& point : pointsArray)
      positions.emplace_back(point[0], point[1], point[2]);

    const std::string debugName = primId.GetString();
    pyxis::MeshDesc meshDesc;
    meshDesc.positions = positions;
    meshDesc.indices = indices;
    meshDesc.debugName = debugName;
    const auto meshHandle = scene.CreateMesh(meshDesc);
    if (!meshHandle.has_value())
      return;

    const GfMatrix4d worldFromLocal = sceneDelegate->GetTransform(primId);
    pyxis::InstanceDesc instanceDesc;
    instanceDesc.mesh = *meshHandle;
    instanceDesc.material = ResolveMaterial(sceneDelegate, sceneDelegate->GetMaterialId(primId), scene);
    instanceDesc.worldFromLocal = ToPyxisMatrix(worldFromLocal);
    instanceDesc.debugName = debugName;
    (void)scene.AppendInstance(instanceDesc);  // TLAS-cap exhaustion surfaces in FrameStats.
  }
};

// ---- Camera: Hydra camera -> GpuScene::SetCamera --------------------------
class HdPyxisOmniCamera final : public HdCamera {
 public:
  explicit HdPyxisOmniCamera(SdfPath const& primId) : HdCamera(primId) {}

  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
            HdDirtyBits* dirtyBits) override {
    HdCamera::Sync(sceneDelegate, renderParam, dirtyBits);
    pyxis::GpuScene* scene = SceneOf(renderParam);
    if (scene != nullptr && sceneDelegate != nullptr) {
      const SdfPath& primId = GetId();
      const GfMatrix4d worldFromLocal = sceneDelegate->GetTransform(primId);
      pyxis::CameraDesc cameraDesc;
      cameraDesc.viewFromWorld = ToPyxisMatrix(worldFromLocal.GetInverse());
      cameraDesc.projFromView = ToPyxisMatrix(ComputeProjectionMatrix());
      const GfRange1f clipRange = GetClippingRange();
      cameraDesc.nearClip = clipRange.GetMin();
      cameraDesc.farClip = clipRange.GetMax();
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
class HdPyxisOmniLight final : public HdLight {
 public:
  HdPyxisOmniLight(SdfPath const& primId, pyxis::LightDesc::Kind kind)
      : HdLight(primId), _kind(kind) {}

  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
            HdDirtyBits* dirtyBits) override {
    pyxis::GpuScene* scene = SceneOf(renderParam);
    if (scene != nullptr && sceneDelegate != nullptr) {
      const SdfPath& primId = GetId();
      pyxis::LightDesc desc;
      desc.kind = _kind;
      const VtValue color = sceneDelegate->GetLightParamValue(primId, HdLightTokens->color);
      if (color.IsHolding<GfVec3f>()) {
        const GfVec3f rgb = color.UncheckedGet<GfVec3f>();
        desc.color = {rgb[0], rgb[1], rgb[2]};
      }
      const VtValue intensity = sceneDelegate->GetLightParamValue(primId, HdLightTokens->intensity);
      if (intensity.IsHolding<float>())
        desc.intensity = intensity.UncheckedGet<float>();

      const GfMatrix4d xform = sceneDelegate->GetTransform(primId);
      const GfVec3d dir = xform.TransformDir(GfVec3d(0, 0, -1)).GetNormalized();
      desc.direction = {static_cast<float>(dir[0]), static_cast<float>(dir[1]),
                        static_cast<float>(dir[2])};
      const GfVec3d pos = xform.ExtractTranslation();
      desc.position = {static_cast<float>(pos[0]), static_cast<float>(pos[1]),
                       static_cast<float>(pos[2])};
      (void)scene->AddLight(desc);
    }
    *dirtyBits = HdChangeTracker::Clean;
  }

  [[nodiscard]] HdDirtyBits GetInitialDirtyBitsMask() const override { return HdLight::AllDirty; }

 private:
  pyxis::LightDesc::Kind _kind;
};

// Minimal CPU-backed render buffer so HdEngine's AOV machinery doesn't crash.
// Pyxis renders into its own exportable image (PyxisEngine), so this buffer is
// not the render target; the C4-full step aliases it to the imported image.
class StubRenderBuffer final : public HdRenderBuffer {
 public:
  explicit StubRenderBuffer(SdfPath const& primId) : HdRenderBuffer(primId) {}

  bool Allocate(GfVec3i const& dimensions, HdFormat format, bool /*multiSampled*/) override {
    _width = static_cast<uint32_t>(dimensions[0]);
    _height = static_cast<uint32_t>(dimensions[1]);
    _format = format;
    _data.assign(static_cast<size_t>(_width) * _height * HdDataSizeOfFormat(format), 0);
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
      int e = -1;
      uint32_t m = mant;
      do {
        ++e;
        m <<= 1;
      } while ((m & 0x400u) == 0);
      bits = (sign << 31) | (static_cast<uint32_t>(127 - 15 - e) << 23) | ((m & 0x3FFu) << 13);
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

// Composite Pyxis's rendered color (RGBA16F, from PyxisEngine readback) into the
// host's bound color HdRenderBuffer. This is the host-facing presentation step —
// every display path (custom panel, usdview, UsdImagingGL) reads this buffer.
// Supports the common AOV formats; converts from RGBA16F as needed. RFC 0004.
void WritePyxisColorToAov(pyxis_omni::PyxisEngine& engine, HdRenderBuffer* buffer) {
  if (buffer == nullptr || buffer->GetWidth() == 0 || buffer->GetHeight() == 0)
    return;
  std::vector<uint8_t> src;
  uint32_t sw = 0, sh = 0;
  if (!engine.ReadbackColorHdr(src, sw, sh) || sw == 0 || sh == 0)
    return;
  const uint32_t bw = buffer->GetWidth(), bh = buffer->GetHeight();
  const uint32_t cw = std::min(sw, bw), ch = std::min(sh, bh);
  const HdFormat fmt = buffer->GetFormat();
  auto* dst = static_cast<uint8_t*>(buffer->Map());
  if (dst == nullptr)
    return;
  const auto* srcRowBase = reinterpret_cast<const uint16_t*>(src.data());
  for (uint32_t y = 0; y < ch; ++y) {
    const uint16_t* s = srcRowBase + static_cast<size_t>(y) * sw * 4;
    if (fmt == HdFormatFloat16Vec4) {
      std::memcpy(dst + static_cast<size_t>(y) * bw * 8, s, static_cast<size_t>(cw) * 8);
    } else if (fmt == HdFormatUNorm8Vec4) {
      uint8_t* d = dst + static_cast<size_t>(y) * bw * 4;
      for (uint32_t x = 0; x < cw; ++x) {
        for (int c = 0; c < 4; ++c) {
          float v = HalfToFloat(s[x * 4 + c]);
          v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
          d[x * 4 + c] = static_cast<uint8_t>(v * 255.f + 0.5f);
        }
      }
    } else if (fmt == HdFormatFloat32Vec4) {
      auto* d = reinterpret_cast<float*>(dst + static_cast<size_t>(y) * bw * 16);
      for (uint32_t x = 0; x < cw * 4; ++x)
        d[x] = HalfToFloat(s[x]);
    }
  }
  buffer->Unmap();
}

}  // namespace

// ---- Render pass ----------------------------------------------------------
HdPyxisOmniRenderPass::HdPyxisOmniRenderPass(HdRenderIndex* index,
                                             HdRprimCollection const& collection,
                                             pyxis_omni::PyxisEngine* engine)
    : HdRenderPass(index, collection), _engine(engine) {}

HdPyxisOmniRenderPass::~HdPyxisOmniRenderPass() = default;

void HdPyxisOmniRenderPass::_Execute(HdRenderPassStateSharedPtr const& renderPassState,
                                     TfTokenVector const& /*renderTags*/) {
  // The prims have already Sync'd into the engine's GpuScene; RenderFrame
  // commits it (builds BLAS/TLAS) and renders into the exportable image.
  if (_engine == nullptr || !_engine->IsValid())
    return;
  _engine->RenderFrame();

  // Present: composite Pyxis's color into the host's bound color render buffer
  // (the AOV every Hydra host reads — custom panel / usdview / UsdImagingGL).
  if (renderPassState) {
    for (const HdRenderPassAovBinding& binding : renderPassState->GetAovBindings()) {
      if (binding.aovName == HdAovTokens->color && binding.renderBuffer != nullptr) {
        WritePyxisColorToAov(*_engine, binding.renderBuffer);
        break;
      }
    }
  }
}

// ---- Render delegate ------------------------------------------------------
HdPyxisOmniRenderDelegate::HdPyxisOmniRenderDelegate() { _Initialize(); }

HdPyxisOmniRenderDelegate::HdPyxisOmniRenderDelegate(HdRenderSettingsMap const& settingsMap)
    : HdRenderDelegate(settingsMap) {
  _Initialize();
}

HdPyxisOmniRenderDelegate::~HdPyxisOmniRenderDelegate() = default;

void HdPyxisOmniRenderDelegate::_Initialize() {
  _resourceRegistry = std::make_shared<HdResourceRegistry>();
  _supportedRprimTypes = {HdPrimTypeTokens->mesh};
  _supportedSprimTypes = {HdPrimTypeTokens->camera, HdPrimTypeTokens->material,
                          HdPrimTypeTokens->distantLight, HdPrimTypeTokens->domeLight,
                          HdPrimTypeTokens->rectLight};
  _supportedBprimTypes = {HdPrimTypeTokens->renderBuffer};

  // Stand up the Pyxis render engine (own Vulkan device + scene + renderer +
  // exporter). Default size; a real viewport resizes via the render buffer.
  _engine = std::make_unique<pyxis_omni::PyxisEngine>();
  if (!_engine->Initialize(1280, 720)) {
    _engine.reset();
    return;
  }
  _renderParam =
      std::make_unique<HdPyxisOmniRenderParam>(_engine->Scene(), _engine->ProfilerPtr());
}

const TfTokenVector& HdPyxisOmniRenderDelegate::GetSupportedRprimTypes() const {
  return _supportedRprimTypes;
}
const TfTokenVector& HdPyxisOmniRenderDelegate::GetSupportedSprimTypes() const {
  return _supportedSprimTypes;
}
const TfTokenVector& HdPyxisOmniRenderDelegate::GetSupportedBprimTypes() const {
  return _supportedBprimTypes;
}

HdRenderParam* HdPyxisOmniRenderDelegate::GetRenderParam() const { return _renderParam.get(); }

HdResourceRegistrySharedPtr HdPyxisOmniRenderDelegate::GetResourceRegistry() const {
  return _resourceRegistry;
}

HdRenderPassSharedPtr HdPyxisOmniRenderDelegate::CreateRenderPass(
    HdRenderIndex* index, HdRprimCollection const& collection) {
  return HdRenderPassSharedPtr(new HdPyxisOmniRenderPass(index, collection, _engine.get()));
}

HdInstancer* HdPyxisOmniRenderDelegate::CreateInstancer(HdSceneDelegate* /*delegate*/,
                                                        SdfPath const& /*id*/) {
  return nullptr;
}
void HdPyxisOmniRenderDelegate::DestroyInstancer(HdInstancer* instancer) { delete instancer; }

HdRprim* HdPyxisOmniRenderDelegate::CreateRprim(TfToken const& typeId, SdfPath const& rprimId) {
  if (typeId == HdPrimTypeTokens->mesh)
    return new HdPyxisOmniMesh(rprimId);
  return nullptr;
}
void HdPyxisOmniRenderDelegate::DestroyRprim(HdRprim* rprim) { delete rprim; }

HdSprim* HdPyxisOmniRenderDelegate::CreateSprim(TfToken const& typeId, SdfPath const& sprimId) {
  if (typeId == HdPrimTypeTokens->camera)
    return new HdPyxisOmniCamera(sprimId);
  if (typeId == HdPrimTypeTokens->material)
    return new StubMaterial(sprimId);
  if (typeId == HdPrimTypeTokens->distantLight)
    return new HdPyxisOmniLight(sprimId, pyxis::LightDesc::Kind::Distant);
  if (typeId == HdPrimTypeTokens->domeLight)
    return new HdPyxisOmniLight(sprimId, pyxis::LightDesc::Kind::Dome);
  if (typeId == HdPrimTypeTokens->rectLight)
    return new HdPyxisOmniLight(sprimId, pyxis::LightDesc::Kind::Rect);
  return nullptr;
}
HdSprim* HdPyxisOmniRenderDelegate::CreateFallbackSprim(TfToken const& typeId) {
  if (typeId == HdPrimTypeTokens->camera)
    return new HdPyxisOmniCamera(SdfPath::EmptyPath());
  if (typeId == HdPrimTypeTokens->material)
    return new StubMaterial(SdfPath::EmptyPath());
  return new HdPyxisOmniLight(SdfPath::EmptyPath(), pyxis::LightDesc::Kind::Distant);
}
void HdPyxisOmniRenderDelegate::DestroySprim(HdSprim* sprim) { delete sprim; }

HdBprim* HdPyxisOmniRenderDelegate::CreateBprim(TfToken const& typeId, SdfPath const& bprimId) {
  if (typeId == HdPrimTypeTokens->renderBuffer)
    return new StubRenderBuffer(bprimId);
  return nullptr;
}
HdBprim* HdPyxisOmniRenderDelegate::CreateFallbackBprim(TfToken const& typeId) {
  if (typeId == HdPrimTypeTokens->renderBuffer)
    return new StubRenderBuffer(SdfPath::EmptyPath());
  return nullptr;
}
void HdPyxisOmniRenderDelegate::DestroyBprim(HdBprim* bprim) { delete bprim; }

void HdPyxisOmniRenderDelegate::CommitResources(HdChangeTracker* /*tracker*/) {
  // GpuScene commit happens inside PyxisEngine::RenderFrame (on the render
  // pass) so it lands on the same command list as the render.
}

PXR_NAMESPACE_CLOSE_SCOPE
