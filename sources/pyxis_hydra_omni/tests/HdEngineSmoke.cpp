// RFC 0004 Stage 3 (C4-full) — headless HdEngine smoke for the Pyxis delegate.
//
// Drives a USD stage through HdPyxisOmniRenderDelegate exactly as a Kit viewport
// would (UsdImagingStageSceneIndex -> HdRenderIndex -> HdEngine), and verifies
// the FULL chain end-to-end on real hardware WITHOUT Kit:
//   HdEngine -> HdPyxisOmniMesh::Sync -> GpuScene::CreateMesh/AppendInstance
//            -> render pass -> PyxisEngine::RenderFrame -> CommitResources (TLAS)
// Success = the synced mesh shows up in the committed scene (instanceCount >= 1)
// and a frame reads back. Exit 0 = pass, non-zero = fail.

#include "HdPyxisOmniRenderDelegate.h"
#include "PyxisEngine.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Minimal task: Sync registers the render pass's collection (so SyncAll syncs
// the rprims), Execute drives the pass (-> PyxisEngine::RenderFrame).
class RenderTask final : public HdTask {
 public:
  RenderTask(HdRenderPassSharedPtr pass, HdRenderPassAovBindingVector bindings, HdCamera* camera)
      : HdTask(SdfPath::EmptyPath()),
        _pass(std::move(pass)),
        _bindings(std::move(bindings)),
        _camera(camera) {}
  void Sync(HdSceneDelegate*, HdTaskContext*, HdDirtyBits* bits) override {
    _pass->Sync();
    *bits = HdChangeTracker::Clean;
  }
  void Prepare(HdTaskContext*, HdRenderIndex*) override {}
  void Execute(HdTaskContext*) override {
    auto state = std::make_shared<HdRenderPassState>();
    state->SetAovBindings(_bindings);  // host binds the color render buffer here.
    if (_camera) {
      state->SetCamera(_camera);  // triggers camera sync -> GpuScene::SetCamera.
      state->SetViewport(GfVec4d(0, 0, 1280, 720));
    }
    _pass->Execute(state, GetRenderTags());
  }
  const TfTokenVector& GetRenderTags() const override {
    static const TfTokenVector tags = {HdRenderTagTokens->geometry};
    return tags;
  }

 private:
  HdRenderPassSharedPtr _pass;
  HdRenderPassAovBindingVector _bindings;
  HdCamera* _camera;
};

// Write an RGBA16F readback as a 24-bit BMP (no deps) so "run it" yields a
// viewable image. Bottom-up BGR rows; 1280*3 is 4-aligned so no row padding.
void WriteBmp(const char* path, const std::vector<uint8_t>& rgba16f, uint32_t w, uint32_t h);

float HalfToFloatSmoke(uint16_t half) {
  const uint32_t sign = (half >> 15) & 1u, exp = (half >> 10) & 0x1Fu, mant = half & 0x3FFu;
  uint32_t bits;
  if (exp == 0) {
    bits = mant ? 0 : (sign << 31);  // treat denormals as ~0 for the check.
  } else if (exp == 0x1F) {
    bits = (sign << 31) | (0xFFu << 23) | (mant << 13);
  } else {
    bits = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
  }
  float out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

void WriteBmp(const char* path, const std::vector<uint8_t>& rgba16f, uint32_t w, uint32_t h) {
  if (rgba16f.size() < size_t(w) * h * 8 || w == 0 || h == 0)
    return;
  const uint32_t rowBytes = w * 3;             // 1280*3 = 3840 (4-aligned).
  const uint32_t dataSize = rowBytes * h;
  const uint32_t fileSize = 54 + dataSize;
  std::FILE* file = std::fopen(path, "wb");
  if (!file)
    return;
  uint8_t hdr[54] = {};
  hdr[0] = 'B'; hdr[1] = 'M';
  std::memcpy(hdr + 2, &fileSize, 4);
  const uint32_t dataOff = 54; std::memcpy(hdr + 10, &dataOff, 4);
  const uint32_t infoSize = 40; std::memcpy(hdr + 14, &infoSize, 4);
  std::memcpy(hdr + 18, &w, 4); std::memcpy(hdr + 22, &h, 4);
  const uint16_t planes = 1; std::memcpy(hdr + 26, &planes, 2);
  const uint16_t bpp = 24; std::memcpy(hdr + 28, &bpp, 2);
  std::memcpy(hdr + 34, &dataSize, 4);
  std::fwrite(hdr, 1, 54, file);
  const auto* src = reinterpret_cast<const uint16_t*>(rgba16f.data());
  std::vector<uint8_t> row(rowBytes);
  for (int y = int(h) - 1; y >= 0; --y) {  // BMP rows are bottom-up.
    const uint16_t* s = src + size_t(y) * w * 4;
    for (uint32_t x = 0; x < w; ++x) {
      auto chan = [&](int c) {
        float v = HalfToFloatSmoke(s[x * 4 + c]);
        v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
        return static_cast<uint8_t>(v * 255.f + 0.5f);
      };
      row[x * 3 + 0] = chan(2);  // B
      row[x * 3 + 1] = chan(1);  // G
      row[x * 3 + 2] = chan(0);  // R
    }
    std::fwrite(row.data(), 1, rowBytes, file);
  }
  std::fclose(file);
}

}  // namespace

int main(int argc, char** argv) {
  // 1. A USD stage with one triangle mesh.
  UsdStageRefPtr stage = UsdStage::CreateInMemory();
  UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/tri"));
  VtVec3fArray points = {GfVec3f(0, 0, 2), GfVec3f(1, 0, 2), GfVec3f(0, 1, 2)};
  mesh.CreatePointsAttr().Set(points);
  mesh.CreateFaceVertexCountsAttr().Set(VtIntArray{3});
  mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray{0, 1, 2});

  // A UsdPreviewSurface material bound to the triangle (exercises the material
  // adapter -> GpuScene::AcquireMaterial).
  UsdShadeMaterial material = UsdShadeMaterial::Define(stage, SdfPath("/mat"));
  UsdShadeShader shader = UsdShadeShader::Define(stage, SdfPath("/mat/pbr"));
  shader.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
  shader.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f)
      .Set(GfVec3f(0.2f, 0.6f, 0.9f));
  shader.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float).Set(0.4f);
  material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), TfToken("surface"));
  UsdShadeMaterialBindingAPI::Apply(mesh.GetPrim()).Bind(material);

  // A distant light (exercises the light adapter -> GpuScene::AddLight).
  UsdLuxDistantLight light = UsdLuxDistantLight::Define(stage, SdfPath("/sun"));
  light.CreateIntensityAttr(VtValue(3.0f));

  // A camera at z=5 looking -Z (USD default), framing the triangle at z=2.
  UsdGeomCamera camera = UsdGeomCamera::Define(stage, SdfPath("/cam"));
  UsdGeomXformCommonAPI(camera).SetTranslate(GfVec3d(0.3, 0.3, 5.0));

  // 2. Render index + the Pyxis delegate.
  auto* delegate = new HdPyxisOmniRenderDelegate();
  if (delegate->Engine() == nullptr) {
    std::fprintf(stderr, "FAIL: PyxisEngine did not initialise (no GPU / interop?).\n");
    return 2;
  }
  HdRenderIndex* index = HdRenderIndex::New(delegate, HdDriverVector{});
  if (index == nullptr) {
    std::fprintf(stderr, "FAIL: HdRenderIndex::New returned null.\n");
    return 3;
  }

  // 3. Feed the stage in via the FULL Hydra-2 scene-index chain (binding
  //    resolution + flattening + ...). The bare UsdImagingStageSceneIndex does
  //    NOT resolve material bindings, so GetMaterialId would return empty.
  UsdImagingCreateSceneIndicesInfo info;
  info.stage = stage;
  const UsdImagingSceneIndices sceneIndices = UsdImagingCreateSceneIndices(info);
  sceneIndices.stageSceneIndex->SetTime(UsdTimeCode::Default());
  index->InsertSceneIndex(sceneIndices.finalSceneIndex, SdfPath::AbsoluteRootPath());
  sceneIndices.stageSceneIndex->ApplyPendingUpdates();

  // 4. Drive a frame.
  HdRprimCollection collection(HdTokens->geometry, HdReprSelector(HdReprTokens->hull));
  HdRenderPassSharedPtr pass = delegate->CreateRenderPass(index, collection);

  // Host-side color render buffer (RGBA16F, matches PyxisEngine's 1280x720) the
  // delegate must composite into — exactly what a viewport / usdview binds.
  constexpr uint32_t kRbW = 1280, kRbH = 720;
  auto* colorBuffer = static_cast<HdRenderBuffer*>(
      delegate->CreateBprim(HdPrimTypeTokens->renderBuffer, SdfPath("/pyxisColor")));
  colorBuffer->Allocate(GfVec3i(kRbW, kRbH, 1), HdFormatFloat16Vec4, /*multiSampled*/ false);
  HdRenderPassAovBinding colorBinding;
  colorBinding.aovName = HdAovTokens->color;
  colorBinding.renderBuffer = colorBuffer;

  // The camera sprim the scene index created (drives GpuScene::SetCamera when
  // the render-pass state references it).
  auto* hdCamera =
      static_cast<HdCamera*>(index->GetSprim(HdPrimTypeTokens->camera, SdfPath("/cam")));

  HdEngine engine;
  HdTaskSharedPtrVector tasks = {std::make_shared<RenderTask>(
      pass, HdRenderPassAovBindingVector{colorBinding}, hdCamera)};
  engine.Execute(index, &tasks);

  // 5. Verify the FSD mesh adapter fed geometry through to Pyxis + it rendered.
  const uint64_t instanceCount = delegate->Engine()->LastInstanceCount();
  const uint64_t meshCount = delegate->Engine()->LastMeshCount();
  const uint64_t materialCount = delegate->Engine()->LastMaterialCount();
  const uint64_t lightCount = delegate->Engine()->LastLightCount();
  std::vector<uint8_t> pixels;
  uint32_t width = 0;
  uint32_t height = 0;
  const bool readback = delegate->Engine()->ReadbackColorHdr(pixels, width, height);
  // Diagnostic: non-zero pixels in the engine's OWN render (distinguishes a
  // black render from a broken composite).
  uint64_t engineNonZero = 0;
  if (readback) {
    const auto* ep = reinterpret_cast<const uint16_t*>(pixels.data());
    const uint64_t total = uint64_t(width) * height;
    for (uint64_t p = 0; p < total; ++p)
      if (ep[p * 4] || ep[p * 4 + 1] || ep[p * 4 + 2])
        ++engineNonZero;
  }
  std::printf("HdEngineSmoke: engineNonZeroPixels=%llu\n",
              static_cast<unsigned long long>(engineNonZero));

  // Write a viewable image next to the exe so "run it" produces something to open.
  if (readback) {
    std::string dir(argv[0]);
    const auto slash = dir.find_last_of("\\/");
    dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash);
    const std::string bmp = dir + "\\pyxis_smoke.bmp";
    WriteBmp(bmp.c_str(), pixels, width, height);
    std::printf("HdEngineSmoke: wrote %s\n", bmp.c_str());
  }

  // Verify the delegate COMPOSITED Pyxis's color into the host's bound render
  // buffer (the presentation path a viewport/usdview uses) — count non-zero px.
  uint64_t nonZeroAovPixels = 0;
  bool aovMatchesEngine = false;  // composited AOV byte-identical to engine render?
  if (const auto* aov = static_cast<const uint16_t*>(colorBuffer->Map())) {
    const uint64_t total = uint64_t(colorBuffer->GetWidth()) * colorBuffer->GetHeight();
    for (uint64_t p = 0; p < total; ++p) {
      const float r = HalfToFloatSmoke(aov[p * 4 + 0]);
      const float g = HalfToFloatSmoke(aov[p * 4 + 1]);
      const float b = HalfToFloatSmoke(aov[p * 4 + 2]);
      if (r != 0.f || g != 0.f || b != 0.f)
        ++nonZeroAovPixels;
    }
    // The Float16Vec4 AOV (same dims/format as the engine readback) must be a
    // faithful copy of what PyxisEngine rendered.
    if (readback && pixels.size() == total * 8)
      aovMatchesEngine = std::memcmp(aov, pixels.data(), pixels.size()) == 0;
    colorBuffer->Unmap();
  }

  std::printf(
      "HdEngineSmoke: meshCount=%llu instanceCount=%llu materialCount=%llu lightCount=%llu "
      "readback=%d (%ux%u) aovNonZeroPixels=%llu aovMatchesEngine=%d\n",
      static_cast<unsigned long long>(meshCount), static_cast<unsigned long long>(instanceCount),
      static_cast<unsigned long long>(materialCount), static_cast<unsigned long long>(lightCount),
      readback ? 1 : 0, width, height, static_cast<unsigned long long>(nonZeroAovPixels),
      aovMatchesEngine ? 1 : 0);

  delete index;
  delete delegate;

  if (meshCount < 1 || instanceCount < 1) {
    std::fprintf(stderr, "FAIL: stage geometry did not reach GpuScene via the delegate.\n");
    return 4;
  }
  if (materialCount < 1) {
    std::fprintf(stderr, "FAIL: bound material did not reach GpuScene via the delegate.\n");
    return 6;
  }
  if (lightCount < 1) {
    std::fprintf(stderr, "FAIL: distant light did not reach GpuScene via the delegate.\n");
    return 7;
  }
  if (!readback) {
    std::fprintf(stderr, "FAIL: no frame read back from the engine.\n");
    return 5;
  }
  if (nonZeroAovPixels < 1) {
    std::fprintf(stderr,
                 "FAIL: delegate did not composite Pyxis color into the host render buffer.\n");
    return 8;
  }
  if (!aovMatchesEngine) {
    std::fprintf(stderr, "FAIL: composited AOV is not byte-identical to the engine render.\n");
    return 9;
  }
  std::printf("PASS: HdEngine drove the Pyxis delegate end-to-end + composited into the host AOV.\n");
  return 0;
}
