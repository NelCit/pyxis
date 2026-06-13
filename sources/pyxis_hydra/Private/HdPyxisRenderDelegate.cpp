// Pyxis Hydra — HdRenderDelegate. Plan §7 / RFC 0006.
//
// The single Pyxis Hydra delegate (RFC 0006 collapsed the pyxis_hydra_omni fork
// onto this source once both shared Kit's nv-usd). It owns a PyxisEngine. Ingest
// is StageWalker-only: the render pass populates the engine's GpuScene by running
// the SAME StageWalker as the standalone pyxis_usd_ingest (byte-identical across
// both adapters, §25.O.3) and builds the camera from the USD GfCamera. The mesh /
// camera / light Rprim/Sprim classes still exist (so Hydra creates + tracks the
// prims) but their Sync impls are no-ops that just clear the dirty bits — the
// per-prim Hydra translation has been removed. The render pass drives
// PyxisEngine::RenderFrame and composites the result into the host's color AOV.

#include "HdPyxisRenderDelegate.h"

#include "AovColorEncode.h"
#include "GlVkInterop.h"
#include "HdPyxisRenderParam.h"
#include "PyxisEngine.h"

#include <Pyxis/Platform/Color/ColorEncoding.h>  // shared half/sRGB conversions
#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/LightDesc.h>  // LightDesc::Kind — Sprim registration
#include <Pyxis/Renderer/Descs/RenderSettings.h>  // Q3 — openPbrFeatureMask drift pin
#include <Pyxis/Renderer/GpuScene.h>

// §25.O.3 — the SHARED stage ingest: the render pass populates the GpuScene by
// running the SAME StageWalker as the standalone, so the World Lobby (geometry,
// face-subsets, materials, lights, analytic geom, instancing) lands BYTE-
// IDENTICALLY across both adapters. This is the ONLY ingest path now — the
// per-prim Hydra Sync translation has been removed.
#include <Pyxis/UsdIngest/StageWalker.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/driver.h>
#include <pxr/imaging/hgi/hgi.h>
#include <pxr/imaging/hgi/texture.h>
#include <pxr/imaging/hgiGL/texture.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sprim.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/metrics.h>     // UsdGeomGetStageMetersPerUnit / UpAxis
#include <pxr/usd/usdGeom/tokens.h>      // UsdGeomTokens->z
#include <pxr/usd/usdUtils/stageCache.h> // Kit stage discovery (no host SetStage)

#include <vector>

#include <hlsl++.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// ---- Persistent engine across pxr renderer switches -----------------------
// The closed omni.hydra.pxr engine DESTROYS this delegate when the user
// switches the viewport renderer away from Pyxis and CONSTRUCTS A FRESH ONE on
// switch-back. To avoid rebuilding the whole engine (Vulkan device + GpuScene +
// re-uploading every texture/mesh/BLAS) on every switch-back, the PyxisEngine
// lives in a process-static holder that outlives any single delegate instance.
// On switch-back the render pass adopts the resident (already StageWalker-walked)
// scene without re-walking (see HdPyxisRenderPass::_walkedStage), so the warm
// textures/geometry/BLAS are reused.
//
// Render setting:
//   pyxis:persistEngine (bool, default true) — toggle the persistence.
// Env override PYXIS_OMNI_NO_PERSIST forces persistence OFF (for the
// VRAM-constrained / large-scene case, and for headless).
struct PersistentPyxisState {
  std::unique_ptr<pyxis::hydra::PyxisEngine> engine;
};

PersistentPyxisState& PersistentState() {
  static PersistentPyxisState state;
  return state;
}

// ---- Q3 OpenPBR feature gates (openpbr-complete-design.md "Control surface") --
// File-local mirror of the shaderinterop::OPENPBR_FEATURE_* bits. pyxis_hydra
// does not include ShaderInterop.slang (renderer-private interop file), so the
// six bits are restated here and pinned: the static_assert ties the composed
// ALL to RenderSettings::openPbrFeatureMask's default, and PyxisRenderer.cpp
// ties THAT default to the interop constant — a drift anywhere fails to compile.
constexpr uint32_t OPENPBR_FEATURE_COAT         = 1u << 0;
constexpr uint32_t OPENPBR_FEATURE_FUZZ         = 1u << 1;
constexpr uint32_t OPENPBR_FEATURE_TRANSMISSION = 1u << 2;
constexpr uint32_t OPENPBR_FEATURE_SUBSURFACE   = 1u << 3;
constexpr uint32_t OPENPBR_FEATURE_ANISOTROPY   = 1u << 4;
constexpr uint32_t OPENPBR_FEATURE_EON_DIFFUSE  = 1u << 5;
constexpr uint32_t OPENPBR_FEATURES_ALL =
    OPENPBR_FEATURE_COAT | OPENPBR_FEATURE_FUZZ | OPENPBR_FEATURE_TRANSMISSION
    | OPENPBR_FEATURE_SUBSURFACE | OPENPBR_FEATURE_ANISOTROPY | OPENPBR_FEATURE_EON_DIFFUSE;
static_assert(OPENPBR_FEATURES_ALL == pyxis::RenderSettings{}.openPbrFeatureMask,
              "Delegate-side OPENPBR_FEATURE_* bits drifted from "
              "RenderSettings::openPbrFeatureMask's default — keep this mirror in lockstep "
              "with ShaderInterop.slang.");

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

// Stage-to-world correction (metresPerUnit + Z->Y) the host derived from the
// stage metadata. Identity when renderParam is null or the host didn't set it
// — see HdPyxisRenderParam::StageToWorld. Applied to every prim/light/camera
// transform so the delegate's world matches StageWalker's metres + Y-up frame.
[[nodiscard]] hlslpp::float4x4 StageToWorldOf(HdRenderParam* renderParam) noexcept {
  auto* param = static_cast<HdPyxisRenderParam*>(renderParam);
  return param ? param->StageToWorld() : hlslpp::float4x4::identity();
}

// The open UsdStage the host supplied (null if none). §25.O.3: the render pass
// ingests it through StageWalker (the same scene as the standalone) and builds
// the camera from its USD GfCamera.
[[nodiscard]] UsdStageRefPtr StageOf(HdRenderParam* renderParam) noexcept {
  auto* param = static_cast<HdPyxisRenderParam*>(renderParam);
  return param ? param->Stage() : UsdStageRefPtr();
}

// Resolve the stage to ingest. PyxisHydraHost sets it explicitly (SetStage). Under
// Kit's omni.hydra.pxr there is NO PyxisHydraHost, so fall back to the open stage
// in the process-global UsdUtilsStageCache (Kit/omni.usd registers the live stage
// there). When several stages are cached (session / anonymous layers), pick the
// one with the most root-level prims — the content stage. Without this fallback the
// delegate has no stage in Kit, never runs StageWalker, and renders an empty scene.
[[nodiscard]] UsdStageRefPtr ResolveStage(HdRenderParam* renderParam) noexcept {
  if (UsdStageRefPtr stage = StageOf(renderParam))
    return stage;
  UsdStageRefPtr best;
  size_t bestRootCount = 0;
  for (const UsdStageRefPtr& cached : UsdUtilsStageCache::Get().GetAllStages()) {
    if (!cached)
      continue;
    size_t rootCount = 0;
    for (const UsdPrim& child : cached->GetPseudoRoot().GetChildren()) {
      (void)child;
      ++rootCount;
    }
    if (!best || rootCount > bestRootCount) {
      best = cached;
      bestRootCount = rootCount;
    }
  }
  return best;
}

// metresPerUnit scale + Z-up->Y-up rotation, matching StageWalker::BuildStageContext
// (§10 column-vector convention). metresPerUnit <= 0 is corrupt metadata -> 1.
[[nodiscard]] hlslpp::float4x4 BuildStageToWorld(double metersPerUnit, bool stageIsZUp) noexcept {
  const float scaleFactor = (metersPerUnit > 0.0) ? static_cast<float>(metersPerUnit) : 1.0f;
  hlslpp::float4x4 scale(
      hlslpp::float4{scaleFactor, 0.0f, 0.0f, 0.0f}, hlslpp::float4{0.0f, scaleFactor, 0.0f, 0.0f},
      hlslpp::float4{0.0f, 0.0f, scaleFactor, 0.0f}, hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f});
  if (!stageIsZUp)
    return scale;
  const hlslpp::float4x4 rot(
      hlslpp::float4{1.0f, 0.0f, 0.0f, 0.0f}, hlslpp::float4{0.0f, 0.0f, 1.0f, 0.0f},
      hlslpp::float4{0.0f, -1.0f, 0.0f, 0.0f}, hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f});
  return mul(rot, scale);
}

// The stage-to-world correction for the resolved stage. The host may have set it
// (SetStageToWorld); otherwise — Kit, no host — derive it from the stage metadata
// so the camera lands in StageWalker's metres + Y-up frame (StageWalker bakes the
// same correction into the geometry itself, so the two must agree).
[[nodiscard]] hlslpp::float4x4 ResolveStageToWorld(HdRenderParam* renderParam,
                                                   const UsdStageRefPtr& stage) noexcept {
  if (StageOf(renderParam))  // host supplied the stage -> it also set the correction.
    return StageToWorldOf(renderParam);
  if (!stage)
    return hlslpp::float4x4::identity();
  return BuildStageToWorld(UsdGeomGetStageMetersPerUnit(stage),
                           UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->z);
}

// ---- Mesh: Hydra mesh (no-op Sync; StageWalker populates the scene) -------
class HdPyxisMesh final : public HdMesh {
 public:
  explicit HdPyxisMesh(SdfPath const& primId) : HdMesh(primId) {}

  // §25.O.3 — the render pass populates the GpuScene in ONE StageWalker pass
  // (byte-identical to the standalone). The per-prim Hydra translation is dead:
  // this Sync only clears the dirty bits so Hydra's lifecycle is satisfied. The
  // Rprim still exists (so Hydra creates + tracks the mesh prim); it just emits
  // nothing — StageWalker owns ingest.
  void Sync(HdSceneDelegate* /*sceneDelegate*/, HdRenderParam* /*renderParam*/,
            HdDirtyBits* dirtyBits, TfToken const& /*reprToken*/) override {
    *dirtyBits = HdChangeTracker::Clean;
  }

  [[nodiscard]] HdDirtyBits GetInitialDirtyBitsMask() const override {
    return HdChangeTracker::AllSceneDirtyBits;
  }

 protected:
  HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
  void _InitRepr(TfToken const& /*reprToken*/, HdDirtyBits* /*dirtyBits*/) override {}
};

// ---- Camera: Hydra camera -> GpuScene::SetCamera --------------------------
class HdPyxisCamera final : public HdCamera {
 public:
  explicit HdPyxisCamera(SdfPath const& primId) : HdCamera(primId) {}

  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
            HdDirtyBits* dirtyBits) override {
    // Keep the base sync so the render pass can read the resolved HdCamera
    // (it builds the active camera from the USD GfCamera / render-pass state).
    // §25.O.3 — the render pass owns camera selection; the per-prim SetCamera
    // translation is dead, so this only clears the dirty bits.
    HdCamera::Sync(sceneDelegate, renderParam, dirtyBits);
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

// Light Sprim: a no-op now that StageWalker owns light ingest. The Sprim type
// (distant / dome / rect / sphere / disk / cylinder) is still REGISTERED so the
// light isn't dropped from the render index, but the per-prim translation that
// built a LightDesc + called AddLight is dead — the render pass emits every
// light via StageWalker (byte-identical to the standalone, §25.O.3).
class HdPyxisLight final : public HdLight {
 public:
  // The kind/shape args are vestigial (CreateSprim still passes the Sprim type);
  // they no longer drive any translation. Kept so CreateSprim's call sites and
  // the supported-Sprim registration stay unchanged.
  enum class RectShape : uint8_t { NotRect, Rect, Disk, Sphere };

  HdPyxisLight(SdfPath const& primId, pyxis::LightDesc::Kind /*kind*/,
               RectShape /*rectShape*/ = RectShape::NotRect)
      : HdLight(primId) {}

  void Sync(HdSceneDelegate* /*sceneDelegate*/, HdRenderParam* /*renderParam*/,
            HdDirtyBits* dirtyBits) override {
    *dirtyBits = HdChangeTracker::Clean;
  }

  [[nodiscard]] HdDirtyBits GetInitialDirtyBitsMask() const override { return HdLight::AllDirty; }
};

// Minimal CPU-backed render buffer so HdEngine's AOV machinery doesn't crash.
// Pyxis renders into its own exportable image (PyxisEngine), so this buffer is
// the host-facing presentation target the render pass composites into.
class StubRenderBuffer final : public HdRenderBuffer {
 public:
  // `hgi` is the host render driver, non-null + GL only when the RFC 0008
  // GL-interop AOV path is active (the delegate passes its captured _hgi). When set
  // for a Float16Vec4 color buffer, Allocate creates a GL-backed AOV the host
  // samples via GetResource(); the render pass fills it from the exported image.
  StubRenderBuffer(SdfPath const& primId, Hgi* hgi) : HdRenderBuffer(primId), _hgi(hgi) {}

  bool Allocate(GfVec3i const& dimensions, HdFormat format, bool /*multiSampled*/) override {
    _width = static_cast<uint32_t>(dimensions[0]);
    _height = static_cast<uint32_t>(dimensions[1]);
    _format = format;
    _data.assign(static_cast<size_t>(_width) * _height * HdDataSizeOfFormat(format), 0);
    // GPU AOV (RFC 0008): a GL-backed color texture the host samples directly. Only
    // for the Float16Vec4 color buffer when a GL Hgi was captured. The render pass
    // copies the exported image into it each frame; falls back to the CPU _data path
    // (Map) otherwise / for other AOVs.
    DestroyGpuAov();
    if (_hgi != nullptr && format == HdFormatFloat16Vec4 && _width > 0 && _height > 0) {
      HgiTextureDesc desc;
      desc.debugName = "pyxis.color.aov";
      desc.dimensions = GfVec3i(static_cast<int>(_width), static_cast<int>(_height), 1);
      desc.format = HgiFormatFloat16Vec4;
      desc.layerCount = 1;
      desc.mipLevels = 1;
      desc.sampleCount = HgiSampleCount1;
      desc.type = HgiTextureType2D;
      desc.usage = HgiTextureUsageBitsColorTarget | HgiTextureUsageBitsShaderRead;
      _gpuAov = _hgi->CreateTexture(desc);
    }
    if (std::getenv("PYXIS_OMNI_DBG") != nullptr)
      std::fprintf(stderr, "PYXISDBG RenderBuffer::Allocate id='%s' %ux%u fmt=%d gpuAov=%d\n",
                   GetId().GetText(), _width, _height, static_cast<int>(format),
                   _gpuAov ? 1 : 0);
    return true;
  }

  // RFC 0008 GL-interop accessors (render pass fills the GL texture).
  [[nodiscard]] bool HasGpuAov() const { return static_cast<bool>(_gpuAov); }
  [[nodiscard]] uint32_t GpuAovGlId() const {
    return _gpuAov ? static_cast<HgiGLTexture*>(_gpuAov.Get())->GetTextureId() : 0u;
  }
  unsigned int GetWidth() const override { return _width; }
  unsigned int GetHeight() const override { return _height; }
  unsigned int GetDepth() const override { return 1; }
  HdFormat GetFormat() const override { return _format; }
  bool IsMultiSampled() const override { return false; }
  void* Map() override {
    if (!_loggedMap && std::getenv("PYXIS_OMNI_DBG") != nullptr) {
      _loggedMap = true;
      std::fprintf(stderr, "PYXISDBG RenderBuffer::Map() called by host (CPU AOV path)\n");
    }
    _mapped = true;
    return _data.data();
  }
  void Unmap() override { _mapped = false; }
  bool IsMapped() const override { return _mapped; }
  void Resolve() override {}
  bool IsConverged() const override { return true; }
  // RFC 0008: hand the host the GL-backed AOV texture so it samples our render on
  // the GPU (no CPU readback). Empty VtValue (CPU Map path) when no GPU AOV.
  VtValue GetResource(bool multiSampled) const override {
    if (!_loggedGetResource && std::getenv("PYXIS_OMNI_DBG") != nullptr) {
      _loggedGetResource = true;
      std::fprintf(stderr, "PYXISDBG RenderBuffer::GetResource(multiSampled=%d) gpuAov=%d\n",
                   multiSampled ? 1 : 0, _gpuAov ? 1 : 0);
    }
    if (_gpuAov)
      return VtValue(_gpuAov);
    return HdRenderBuffer::GetResource(multiSampled);
  }

 protected:
  void _Deallocate() override {
    _data.clear();
    _width = _height = 0;
    DestroyGpuAov();
  }

 private:
  void DestroyGpuAov() {
    if (_gpuAov && _hgi != nullptr)
      _hgi->DestroyTexture(&_gpuAov);
    _gpuAov = HgiTextureHandle();
  }

  Hgi* _hgi = nullptr;            // host render driver (GL only); null = CPU path.
  HgiTextureHandle _gpuAov;       // GL-backed color AOV the host samples (RFC 0008).
  uint32_t _width = 0;
  uint32_t _height = 0;
  HdFormat _format = HdFormatUNorm8Vec4;
  bool _mapped = false;
  bool _loggedMap = false;
  mutable bool _loggedGetResource = false;  // GetResource() is const
  std::vector<uint8_t> _data;
};

using pyxis::color::FloatToHalf;
using pyxis::color::HalfToFloat;

// Composite Pyxis's rendered color (RGBA16F, from PyxisEngine readback) into the
// host's bound color HdRenderBuffer. This is the host-facing presentation step —
// every display path (custom panel, usdview, UsdImagingGL, omni.hydra.pxr) reads
// this buffer. Supports the common AOV formats; converts from RGBA16F as needed.
//
// §25.O.3 color management: PyxisEngine exports the ACES-tonemapped color in
// LINEAR display space (raygen writes acesFilmic() with no OETF). We write that
// LINEAR value to the host's color AOV unchanged — the Hydra convention is that
// the color AOV is linear and the consumer applies the display OETF. EMPIRICALLY
// VERIFIED: the Kit pxr viewport (and usdview) run HdxColorCorrectionTask, whose
// measured transfer function on this AOV is exactly the sRGB OETF; pre-encoding
// sRGB here therefore double-applied it (washed-out / too bright vs the standalone
// pyxis.exe PNG). Writing linear makes the Kit viewport byte-match the standalone
// headless output. The standalone WorldLobbyHeadless harness does NOT consume this
// AOV (it reads ReadbackColorHdr directly and sRGB-encodes in WriteBmp), so §25.O.3
// parity is unaffected. PYXIS_OMNI_AOV_SRGB=1 restores the sRGB pre-encode for a
// hypothetical host that presents the AOV without any color correction.
void WritePyxisColorToAov(pyxis::hydra::PyxisEngine& engine, HdRenderBuffer* buffer,
                          std::vector<uint8_t>& src) {
  // Per-frame trace (composite / render-pass): gated on PYXIS_OMNI_TRACE, NOT
  // PYXIS_OMNI_DBG, so the default-on DBG keeps only occasional lifecycle prints
  // and doesn't flood every frame.
  const bool dbg = std::getenv("PYXIS_OMNI_TRACE") != nullptr;
  if (buffer == nullptr || buffer->GetWidth() == 0 || buffer->GetHeight() == 0) {
    if (dbg)
      std::fprintf(stderr, "PYXISDBG WriteAov: buffer null or 0-size\n");
    return;
  }
  // `src` is a caller-owned scratch buffer reused across frames (see the render
  // pass member) — ReadbackColorHdr::resize keeps its capacity, so no per-frame
  // heap alloc of the readback image (~16 MB @1080p).
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
  // The color AOV is LINEAR (Hydra convention): the host's HdxColorCorrectionTask
  // (Kit pxr viewport, usdview) applies the sRGB OETF on present. EMPIRICALLY
  // VERIFIED — the Kit viewport's measured display transfer function is exactly the
  // sRGB OETF (Kit_pixel = sRGB(written)); the readback here is the ACES-tonemapped
  // LINEAR color (range [0,1], not raw HDR). So we write that linear value VERBATIM
  // and let the host encode — pre-applying sRGB here double-encodes (washed-out, too
  // bright). NOTE: the standalone WorldLobbyHeadless harness does NOT read this AOV;
  // it reads ReadbackColorHdr directly and sRGB-encodes in its own WriteBmp, so it is
  // unaffected by this and §25.O.3 parity holds. PYXIS_OMNI_AOV_SRGB=1 forces the old
  // pre-encode for a hypothetical host that presents the AOV without color-correction.
  const bool encodeSrgb = std::getenv("PYXIS_OMNI_AOV_SRGB") != nullptr;
  auto encode = [encodeSrgb](float linear, int channel) -> float {
    // Single source of truth (also pinned by pyxis_unit_tests AovColorEncode).
    return pyxis::hydra::EncodeAovColorChannel(linear, channel, encodeSrgb);
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
    const UsdStageRefPtr stage = ResolveStage(renderParam);
    const bool ingestDbg = std::getenv("PYXIS_OMNI_DBG") != nullptr;
    if (stage && _engine->Scene() != nullptr) {
      _loggedNoStage = false;
      if (_walkedStage != stage) {
        // Trace the ingest decision ONCE per stage transition (not per frame).
        if (ingestDbg) {
          const bool hasHost = StageOf(renderParam) != nullptr;
          const size_t cacheStages = UsdUtilsStageCache::Get().GetAllStages().size();
          std::fprintf(stderr,
                       "PYXISDBG ingest: hostStage=%d cacheStages=%zu resolved=yes rootLayer=%s\n",
                       hasHost ? 1 : 0, cacheStages,
                       stage->GetRootLayer()->GetIdentifier().c_str());
        }
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
          _walkedStage = stage;
          if (ingestDbg) {
            const pyxis::usd_ingest::IngestStats& stats = result.Stats();
            std::fprintf(stderr,
                         "PYXISDBG WalkStage: meshes=%u instances=%u materials=%u lights=%u "
                         "cameras=%u skipped=%u\n",
                         stats.meshesEmitted, stats.instancesEmitted, stats.materialsEmitted,
                         stats.lightsEmitted, stats.camerasEmitted, stats.skipped);
          }
        }
      }
    } else if (ingestDbg && !_loggedNoStage) {
      _loggedNoStage = true;  // print once per no-stage episode, not every frame.
      std::fprintf(stderr, "PYXISDBG ingest: NO STAGE -> empty scene (Kit stage not in "
                           "UsdUtilsStageCache?). engineScene=%d\n",
                   _engine->Scene() != nullptr ? 1 : 0);
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
          stage = ResolveStage(renderParam);  // host stage, or Kit's cached stage.
          stageToWorld = ResolveStageToWorld(renderParam, stage);
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

    // Q3 OpenPBR feature gates — re-read the six pyxis:openpbr* render
    // settings EVERY frame (an Init-time read would miss later carb ->
    // SetRenderSetting updates from the Render Settings panel) and push the
    // composed mask into the engine; it lands in the RenderSettings the next
    // RenderFrame builds, so a panel toggle takes effect same-frame. All-on
    // (0x3F) when the settings are absent — usdview / parity stays reference.
    if (const HdRenderIndex* renderIndex = GetRenderIndex()) {
      if (const auto* pyxisDelegate =
              dynamic_cast<const HdPyxisRenderDelegate*>(renderIndex->GetRenderDelegate())) {
        _engine->SetOpenPbrFeatureMask(pyxisDelegate->ReadOpenPbrFeatureMask());
        // Auto-exposure (pyxis:autoExposure / pyxis:autoExposureKey) — same
        // per-frame re-read so a Render Settings panel toggle takes effect
        // same-frame.
        _engine->SetAutoExposure(pyxisDelegate->ReadAutoExposure(),
                                 pyxisDelegate->ReadAutoExposureKey());
      }
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
          // RFC 0008 direct GPU AOV: if the color buffer is GL-backed (Kit/HgiGL),
          // import the exported image into GL and copy it into the AOV texture — no
          // CPU readback. Falls back to WritePyxisColorToAov (readback) otherwise or
          // if any GL-interop step fails.
          auto* gpuBuf = dynamic_cast<StubRenderBuffer*>(binding.renderBuffer);
          const pyxis::ExportedImage& exp = _engine->ExportedColor();
          bool didGpuPath = false;
          if (gpuBuf != nullptr && gpuBuf->HasGpuAov() && exp.texture != nullptr
              && exp.memoryHandle != nullptr) {
            _engine->WaitRenderComplete();  // shared image holds finished pixels.
            const uint32_t srcTex = _glInterop.ImportExportedImage(
                exp.memoryHandle, exp.allocationSize, exp.width, exp.height, /*optimalTiling*/ true);
            if (srcTex != 0u)
              didGpuPath = _glInterop.CopyImportedInto(gpuBuf->GpuAovGlId(), exp.width, exp.height);
          }
          if (!didGpuPath)
            WritePyxisColorToAov(*_engine, binding.renderBuffer, _readbackScratch);
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
  // PYXIS_OMNI_DBG: the "ingest: ..." trace is interesting only on a stage
  // transition, not every frame. Remember whether we last logged the no-stage
  // state so that branch prints once per episode instead of flooding.
  bool _loggedNoStage = false;
  // RFC 0008: Vulkan->GL import for the direct GPU color AOV (HgiGL host). Loads its
  // GL entry points lazily in Kit's GL context; no-op/unused on the readback path.
  pyxis::hydra::GlVkInterop _glInterop;
  // Reused scratch for the per-frame color readback (WritePyxisColorToAov) so the
  // ~16 MB @1080p readback buffer is allocated once, not every frame.
  std::vector<uint8_t> _readbackScratch;
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

uint32_t HdPyxisRenderDelegate::ReadOpenPbrFeatureMask() const {
  // Per-token rows; tokens match render_settings.py's carb paths via the
  // established <root>/<token-colon-as-slash>/value convention. Type-coerce
  // like ReadPersistEngineSetting — carb may deliver bool / int / string.
  struct FeatureRow {
    TfToken token;
    uint32_t bit;
  };
  static const FeatureRow ROWS[] = {
      {TfToken("pyxis:openpbrCoat"), OPENPBR_FEATURE_COAT},
      {TfToken("pyxis:openpbrFuzz"), OPENPBR_FEATURE_FUZZ},
      {TfToken("pyxis:openpbrTransmission"), OPENPBR_FEATURE_TRANSMISSION},
      {TfToken("pyxis:openpbrSubsurface"), OPENPBR_FEATURE_SUBSURFACE},
      {TfToken("pyxis:openpbrAnisotropy"), OPENPBR_FEATURE_ANISOTROPY},
      {TfToken("pyxis:openpbrEonDiffuse"), OPENPBR_FEATURE_EON_DIFFUSE},
  };
  uint32_t mask = 0u;
  for (const FeatureRow& row : ROWS) {
    const VtValue value = GetRenderSetting(row.token);
    bool enabled = true;  // default ON (setting absent / unknown type)
    if (value.IsHolding<bool>())
      enabled = value.UncheckedGet<bool>();
    else if (value.IsHolding<int>())
      enabled = value.UncheckedGet<int>() != 0;
    else if (value.IsHolding<std::string>()) {
      const std::string& str = value.UncheckedGet<std::string>();
      enabled = !(str == "0" || str == "false" || str == "False");
    }
    if (enabled)
      mask |= row.bit;
  }
  return mask;
}

bool HdPyxisRenderDelegate::ReadAutoExposure() const {
  const VtValue value = GetRenderSetting(TfToken("pyxis:autoExposure"));
  if (value.IsHolding<bool>())
    return value.UncheckedGet<bool>();
  if (value.IsHolding<int>())
    return value.UncheckedGet<int>() != 0;
  if (value.IsHolding<std::string>()) {
    const std::string& str = value.UncheckedGet<std::string>();
    return (str == "1" || str == "true" || str == "True");
  }
  return false;  // default OFF (parity-stable reference)
}

float HdPyxisRenderDelegate::ReadAutoExposureKey() const {
  const VtValue value = GetRenderSetting(TfToken("pyxis:autoExposureKey"));
  if (value.IsHolding<float>())
    return value.UncheckedGet<float>();
  if (value.IsHolding<double>())
    return static_cast<float>(value.UncheckedGet<double>());
  if (value.IsHolding<int>())
    return static_cast<float>(value.UncheckedGet<int>());
  return 0.18f;  // photographic 18% grey
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
  // Q3 OpenPBR feature gates — one BOOL per OPENPBR_FEATURE_* bit, all default
  // ON (the parity-stable reference look). The Render Settings panel rows
  // (render_settings.py) bind carb paths to these tokens; the render pass
  // re-composes the mask per-frame via ReadOpenPbrFeatureMask.
  _settingDescriptors.push_back(
      {"OpenPBR: coat layer", TfToken("pyxis:openpbrCoat"), VtValue(true)});
  _settingDescriptors.push_back(
      {"OpenPBR: fuzz (sheen)", TfToken("pyxis:openpbrFuzz"), VtValue(true)});
  _settingDescriptors.push_back(
      {"OpenPBR: transmission", TfToken("pyxis:openpbrTransmission"), VtValue(true)});
  _settingDescriptors.push_back(
      {"OpenPBR: subsurface", TfToken("pyxis:openpbrSubsurface"), VtValue(true)});
  _settingDescriptors.push_back(
      {"OpenPBR: anisotropy", TfToken("pyxis:openpbrAnisotropy"), VtValue(true)});
  _settingDescriptors.push_back(
      {"OpenPBR: energy-preserving diffuse (EON)", TfToken("pyxis:openpbrEonDiffuse"),
       VtValue(true)});
  // Auto-exposure — derive the exposure from the frame's average luminance so a
  // scene with hot lights + no authored camera exposure displays without
  // clipping to white. Default OFF (parity-stable); the render pass re-reads it
  // per-frame via ReadAutoExposure and pushes it into the engine.
  _settingDescriptors.push_back(
      {"Auto exposure", TfToken("pyxis:autoExposure"), VtValue(false)});
  _settingDescriptors.push_back(
      {"Auto-exposure target grey", TfToken("pyxis:autoExposureKey"), VtValue(0.18f)});

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
  // color: half-float RGBA holding the LINEAR (ACES-tonemapped) color. Hydra
  // convention: the color AOV is linear and the host (Kit pxr viewport / usdview,
  // via HdxColorCorrectionTask) applies the sRGB OETF on present. Float16 keeps
  // precision in the darks before that OETF. (WritePyxisColorToAov writes linear,
  // NOT sRGB — see the note there; empirically the Kit viewport's display transfer
  // function is exactly sRGB, so pre-encoding would double-apply it.)
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
  _renderParam->SetStageToWorld(BuildStageToWorld(metersPerUnit, stageIsZUp));
}

void HdPyxisRenderDelegate::SetStage(const UsdStageRefPtr& stage) noexcept {
  if (_renderParam != nullptr)
    _renderParam->SetStage(stage);
}

HdResourceRegistrySharedPtr HdPyxisRenderDelegate::GetResourceRegistry() const {
  return _resourceRegistry;
}

void HdPyxisRenderDelegate::SetDrivers(HdDriverVector const& drivers) {
  // Probe (PYXIS_OMNI_DBG): what Hgi backend does the host (omni.hydra.pxr) run, and
  // can we reach a native texture handle? Determines whether a GPU-direct color AOV
  // (no CPU readback) is reachable — see RFC 0008.
  // Filter by the held type (Hgi*) rather than HgiTokens->renderDriver to avoid
  // linking usd_hgi for the token data symbol; GetAPIName() is a vtable call.
  // Capture the Hgi only when it is GL — that is the backend our GL-interop AOV path
  // (RFC 0008) targets; for any other backend we keep the CPU readback path.
  for (const HdDriver* driver : drivers) {
    if (driver == nullptr || !driver->driver.IsHolding<Hgi*>())
      continue;
    Hgi* hgi = driver->driver.UncheckedGet<Hgi*>();
    if (hgi == nullptr)
      continue;
    const bool isGl = hgi->GetAPIName().GetString() == "OpenGL";
    if (isGl)
      _hgi = hgi;
    if (std::getenv("PYXIS_OMNI_DBG") != nullptr)
      std::fprintf(stderr, "PYXISDBG SetDrivers: host Hgi API = '%s' (hgi=%p) glInterop=%d\n",
                   hgi->GetAPIName().GetText(), static_cast<void*>(hgi), isGl ? 1 : 0);
  }
  HdRenderDelegate::SetDrivers(drivers);
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
    return new StubRenderBuffer(bprimId, _hgi);  // _hgi (GL) enables the direct AOV
  return nullptr;
}
HdBprim* HdPyxisRenderDelegate::CreateFallbackBprim(TfToken const& typeId) {
  if (typeId == HdPrimTypeTokens->renderBuffer)
    return new StubRenderBuffer(SdfPath::EmptyPath(), _hgi);
  return nullptr;
}
void HdPyxisRenderDelegate::DestroyBprim(HdBprim* bprim) { delete bprim; }

void HdPyxisRenderDelegate::CommitResources(HdChangeTracker* /*tracker*/) {
  // GpuScene commit happens inside PyxisEngine::RenderFrame (on the render
  // pass) so it lands on the same command list as the render.
}

PXR_NAMESPACE_CLOSE_SCOPE
