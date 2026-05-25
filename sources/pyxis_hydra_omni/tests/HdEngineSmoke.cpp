// RFC 0004 Stage 3 (C4-full) — headless HdEngine smoke for the Pyxis delegate.
//
// Drives a USD stage through HdPyxisOmniRenderDelegate exactly as a Kit viewport
// would (UsdImagingStageSceneIndex -> HdRenderIndex -> HdEngine), and verifies
// the FULL chain end-to-end on real hardware WITHOUT Kit:
//   HdEngine -> HdPyxisOmniMesh::Sync -> GpuScene::CreateMesh/AppendInstance
//            -> render pass -> PyxisEngine::RenderFrame -> CommitResources (TLAS)
// Success = the synced mesh shows up in the committed scene (instanceCount >= 1)
// and a frame reads back. Exit 0 = pass, non-zero = fail.

#include "PyxisEngine.h"
#include "PyxisHydraHost.h"

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

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

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

  // 2. The reusable Pyxis Hydra host (delegate + render index + render pass +
  //    color AOV). The same class the in-Kit viewport panel drives (RFC 0004
  //    Option A) — this test is just its first client.
  constexpr uint32_t kRbW = 1280, kRbH = 720;
  PyxisHydraHost host(kRbW, kRbH);
  if (host.Engine() == nullptr) {
    std::fprintf(stderr, "FAIL: PyxisEngine did not initialise (no GPU / interop?).\n");
    return 2;
  }
  if (!host.IsValid()) {
    std::fprintf(stderr, "FAIL: PyxisHydraHost render stack did not initialise.\n");
    return 3;
  }

  // 3. Feed the stage + 4. drive a frame, framing through /cam.
  host.SetStage(stage);
  if (!host.Render(UsdTimeCode::Default(), SdfPath("/cam"))) {
    std::fprintf(stderr, "FAIL: PyxisHydraHost::Render returned false.\n");
    return 3;
  }
  HdRenderBuffer* colorBuffer = host.ColorBuffer();

  // 5. Verify the FSD mesh adapter fed geometry through to Pyxis + it rendered.
  const uint64_t instanceCount = host.Engine()->LastInstanceCount();
  const uint64_t meshCount = host.Engine()->LastMeshCount();
  const uint64_t materialCount = host.Engine()->LastMaterialCount();
  const uint64_t lightCount = host.Engine()->LastLightCount();
  std::vector<uint8_t> pixels;
  uint32_t width = 0;
  uint32_t height = 0;
  const bool readback = host.Engine()->ReadbackColorHdr(pixels, width, height);
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
  bool aovMatchesEngine = false;  // composited AOV == sRGB(flip(engine render))?
  if (const auto* aov = static_cast<const uint16_t*>(colorBuffer->Map())) {
    const uint32_t bufW = colorBuffer->GetWidth();
    const uint32_t bufH = colorBuffer->GetHeight();
    const uint64_t total = uint64_t(bufW) * bufH;
    for (uint64_t p = 0; p < total; ++p) {
      const float r = HalfToFloatSmoke(aov[p * 4 + 0]);
      const float g = HalfToFloatSmoke(aov[p * 4 + 1]);
      const float b = HalfToFloatSmoke(aov[p * 4 + 2]);
      if (r != 0.f || g != 0.f || b != 0.f)
        ++nonZeroAovPixels;
    }
    // WritePyxisColorToAov is the host PRESENTATION transform: it sRGB-encodes RGB
    // (so Kit/usdview present display-ready values matching RTX) and flips rows
    // (Vulkan top-down -> GL bottom-up). Verify the AOV is EXACTLY that transform
    // of the engine's linear readback (the §25.O.3 display contract), within the
    // half<->float round-trip epsilon.
    auto srgb = [](float v) {
      v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
      return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    };
    if (readback && pixels.size() == total * 8 && width == bufW && height == bufH) {
      const auto* eng = reinterpret_cast<const uint16_t*>(pixels.data());
      aovMatchesEngine = true;
      for (uint32_t y = 0; y < bufH && aovMatchesEngine; ++y) {
        const uint32_t srcY = bufH - 1 - y;  // engine top-down -> AOV bottom-up
        for (uint32_t x = 0; x < bufW; ++x) {
          for (int c = 0; c < 3; ++c) {
            const float a = HalfToFloatSmoke(aov[(uint64_t(y) * bufW + x) * 4 + c]);
            const float e = srgb(HalfToFloatSmoke(eng[(uint64_t(srcY) * bufW + x) * 4 + c]));
            if (std::fabs(a - e) > 0.01f) {
              aovMatchesEngine = false;
              break;
            }
          }
          if (!aovMatchesEngine)
            break;
        }
      }
    }
    colorBuffer->Unmap();
  }

  std::printf(
      "HdEngineSmoke: meshCount=%llu instanceCount=%llu materialCount=%llu lightCount=%llu "
      "readback=%d (%ux%u) aovNonZeroPixels=%llu aovMatchesEngine=%d\n",
      static_cast<unsigned long long>(meshCount), static_cast<unsigned long long>(instanceCount),
      static_cast<unsigned long long>(materialCount), static_cast<unsigned long long>(lightCount),
      readback ? 1 : 0, width, height, static_cast<unsigned long long>(nonZeroAovPixels),
      aovMatchesEngine ? 1 : 0);

  // host destructor tears down the render index + delegate + PyxisEngine.

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
    std::fprintf(stderr,
                 "FAIL: composited AOV is not the sRGB-encoded, row-flipped engine render "
                 "(WritePyxisColorToAov display contract).\n");
    return 9;
  }
  std::printf("PASS: HdEngine drove the Pyxis delegate end-to-end + composited into the host AOV.\n");
  return 0;
}
