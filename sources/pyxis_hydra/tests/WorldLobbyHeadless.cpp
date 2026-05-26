// RFC 0006 — headless Omniverse render of a REAL scene (World Lobby) through the
// Pyxis Hydra delegate, driven by the GENERIC Hydra path (no omni.hydra.pxr, no
// compatibilityMode):
//
//   UsdStage::Open(World_Lobby.usd)  (Kit's nv-usd 25.11)
//     -> UsdImagingCreateSceneIndices  (binding resolution + flatten)
//       -> HdRenderIndex + HdPyxisRenderDelegate
//         -> HdEngine::Execute -> HdPyxisMesh/Camera/Light::Sync -> GpuScene
//           -> render pass -> PyxisEngine::RenderFrame -> composite to color AOV
//
// This proves Pyxis works in Omniverse on a production scene depending ONLY on
// Hydra (hd/ + usdImaging/) — exactly the decoupling target: Pyxis is just a USD
// HdRendererPlugin any Hydra-2 host can drive, independent of which viewport
// engine (rtx / pxr / …) the host happens to use. Writes a viewable BMP.
//
//   pyxis_hydra_omni_lobby <World_Lobby.usd> [cameraPrimPath] [width height]
//
// Exit 0 = rendered non-black; non-zero = failure (see messages).

#include "PyxisEngine.h"
#include "PyxisHydraHost.h"

#include <Pyxis/Platform/Color/ColorEncoding.h>

#include <pxr/base/gf/range3d.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// 24-bit BMP (bottom-up BGR). Caller guarantees width*3 is 4-aligned.
void WriteBmp(const char* path, const std::vector<uint8_t>& rgba16f, uint32_t w, uint32_t h) {
  if (rgba16f.size() < size_t(w) * h * 8 || w == 0 || h == 0)
    return;
  const uint32_t rowBytes = w * 3;
  const uint32_t dataSize = rowBytes * h;
  const uint32_t fileSize = 54 + dataSize;
  std::FILE* file = nullptr;
  if (fopen_s(&file, path, "wb") != 0 || file == nullptr)
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
  for (int y = int(h) - 1; y >= 0; --y) {
    const uint16_t* srcRow = src + size_t(y) * w * 4;
    for (uint32_t x = 0; x < w; ++x) {
      auto chan = [&](int c) {
        // PyxisEngine exports the ACES-tonemapped color in LINEAR display space
        // (raygen writes acesFilmic() with no OETF). For a display-correct BMP —
        // and to match the standalone pyxis.exe PNG, which sRGB-encodes the same
        // linear buffer — apply the sRGB OETF here (this mirrors what
        // WritePyxisColorToAov does for the Kit viewport). §25.O.3 parity.
        const float v = pyxis::color::LinearToSrgb(pyxis::color::HalfToFloat(srcRow[x * 4 + c]));
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
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <World_Lobby.usd> [cameraPrimPath] [width height]\n", argv[0]);
    return 2;
  }
  const std::string usdPath = argv[1];
  const std::string cameraArg = argc >= 3 ? argv[2] : "/World/Cameras/CamLobbyWide";
  uint32_t width = 1920, height = 1080;
  if (argc >= 5) {
    width = static_cast<uint32_t>(std::atoi(argv[3]));
    height = static_cast<uint32_t>(std::atoi(argv[4]));
  }
  // [frames] — progressive path-trace accumulation (default 1 = one noisy sample;
  // pass e.g. 256 to converge, matching the live viewport, for parity diffs).
  uint32_t frames = 1;
  if (argc >= 6)
    frames = static_cast<uint32_t>(std::atoi(argv[5]));

  // 1. Open the real scene through Kit's nv-usd (the Omniverse USD runtime).
  UsdStageRefPtr stage = UsdStage::Open(usdPath);
  if (!stage) {
    std::fprintf(stderr, "FAIL: could not open stage '%s'\n", usdPath.c_str());
    return 2;
  }

  // Resolve the camera: use the requested path if it is a camera; else fall back
  // to the first UsdGeomCamera on the stage (and list what's available).
  SdfPath cameraPath;
  if (UsdPrim p = stage->GetPrimAtPath(SdfPath(cameraArg)); p && p.IsA<UsdGeomCamera>())
    cameraPath = p.GetPath();
  if (cameraPath.IsEmpty()) {
    for (const UsdPrim& prim : stage->Traverse()) {
      if (prim.IsA<UsdGeomCamera>()) {
        std::printf("WorldLobbyHeadless: camera available: %s\n", prim.GetPath().GetText());
        if (cameraPath.IsEmpty())
          cameraPath = prim.GetPath();
      }
    }
  }
  if (cameraPath.IsEmpty()) {
    std::fprintf(stderr, "FAIL: no UsdGeomCamera found on stage\n");
    return 3;
  }
  std::printf("WorldLobbyHeadless: scene=%s camera=%s %ux%u\n", usdPath.c_str(),
              cameraPath.GetText(), width, height);

  // 2. The generic Pyxis Hydra host (delegate + HdRenderIndex + UsdImaging scene
  //    indices + render pass + color AOV). NO omni.hydra.pxr. NO compatibilityMode.
  PyxisHydraHost host(width, height);
  if (host.Engine() == nullptr) {
    std::fprintf(stderr, "FAIL: PyxisEngine did not initialise (no GPU / interop?).\n");
    return 4;
  }
  if (!host.IsValid()) {
    std::fprintf(stderr, "FAIL: PyxisHydraHost render stack did not initialise.\n");
    return 4;
  }

  // 3. Feed the stage + render through the hero camera.
  host.SetStage(stage);
  if (!host.Render(UsdTimeCode::Default(), cameraPath, frames)) {
    std::fprintf(stderr, "FAIL: PyxisHydraHost::Render returned false.\n");
    return 5;
  }

  const uint64_t meshCount = host.Engine()->LastMeshCount();
  const uint64_t instanceCount = host.Engine()->LastInstanceCount();
  const uint64_t materialCount = host.Engine()->LastMaterialCount();
  const uint64_t lightCount = host.Engine()->LastLightCount();

  std::vector<uint8_t> pixels;
  uint32_t rbW = 0, rbH = 0;
  const bool readback = host.Engine()->ReadbackColorHdr(pixels, rbW, rbH);
  uint64_t nonZero = 0;
  if (readback) {
    const auto* ep = reinterpret_cast<const uint16_t*>(pixels.data());
    const uint64_t total = uint64_t(rbW) * rbH;
    for (uint64_t p = 0; p < total; ++p)
      if (ep[p * 4] || ep[p * 4 + 1] || ep[p * 4 + 2])
        ++nonZero;
    std::string dir(argv[0]);
    const auto slash = dir.find_last_of("\\/");
    dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash);
    const std::string bmp = dir + "\\pyxis_world_lobby.bmp";
    WriteBmp(bmp.c_str(), pixels, rbW, rbH);
    std::printf("WorldLobbyHeadless: wrote %s\n", bmp.c_str());
  }

  std::printf("WorldLobbyHeadless: meshes=%llu instances=%llu materials=%llu lights=%llu "
              "readback=%d (%ux%u) nonZeroPixels=%llu (%.1f%%)\n",
              (unsigned long long)meshCount, (unsigned long long)instanceCount,
              (unsigned long long)materialCount, (unsigned long long)lightCount, readback ? 1 : 0,
              rbW, rbH, (unsigned long long)nonZero,
              readback ? 100.0 * double(nonZero) / (double(rbW) * rbH) : 0.0);

  if (meshCount < 1 || instanceCount < 1) {
    std::fprintf(stderr, "FAIL: World Lobby geometry did not reach GpuScene via the delegate.\n");
    return 6;
  }
  if (!readback || nonZero == 0) {
    std::fprintf(stderr, "FAIL: render is black.\n");
    return 7;
  }
  std::printf("PASS: World Lobby rendered through Pyxis via the generic Hydra path "
              "(no omni.hydra.pxr, no compatibilityMode).\n");
  return 0;
}
