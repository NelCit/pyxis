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

#include "HdPyxisRenderParam.h"
#include "PyxisEngine.h"

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/LightDesc.h>  // LightDesc::Kind — Sprim registration
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
